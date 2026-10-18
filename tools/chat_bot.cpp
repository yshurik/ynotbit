#include "chat_bot.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>

namespace tools {
namespace {
constexpr size_t kMaxPeers = 20000;               // peers whose rates are remembered at once
constexpr int64_t kForgetPeerMs = 10 * 60 * 1000; // a peer quiet this long has its full rate again
constexpr int64_t kSaySpacingMs = 60 * 1000;      // how often the same peer's limit is spoken of

// What a delivery ends in: nothing more happens to the message after these.
bool final(const std::string &state) {
    return state == "acknowledged" || state == "failed" || state == "expired" ||
           state == "cancelled";
}
bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
// The first `count` characters (code points) of UTF-8 text.
std::string leftChars(const std::string &text, size_t count) {
    size_t i = 0, chars = 0;
    while (i < text.size() && chars < count) {
        ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80)
            ++i;
        ++chars;
    }
    return text.substr(0, i);
}
size_t charCount(const std::string &text) {
    size_t chars = 0;
    for (const char c : text)
        chars += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return chars;
}
} // namespace

ChatBot::ChatBot(ChatHost &host, Handler handler, ChatBotOptions options, Log log)
    : host_(host), handler_(std::move(handler)), options_(options), log_(std::move(log)) {}

void ChatBot::say(const std::string &line) const {
    if (log_)
        log_(line);
}

std::string ChatBot::brief(const std::string &address) {
    return leftChars(address, 12) + "…";
}

// QString::simplified(), then the line is cut to 80 characters.
std::string ChatBot::oneLine(const std::string &text) {
    std::string line;
    bool pending = false;
    for (const char c : text) {
        if (isSpace(c)) {
            pending = !line.empty();
            continue;
        }
        if (pending) {
            line += ' ';
            pending = false;
        }
        line += c;
    }
    return charCount(line) > 80 ? leftChars(line, 79) + "…" : line;
}

ChatBot::Stats ChatBot::stats() const {
    auto result = stats_;
    result.working = int(jobs_.size());
    result.tracked = int(peers_.size() + sent_.size());
    return result;
}

void ChatBot::poll(int64_t now) {
    // What is due first, so a chat that was waiting out its delay makes room for the next; and
    // what is due again after taking up new ones, so with no delay a chat is answered at once.
    send(now);
    admit(now);
    send(now);
    watchStates(now);
}

