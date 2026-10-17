// The node's status.json: the keys the relay bridge writes, and what the reader does with a file
// that is missing, foreign or half-written.
#include <bmc/node_status.h>
#include <bmc/test.h>
#include <filesystem>

// The object src/relay_bridge.cpp writes, key for key (QJsonObject sorts them).
static const char *kFullStatus =
    R"({"attempts":1,"eligible_addresses":3,"established_incoming":2,"established_outgoing":1,)"
    R"("known_addresses":4,"listen_port":8444,"object_bytes":2048,"objects":7,"peers":3,)"
    R"("pending":2,"pending_outgoing":5,"rejections":6,"setup_timeouts":9,"time":1700000000})";

TEST(the_counters_are_read) {
    const auto status = bmc::NodeStatus::parse(kFullStatus);
    CHECK(status.has_value());
    CHECK_EQ(status->time, int64_t(1700000000));
    CHECK_EQ(status->peers, 3);
    CHECK_EQ(status->pending, 2);
    CHECK_EQ(status->objects, int64_t(7));
    CHECK_EQ(status->objectBytes, int64_t(2048));
    CHECK_EQ(status->establishedOutgoing, 1);
    CHECK_EQ(status->pendingOutgoing, 5);
    CHECK_EQ(status->establishedIncoming, 2);
    CHECK_EQ(status->knownAddresses, 4);
    CHECK_EQ(status->eligibleAddresses, 3);
    CHECK_EQ(status->attempts, int64_t(1));
    CHECK_EQ(status->setupTimeouts, int64_t(9));
    CHECK_EQ(status->rejections, int64_t(6));
    CHECK_EQ(status->listenPort, 8444);
}

TEST(a_status_without_objects_is_not_a_status) {
    // "objects" is the member Session::tick tests before it believes any of the rest.
    for (const char *text :
         {"", "{}", "[]", R"("objects")", R"({"time":5})", "{", "not json", R"({"objects":7)"})
        CHECK(!bmc::NodeStatus::parse(text).has_value());
}

TEST(a_member_of_another_type_reads_as_zero) {
    const auto status =
        bmc::NodeStatus::parse(R"({"objects":"lots","peers":"3","time":null,"listen_port":true})");
    CHECK(status.has_value()); // "objects" is there, of whatever type, so this is a status
    CHECK_EQ(status->objects, int64_t(0));
    CHECK_EQ(status->peers, 0);
    CHECK_EQ(status->time, int64_t(0));
    CHECK_EQ(status->listenPort, 0);
}

TEST(a_status_older_than_the_grace_is_not_fresh) {
    const auto status = *bmc::NodeStatus::parse(R"({"objects":0,"time":1000})");
    CHECK(status.fresh(1000));
    CHECK(status.fresh(1010));
    CHECK(!status.fresh(1011));
    // A status with no time at all is never fresh: the node has not said anything yet.
    CHECK(!bmc::NodeStatus::parse(R"({"objects":0})")->fresh(1000));
}

TEST(the_file_is_read_whole_and_a_missing_one_is_nothing) {
    const auto dir = std::filesystem::path(bmc::test::tempDir());
    const auto file = dir / "status.json";
    CHECK(!bmc::NodeStatus::fromFile(file).has_value());
    bmc::test::writeFile(file.string(),
                         bmc::view(std::string(R"({"objects":12,"listen_port":9000})")));
    const auto status = bmc::NodeStatus::fromFile(file);
    CHECK(status.has_value());
    CHECK_EQ(status->objects, int64_t(12));
    CHECK_EQ(status->listenPort, 9000);
    CHECK_EQ(status->peers, 0); // a member the file does not have
    bmc::test::writeFile(file.string(), bmc::view(std::string("garbage")));
    CHECK(!bmc::NodeStatus::fromFile(file).has_value());
}
