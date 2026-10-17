// The watchdog's judgement of a node that runs but does no good: its status stopped, or it has
// had no peer for too long. These are the assertions tests/core_tests.cpp made before the struct
// moved out of Qt, with two more at the grace's own boundary.
#include <bmc/node_watch.h>
#include <bmc/test.h>

using V = bmc::NodeWatch::Verdict;

TEST(a_node_just_started_is_left_alone) {
    bmc::NodeWatch watch;
    watch.started = 1000;
    watch.hungAfter = 120;
    watch.lonelyAfter = 600;
    watch.spacing = 300;
    CHECK(watch.judge(1010, 0, 0) == V::Fine);    // no status yet: no judgement to make
    CHECK(watch.judge(1100, 1099, 0) == V::Fine); // fresh status, no peers: not yet lonely
}

TEST(a_status_stale_for_long_is_hung) {
    bmc::NodeWatch watch;
    watch.started = 1000;
    watch.hungAfter = 120;
    watch.lonelyAfter = 600;
    watch.spacing = 300;
    // Stale for a while, not for one tick: a Mac that wakes from sleep reads the status of hours
    // ago for a moment, before the node has written another.
    CHECK(watch.judge(5000, 1099, 3) == V::Fine);
    CHECK(watch.judge(5011, 1099, 3) == V::Hung);
    CHECK(watch.judge(5011, 5010, 3) == V::Fine); // a fresh status clears it
    // The grace itself, on a node whose status was already old when the first tick saw it.
    bmc::NodeWatch grace;
    grace.started = 0;
    grace.hungAfter = 0;
    CHECK(grace.judge(1000, 500, 0) == V::Fine);
    CHECK(grace.judge(1009, 500, 0) == V::Fine);
    CHECK(grace.judge(1010, 500, 0) == V::Hung);
}

TEST(no_peer_for_long_is_lonely) {
    bmc::NodeWatch lonely;
    lonely.started = 0;
    lonely.lonelyAfter = 600;
    lonely.spacing = 300;
    CHECK(lonely.judge(100, 100, 0) == V::Fine);
    CHECK(lonely.judge(699, 699, 0) == V::Fine); // less than asked for
    CHECK(lonely.judge(700, 700, 1) == V::Fine); // a peer that came starts the count again
    CHECK(lonely.judge(701, 701, 0) == V::Fine);
    CHECK(lonely.judge(1300, 1300, 0) == V::Fine);
    CHECK(lonely.judge(1301, 1301, 0) == V::Lonely);
    // Without lonelyAfter, a node with no peers is never judged lonely: a desktop that is simply
    // offline is not a service.
    bmc::NodeWatch quiet;
    quiet.started = 0;
    CHECK(quiet.judge(100000, 100000, 0) == V::Fine);
}

TEST(a_restarted_node_gets_the_whole_time_again) {
    bmc::NodeWatch lonely;
    lonely.started = 0;
    lonely.lonelyAfter = 600;
    lonely.spacing = 300;
    // The state that got there: a peer at 700, none since 701, so lonely at 1301.
    CHECK(lonely.judge(100, 100, 0) == V::Fine);
    CHECK(lonely.judge(700, 700, 1) == V::Fine);
    CHECK(lonely.judge(701, 701, 0) == V::Fine);
    CHECK(lonely.judge(1301, 1301, 0) == V::Lonely);
    lonely.restarted(1301);
    CHECK(lonely.judge(1400, 1400, 0) == V::Fine);
    CHECK(lonely.judge(1999, 1999, 0) == V::Fine);
    CHECK(lonely.judge(2000, 2000, 0) == V::Lonely); // and is judged again after it
}

TEST(a_hung_node_is_not_restarted_twice_within_the_spacing) {
    bmc::NodeWatch spaced;
    spaced.started = 0;
    spaced.hungAfter = 60;
    spaced.spacing = 300;
    spaced.restarted(1000);
    CHECK(spaced.judge(1100, 1000, 0) == V::Fine);
    CHECK(spaced.judge(1111, 1000, 0) == V::Fine);
    CHECK(spaced.judge(1400, 1000, 0) == V::Hung); // once the spacing is over
}
