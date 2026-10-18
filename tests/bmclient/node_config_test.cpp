// The node's argv and the endpoint strings the app accepts. The argv table and the endpoint table
// are both what Qt's Session produced, taken before the move (see the plan's Task 0): they are the
// behaviour, not a description of it.
#include <bmc/node_config.h>
#include <bmc/test.h>
#include <string>
#include <vector>

namespace {
std::string joined(const std::vector<std::string> &args) {
    std::string out;
    for (const auto &arg : args) {
        if (!out.empty())
            out += ' ';
        out += arg;
    }
    return out;
}
} // namespace

TEST(the_arguments_are_the_ones_the_app_builds) {
    const std::string root = "/nodes/one";
    const std::string maildir = root + "/unused-maildir";
    const auto argv = [&](bmc::NodeConfig config) {
        config.root = root;
        config.maildir = maildir;
        return joined(config.nodeArguments());
    };
    CHECK_EQ(argv({}), std::string("--node -D /nodes/one -m /nodes/one/unused-maildir "
                                   "-R 2048 -A 90"));
    bmc::NodeConfig quiet;
    quiet.listen = false;
    CHECK_EQ(argv(quiet), std::string("--node -D /nodes/one -m /nodes/one/unused-maildir -i "
                                      "-R 2048 -A 90"));
    bmc::NodeConfig port;
    port.port = 8445;
    CHECK_EQ(argv(port), std::string("--node -D /nodes/one -m /nodes/one/unused-maildir "
                                     "-R 2048 -A 90 -p 8445"));
    bmc::NodeConfig peers;
    peers.maxOutgoing = 16;
    peers.maxIncoming = 4;
    CHECK_EQ(argv(peers), std::string("--node -D /nodes/one -m /nodes/one/unused-maildir "
                                      "-R 2048 -A 90 -o 16 -n 4"));
    bmc::NodeConfig one;
    one.peer = "127.0.0.1:8444";
    CHECK_EQ(argv(one), std::string("--node -D /nodes/one -m /nodes/one/unused-maildir "
                                    "-R 2048 -A 90 -P 127.0.0.1:8444 -L"));
    // A proxy turns listening off, so -i appears beside -r.
    bmc::NodeConfig hidden;
    hidden.listen = false;
    hidden.proxy = "127.0.0.1:9050";
    CHECK_EQ(argv(hidden), std::string("--node -D /nodes/one -m /nodes/one/unused-maildir -i "
                                       "-R 2048 -A 90 -r 127.0.0.1:9050 -B"));
    bmc::NodeConfig shortlived;
    shortlived.retentionMB = 64;
    shortlived.retentionDays = 1;
    CHECK_EQ(argv(shortlived), std::string("--node -D /nodes/one -m /nodes/one/unused-maildir "
                                           "-R 64 -A 1"));
    // A private bot links to its peer alone: the three flags come last, after everything else.
    bmc::NodeConfig privateLink;
    privateLink.peer = "127.0.0.1:8444";
    privateLink.linkOnly = true;
    CHECK_EQ(argv(privateLink), std::string("--node -D /nodes/one -m /nodes/one/unused-maildir "
                                            "-R 2048 -A 90 -P 127.0.0.1:8444 -L -b -B -e"));
    bmc::NodeConfig all;
    all.listen = false;
    all.retentionMB = 128;
    all.retentionDays = 7;
    all.maxOutgoing = 32;
    all.maxIncoming = 16;
    all.port = 9000;
    all.peer = "127.0.0.1:8444";
    all.proxy = "127.0.0.1:9050";
    CHECK_EQ(argv(all),
             std::string("--node -D /nodes/one -m /nodes/one/unused-maildir -i -R 128 -A 7 "
                         "-o 32 -n 16 -p 9000 -P 127.0.0.1:8444 -L -r 127.0.0.1:9050 -B"));
}

