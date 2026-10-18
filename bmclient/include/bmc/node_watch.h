#pragma once
#include <algorithm>
#include <cstdint>
namespace bmc {
// The watchdog's judgement of a node that runs but does no good: its status stopped, or it
// has had no peer for too long. Pure, so it can be tested; Session feeds it the node's
// status once a tick and restarts the node when it says so. Times are seconds.
struct NodeWatch {
    enum class Verdict { Fine, Hung, Lonely };
    int64_t started = 0;     // when the node was started: the reference while it has no status
    int64_t hungAfter = 120; // a status this old, for hungGrace seconds, means the node is hung
    int64_t lonelyAfter = 0; // no peer for this long means lonely; 0 never judges it so
    int64_t spacing = 300;   // the least time between two restarts the watchdog asks for
    static constexpr int64_t hungGrace = 10;

    // statusTime: the "time" of the node's status.json, 0 when there is none yet.
    Verdict judge(int64_t now, int64_t statusTime, int peers) {
        if (peers > 0)
            noPeersSince_ = 0;
        else if (!noPeersSince_)
            noPeersSince_ = now;
        // Stale for a while, not for one tick: a Mac that wakes from sleep reads the status
        // of hours ago for a moment, before the node has written another.
        const bool stale = now - std::max(statusTime, started) > hungAfter;
        if (!stale)
            staleSince_ = 0;
        else if (!staleSince_)
            staleSince_ = now;
        Verdict verdict = Verdict::Fine;
        if (stale && now - staleSince_ >= hungGrace)
            verdict = Verdict::Hung;
        else if (!stale && lonelyAfter && peers == 0 && now - noPeersSince_ >= lonelyAfter)
            verdict = Verdict::Lonely;
        if (verdict != Verdict::Fine && lastRestart_ && now - lastRestart_ < spacing)
            return Verdict::Fine;
        return verdict;
    }
    // The node was started, by the watchdog or not: it gets the whole time again.
    void restarted(int64_t now) {
        started = lastRestart_ = now;
        noPeersSince_ = staleSince_ = 0;
    }

  private:
    int64_t noPeersSince_ = 0, staleSince_ = 0, lastRestart_ = 0;
};
} // namespace bmc
