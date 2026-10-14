// Measures how smoothly a long chat opens and scrolls. Builds a conversation of synthetic
// messages (short lines, a few paragraphs, a few with a list and code) and shows it in the real
// chat pane as the window does: the newest messages, and a page more of older ones each time
// the pane asks. Times the opening, a message arriving, a sent one moving on, and then scrolls
// up through the whole conversation a step at a time, timing each frame: the scroll, the
// repaint, and the loading of a page when the scroll asks for one. It also checks that the
// reader's place holds when a page comes in above.
//
//   chat_scroll_bench [--messages <n>] [--window <n>] [--page <n>] [--step <px>]
//
// --messages  how long the conversation is (1000)
// --window    the messages shown when it opens (100)
// --page      the older ones brought in each time (50)
// --step      pixels per scroll step (90, a notch of a trackpad)
// Not a test: it prints numbers. QT_QPA_PLATFORM=offscreen runs it without opening a window.
#include "chats_pane.h"
#include <QtWidgets>
#include <algorithm>
#include <iostream>

namespace {
quint32 state = 12345;
int random(int n) {
    state = state * 1664525u + 1013904223u;
    return int((state >> 8) % unsigned(n));
}
// A message as a real chat has them: mostly a line, now and then a paragraph, rarely a list
// with code and a link.
QString text() {
    static const char *lines[] = {"ok", "Thanks!", "See you at 5", "Sounds good", "lol",
                                  "Will do", "Where are you?", "yes", "no idea", "later"};
    static const char *paragraphs[] = {
        "Just checking in about the **release**: the build for macOS is green, Windows is still "
        "waiting on the signing certificate. I will push the notes once CI finishes.",
        "I read the thread again and I think we should keep the *old* behaviour for the relay "
        "and only change the node, otherwise the tests for the handshake need a rewrite.",
        "Lunch at the usual place? They have the soup you liked, and the table by the window is "
        "free on Thursdays, so we can talk about the plan for the next week without a rush."};
    const int kind = random(100);
    if (kind < 65)
        return lines[random(10)];
    if (kind < 93)
        return paragraphs[random(3)];
    return "Notes for tomorrow:\n\n1. Bump the version in `CMakeLists.txt`\n2. Dispatch CI and "
           "wait for the draft release\n3. Check [the notes](https://example.org/releases/v0.7.1)"
           "\n\n```\ncmake --build build --target ynotbit\nctest --output-on-failure\n```\n"
           "Then publish, and tell the others in the chat.";
}
QVariantMap message(int n, bool mine, const QString &state) {
    return QVariantMap{{"id", "bench-" + QString::number(n)},
                       {"body", text()},
                       {"outgoing", mine},
                       {"seqno", n},
                       {"received", qint64(1790000000) + n * 90},
                       {"expires", qint64(1790086400) + n * 90},
                       {"state", state}};
}
double percentile(QList<double> sorted, double q) {
    std::sort(sorted.begin(), sorted.end());
    return sorted.isEmpty() ? 0 : sorted[qMin(int(q * sorted.size()), int(sorted.size()) - 1)];
}
template <class F> double milliseconds(F f) {
    QElapsedTimer clock;
    clock.start();
    f();
    return clock.nsecsElapsed() / 1e6;
}
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    int messages = 1000, window = bm::kChatWindow, page = bm::kChatPage, step = 90;
    const auto args = app.arguments();
    for (int i = 1; i + 1 < args.size(); i += 2) {
        const int value = args[i + 1].toInt();
        if (args[i] == "--messages")
            messages = value;
        else if (args[i] == "--window")
            window = value;
        else if (args[i] == "--page")
            page = value;
        else if (args[i] == "--step")
            step = value;
    }
    QVariantList all;
    for (int n = 0; n < messages; ++n)
        all << message(n, random(100) < 40, "acknowledged");
    bm::ChatsPane pane;
    pane.resize(960, 720);
    pane.setConversation("BM-bench-me", "Me", "BM-bench-peer", "Peer", false);
    pane.show();
    app.processEvents();
    auto scroll = pane.findChild<QScrollArea *>("chatMessagesScroll");
    auto bar = scroll->verticalScrollBar();
    const auto settle = [&] {
        for (int i = 0; i < 3; ++i)
            app.processEvents();
        pane.grab();
    };
    // What the window does: the newest `shown` messages, and whether older ones are kept.
    int shown = qMin(window, int(all.size()));
    const auto publish = [&] { pane.setMessages(all.mid(all.size() - shown), shown < all.size()); };
    // A page asked for, and what the reader's place did when it came in.
    QList<double> loads;
    int worstDrift = 0;
    const auto topOf = [&](QWidget *w) { return w->mapTo(scroll->viewport(), QPoint(0, 0)).y(); };
    QObject::connect(&pane, &bm::ChatsPane::olderRequested, [&] {
        QLabel *anchor = nullptr; // the first message in view
        QPointer<QLabel> kept;
        for (auto body : pane.findChildren<QLabel *>("chatBody"))
            if (topOf(body) >= 0 && (!anchor || topOf(body) < topOf(anchor)))
                anchor = body;
        kept = anchor;
        const int placed = anchor ? topOf(anchor) : 0;
        shown = qMin(shown + page, int(all.size()));
        loads << milliseconds([&] { publish(); });
        if (kept)
            worstDrift = qMax(worstDrift, qAbs(topOf(kept) - placed));
    });

