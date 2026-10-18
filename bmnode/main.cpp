// The node on its own, without Qt: the notbit daemon plus the std relay bridge, which is compiled
// into this target and defines the symbol the daemon calls once a second (ynotbit_relay_tick).
//
// The app starts itself with --node, so this accepts the same argument list, --node included: one
// argv and one YNOTBIT_TEST_NODE_FLAG serve both binaries, and both write the same status.json.
#include <cstring>
extern "C" int ntb_daemon(int argc, char **argv);
int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--node") == 0) {
        --argc;
        ++argv;
    }
    return ntb_daemon(argc, argv);
}
