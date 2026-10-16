#include "chat_bot.h"
#include "session.h"
#include <algorithm>
#include <cmath>
#include <exception>
namespace tools {
namespace {
constexpr int kMaxPeers = 20000;                 // peers whose rates are remembered at once
constexpr qint64 kForgetPeerMs = 10 * 60 * 1000; // a peer quiet this long has its full rate again
constexpr qint64 kSaySpacingMs = 60 * 1000;      // how often the same peer's limit is spoken of
// What a delivery ends in: nothing more happens to the message after these.
bool final(const QString &state) {
    return state == "acknowledged" || state == "failed" || state == "expired" ||
           state == "cancelled";
}
} // namespace
ChatBot::ChatBot(bm::Session &session, Handler handler, ChatBotOptions options, Log log)
    : session_(session), handler_(std::move(handler)), options_(options), log_(std::move(log)) {}
void ChatBot::say(const QString &line) const {
    if (log_)
        log_(line);
}
QString ChatBot::brief(const QString &address) {
    return address.left(12) + "…";
}
QString ChatBot::oneLine(const QString &text) {
    const auto line = text.simplified();
    return line.size() > 80 ? line.left(79) + "…" : line;
}
ChatBot::Stats ChatBot::stats() const {
    auto result = stats_;
    result.working = int(jobs_.size());
    result.tracked = int(peers_.size() + sent_.size());
    return result;
}
void ChatBot::poll(qint64 now) {
    // What is due first, so a chat that was waiting out its delay makes room for the next; and
    // what is due again after taking up new ones, so with no delay a chat is answered at once.
    send(now);
    admit(now);
    send(now);
    watchStates(now);
}
// How many of wanted answers to peer may go now: what the peer's rate and the total rate allow,
// taken from both. Rates are token buckets: a burst, refilled by the minute.
int ChatBot::allowed(const QString &peer, int wanted, qint64 now) {
    if (wanted <= 0)
        return 0;
    const bool peerLimit = options_.ratePerMinute > 0, allLimit = options_.globalPerMinute > 0;
    if (!peerLimit && !allLimit)
        return wanted;
    const auto refill = [now](Bucket &bucket, int burst, double perMinute) {
        bucket.tokens = std::min<double>(
            burst, bucket.tokens + std::max<qint64>(0, now - bucket.last) / 60000.0 * perMinute);
        bucket.last = now;
    };
    int n = wanted;
    Bucket *mine = nullptr;
    if (peerLimit) {
        if (now >= nextPrune_) {
            nextPrune_ = now + kForgetPeerMs / 10;
            for (auto it = peers_.begin(); it != peers_.end();)
                it = now - it->last > kForgetPeerMs ? peers_.erase(it) : std::next(it);
        }
        if (!peers_.contains(peer) && peers_.size() >= kMaxPeers) {
            say("too many peers to count the rates of: counting them afresh");
            peers_.clear();
        }
        auto found = peers_.find(peer);
        if (found == peers_.end())
            found = peers_.insert(peer, Bucket{double(options_.rateBurst), now, 0});
        else
            refill(*found, options_.rateBurst, options_.ratePerMinute);
        mine = &*found;
        n = std::min(n, int(std::floor(mine->tokens + 1e-9)));
    }
    if (allLimit) {
        if (!allStarted_) {
            all_ = Bucket{double(options_.globalBurst), now, 0};
            allStarted_ = true;
        } else {
            refill(all_, options_.globalBurst, options_.globalPerMinute);
        }
        n = std::min(n, int(std::floor(all_.tokens + 1e-9)));
    }
    n = std::max(n, 0);
    if (mine)
        mine->tokens -= n;
    if (allLimit)
        all_.tokens -= n;
    if (n < wanted && mine && (!mine->said || now - mine->saidLimited >= kSaySpacingMs)) {
        mine->said = true;
        mine->saidLimited = now;
        say("too many answers to " + brief(peer) + ": the rest are left unsent");
    }
    return n;
}
// Takes up the chats nobody has looked at, oldest first, as many as it has room for.
void ChatBot::admit(qint64 now) {
    if (int(jobs_.size()) >= options_.batch)
        return;
    for (const auto &value : session_.unreadChats(options_.batch)) {
        if (int(jobs_.size()) >= options_.batch)
            break;
        const auto chat = value.toMap();
        const auto id = chat["id"].toString(), local = chat["local"].toString(),
                   peer = chat["peer"].toString(), text = chat["body"].toString();
        if (jobs_.contains(id))
            continue;
        ++stats_.received;
        say("<- " + brief(peer) + ": " + oneLine(text));
        QStringList replies;
        try {
            replies = handler_(local, peer, text);
        } catch (const std::exception &e) {
            say("the handler failed on a chat from " + brief(peer) + ": " + e.what());
            ++stats_.failed;
            session_.markChatMessageRead(id);
            continue;
        } catch (...) {
            say("the handler failed on a chat from " + brief(peer));
            ++stats_.failed;
            session_.markChatMessageRead(id);
            continue;
        }
        // A flood is dropped, not kept for later: it does not become a backlog.
        const int allowedNow = allowed(peer, int(replies.size()), now);
        if (allowedNow < replies.size()) {
            stats_.limited += quint64(replies.size() - allowedNow);
            replies = replies.mid(0, allowedNow);
        }
        if (replies.isEmpty()) {
            session_.markChatMessageRead(id);
            continue;
        }
        jobs_.insert(id, Job{id, local, peer, replies, 0, now + options_.delayMs, 0});
        order_ << id;
    }
}
// Sends the answers that are due. A chat is read once its last answer is queued; an answer that
// will not queue is tried again, a few times, and then given up (and the chat read, or it would
// be tried for ever).
void ChatBot::send(qint64 now) {
    for (int i = 0; i < order_.size();) {
        const auto found = jobs_.find(order_[i]);
        if (found == jobs_.end()) {
            order_.removeAt(i);
            continue;
        }
        Job &job = *found;
        if (job.due > now) {
            ++i;
            continue;
        }
        const auto &text = job.replies[job.next];
        QString replyId;
        if (session_.sendChat(job.local, job.peer, text, &replyId)) {
            say("-> " + brief(job.peer) + ": " + oneLine(text));
            sent_.insert(replyId, Sent{job.peer, QString()});
            sentOrder_ << replyId;
            while (int(sentOrder_.size()) > options_.track) // the oldest are let go
                sent_.remove(sentOrder_.takeFirst());
            job.attempts = 0;
            if (++job.next < job.replies.size()) {
                job.due = now + options_.gapMs;
                ++i;
            } else {
                ++stats_.answered;
                session_.markChatMessageRead(job.id);
                finish(job.id); // removes order_[i]
            }
        } else if (++job.attempts >= options_.maxAttempts) {
            say("cannot answer " + brief(job.peer) + ": " + session_.error() + "; given up after " +
                QString::number(job.attempts) + " attempts");
            ++stats_.failed;
            session_.markChatMessageRead(job.id);
            finish(job.id);
        } else {
            say("cannot answer " + brief(job.peer) + ": " + session_.error() + "; will try again");
            job.due = now + options_.retryMs;
            ++i;
        }
    }
}
void ChatBot::finish(const QString &id) {
    jobs_.remove(id);
    order_.removeAll(id);
}
// Says where each answer got to when that changes, and forgets it once it is final.
void ChatBot::watchStates(qint64 now) {
    if (sent_.isEmpty() || now < nextStates_)
        return;
    nextStates_ = now + 1000;
    QStringList gone;
    for (auto it = sent_.begin(); it != sent_.end(); ++it) {
        const auto state = session_.chatState(it.key());
        if (state != it->state) {
            it->state = state;
            if (!state.isEmpty())
                say("   to " + brief(it->peer) + ": " + QString(state).replace('_', ' '));
        }
        if (state.isEmpty() || final(state))
            gone << it.key();
    }
    for (const auto &id : gone) {
        sent_.remove(id);
        sentOrder_.removeAll(id);
    }
}
} // namespace tools