TEST(a_port_the_settings_do_not_ask_for_is_the_nodes_own) {
    bmc::NodeConfig config;
    config.root = "/n";
    config.maildir = "/n/m";
    CHECK_EQ(config.listenPort(), bmc::kNodePort);
    CHECK(joined(config.nodeArguments()).find("-p") == std::string::npos);
    config.port = 9000;
    CHECK_EQ(config.listenPort(), 9000);
    CHECK_EQ(joined(config.nodeArguments()),
             std::string("--node -D /n -m /n/m -R 2048 -A 90 -p 9000"));
    // The app's quirk, kept: a port outside 1..65535 is still asked for, at the default.
    config.port = 65536;
    CHECK_EQ(joined(config.nodeArguments()),
             std::string("--node -D /n -m /n/m -R 2048 -A 90 -p 8444"));
    config.port = -1;
    CHECK_EQ(config.listenPort(), bmc::kNodePort);
    CHECK(joined(config.nodeArguments()).find("-p") == std::string::npos);
}

TEST(retention_is_clamped_to_what_the_node_accepts) {
    bmc::NodeConfig low;
    low.retentionMB = 0;
    low.retentionDays = 0;
    low.clampRetention();
    CHECK_EQ(low.retentionMB, 64);
    CHECK_EQ(low.retentionDays, 1);
    bmc::NodeConfig high;
    high.retentionMB = 99999;
    high.retentionDays = 99999;
    high.clampRetention();
    CHECK_EQ(high.retentionMB, 32768);
    CHECK_EQ(high.retentionDays, 3650);
}

TEST(endpoints_are_the_ones_qt_accepted) {
    const struct {
        const char *text;
        bool valid;
    } cases[] = {
        {"", true},
        {"127.0.0.1:8444", true},
        {"0.0.0.0:8444", true},
        {"255.255.255.255:8444", true},
        {"1.2.3.4:1", true},
        {"1.2.3.4:65535", true},
        {"[::1]:8444", true},
        {"[::1]:80", true},
        {"[::ffff:1.2.3.4]:8444", true},
        {"1.2.3.4:080", true},
        {"1.2.3.4:08", true},
        {"1.2.3.4:008444", true},
        {"010.2.3.4:8444", true},
        {" 1.2.3.4:8444", true},
        {"1.2.3.4", false},
        {"[::1]", false},
        {"1.2.3.4:", false},
        {":8444", false},
        {"::1", false},
        {"::1:8444", false},
        {"1.2.3.4:0", false},
        {"1.2.3.4:-1", false},
        {"1.2.3.4:+8444", false},
        {"1.2.3.4:65536", false},
        {"1.2.3.4:0000065536", false},
        {"a b:1", false},
        {"localhost:8444", false},
        {"999.1.1.1:8444", false},
        {"1.2.3.4.5:8444", false},
        {"[1.2.3.4]:8444", false},
        {"1.2.3.4:8444/x", false},
        {"1.2.3.4:8444?x", false},
        {"1.2.3.4:8444#x", false},
        {"1.2.3.4:8444%201", false},
        {"1.2.3.4:8444.", false},
        {"1.2.3.4:8444:8444", false},
        {"user@1.2.3.4:8444", false},
        {"tcp://1.2.3.4:8444", false},
        {"  ", false},
        // The one documented difference: Qt trims only the leading whitespace, this trims both
        // ends. The app trims the value it saves, so no user can tell the two apart.
        {"1.2.3.4:8444 ", true},
        {"1.2.3.4:8444\t", true},
    };
    for (const auto &one : cases)
        if (bmc::validEndpoint(one.text) != one.valid)
            bmc::test::fail(__FILE__, __LINE__,
                            std::string("validEndpoint(\"") + one.text + "\") should be " +
                                (one.valid ? "true" : "false"));
}

TEST(port_mapping_needs_a_listening_node_without_a_proxy) {
    CHECK(bmc::wantsPortMapping(true, true, true, ""));
    CHECK(!bmc::wantsPortMapping(false, true, true, ""));
    CHECK(!bmc::wantsPortMapping(true, false, true, ""));
    CHECK(!bmc::wantsPortMapping(true, true, false, ""));
    CHECK(!bmc::wantsPortMapping(true, true, true, "127.0.0.1:9050"));
}