// How many of wanted answers to peer may go now: what the peer's rate and the total rate allow,
// taken from both. Rates are token buckets: a burst, refilled by the minute.
int ChatBot::allowed(const std::string &peer, int wanted, int64_t now) {
    if (wanted <= 0)
        return 0;
    const bool peerLimit = options_.ratePerMinute > 0, allLimit = options_.globalPerMinute > 0;
    if (!peerLimit && !allLimit)
        return wanted;
    const auto refill = [now](Bucket &bucket, int burst, double perMinute) {
        bucket.tokens = std::min<double>(
            burst, bucket.tokens + std::max<int64_t>(0, now - bucket.last) / 60000.0 * perMinute);
        bucket.last = now;
    };
    int n = wanted;
    Bucket *mine = nullptr;
    if (peerLimit) {
        if (now >= nextPrune_) {
            nextPrune_ = now + kForgetPeerMs / 10;
            for (auto it = peers_.begin(); it != peers_.end();)
                it = now - it->second.last > kForgetPeerMs ? peers_.erase(it) : std::next(it);
        }
        if (!peers_.count(peer) && peers_.size() >= kMaxPeers) {
            say("too many peers to count the rates of: counting them afresh");
            peers_.clear();
        }
        auto found = peers_.find(peer);
        if (found == peers_.end())
            found = peers_.emplace(peer, Bucket{double(options_.rateBurst), now, false, 0}).first;
        else
            refill(found->second, options_.rateBurst, options_.ratePerMinute);
        mine = &found->second;
        n = std::min(n, int(std::floor(mine->tokens + 1e-9)));
    }
    if (allLimit) {
        if (!allStarted_) {
            all_ = Bucket{double(options_.globalBurst), now, false, 0};
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
void ChatBot::admit(int64_t now) {
    ChatHost::Chat chat;
    while (int(jobs_.size()) < options_.batch && host_.nextChat(chat)) {
        if (jobs_.count(chat.id))
            continue;
        ++stats_.received;
        say("<- " + brief(chat.peer) + ": " + oneLine(chat.text));
        std::vector<std::string> replies;
        try {
            replies = handler_(chat.local, chat.peer, chat.text);
        } catch (const std::exception &e) {
            say("the handler failed on a chat from " + brief(chat.peer) + ": " + e.what());
            ++stats_.failed;
            host_.done(chat.id);
            continue;
        } catch (...) {
            say("the handler failed on a chat from " + brief(chat.peer));
            ++stats_.failed;
            host_.done(chat.id);
            continue;
        }
        // A flood is dropped, not kept for later: it does not become a backlog.
        const int allowedNow = allowed(chat.peer, int(replies.size()), now);
        if (size_t(allowedNow) < replies.size()) {
            stats_.limited += uint64_t(replies.size() - size_t(allowedNow));
            replies.resize(size_t(allowedNow));
        }
        if (replies.empty()) {
            host_.done(chat.id);
            continue;
        }
        jobs_.emplace(chat.id, Job{chat.id, chat.local, chat.peer, std::move(replies), 0,
                                   now + options_.delayMs, 0});
        order_.push_back(chat.id);
    }
}

// Sends the answers that are due. A chat is read once its last answer is queued; an answer that
// will not queue is tried again, a few times, and then given up (and the chat read, or it would
// be tried for ever).
void ChatBot::send(int64_t now) {
    for (size_t i = 0; i < order_.size();) {
        const auto found = jobs_.find(order_[i]);
        if (found == jobs_.end()) {
            order_.erase(order_.begin() + std::ptrdiff_t(i));
            continue;
        }
        Job &job = found->second;
        if (job.due > now) {
            ++i;
            continue;
        }
        const auto &text = job.replies[job.next];
        std::string replyId, why;
        if (host_.send(job.local, job.peer, text, replyId, why)) {
            say("-> " + brief(job.peer) + ": " + oneLine(text));
            sent_[replyId] = Sent{job.peer, std::string()};
            sentOrder_.push_back(replyId);
            while (int(sentOrder_.size()) > options_.track) { // the oldest are let go
                sent_.erase(sentOrder_.front());
                sentOrder_.pop_front();
            }
            job.attempts = 0;
            if (++job.next < job.replies.size()) {
                job.due = now + options_.gapMs;
                ++i;
            } else {
                ++stats_.answered;
                host_.done(job.id);
                finish(job.id); // removes order_[i]
            }
        } else if (++job.attempts >= options_.maxAttempts) {
            say("cannot answer " + brief(job.peer) + ": " + why + "; given up after " +
                std::to_string(job.attempts) + " attempts");
            ++stats_.failed;
            host_.done(job.id);
            finish(job.id);
        } else {
            say("cannot answer " + brief(job.peer) + ": " + why + "; will try again");
            job.due = now + options_.retryMs;
            ++i;
        }
    }
}

void ChatBot::finish(const std::string &id) {
    jobs_.erase(id);
    order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
}

// Says where each answer got to when that changes, and forgets it once it is final.
void ChatBot::watchStates(int64_t now) {
    if (sent_.empty() || now < nextStates_)
        return;
    nextStates_ = now + 1000;
    std::vector<std::string> gone;
    for (auto &[id, sent] : sent_) {
        const auto state = host_.state(id);
        if (state != sent.state) {
            sent.state = state;
            if (!state.empty()) {
                auto shown = state;
                std::replace(shown.begin(), shown.end(), '_', ' ');
                say("   to " + brief(sent.peer) + ": " + shown);
            }
        }
        if (state.empty() || final(state))
            gone.push_back(id);
    }
    for (const auto &id : gone) {
        sent_.erase(id);
        sentOrder_.erase(std::remove(sentOrder_.begin(), sentOrder_.end(), id), sentOrder_.end());
    }
}

} // namespace tools