    std::cout << messages << " messages, a window of " << window << ", pages of " << page << "\n";
    const double opening = milliseconds([&] {
        publish();
        settle();
    });
    std::cout << "open a conversation:           " << opening << " ms\n";
    double arriving = 0, moving = 0;
    {
        auto grown = all.mid(all.size() - shown);
        grown << message(messages, false, "received");
        arriving = milliseconds([&] {
            pane.setMessages(grown, shown < all.size());
            settle();
        });
        const int last = grown.size() - 2;
        auto mine = grown[last].toMap();
        mine["outgoing"] = true;
        mine["state"] = "queued";
        grown[last] = mine;
        pane.setMessages(grown, shown < all.size());
        settle();
        mine["state"] = "acknowledged";
        grown[last] = mine;
        moving = milliseconds([&] {
            pane.setMessages(grown, shown < all.size());
            settle();
        });
        publish(); // as it was
        settle();
    }
    std::cout << "a new message arrives:         " << arriving << " ms\n";
    std::cout << "a sent message moves on:       " << moving << " ms\n";
    bar->setValue(bar->maximum());
    settle();
    // Up through the whole conversation: a step per frame, as a trackpad scrolls.
    QList<double> frames;
    int slow = 0, veryslow = 0;
    for (int tick = 0; tick < 100000; ++tick) {
        const double frame = milliseconds([&] {
            bar->setValue(qMax(0, bar->value() - step));
            app.processEvents(); // a page asked for is brought in here
            pane.grab();
        });
        frames << frame;
        slow += frame > 16.7;
        veryslow += frame > 33.4;
        if (bar->value() == 0 && shown == all.size())
            break;
    }
    double loadSum = 0, loadMax = 0;
    for (double load : loads) {
        loadSum += load;
        loadMax = qMax(loadMax, load);
    }
    std::cout << "scroll up through everything:  " << frames.size() << " frames, " << loads.size()
              << " pages\n";
    std::cout << "  a page comes in:             " << (loads.isEmpty() ? 0 : loadSum / loads.size())
              << " ms on average, " << loadMax << " ms at most\n";
    std::cout << "  frame time:                  p50 " << percentile(frames, 0.5) << ", p95 "
              << percentile(frames, 0.95) << ", p99 " << percentile(frames, 0.99) << ", max "
              << percentile(frames, 1.0) << " ms\n";
    std::cout << "  frames over 16.7 ms / 33 ms: " << slow << " / " << veryslow << "\n";
    std::cout << "  reader's place, worst drift: " << worstDrift << " px\n";
    std::cout << "  bubbles built at the end:    " << pane.findChildren<QLabel *>("chatBody").size()
              << "\n";
    return 0;
}
