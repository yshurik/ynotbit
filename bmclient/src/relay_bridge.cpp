// Ciphertext-only filesystem boundary. Runs in the keyless relay process, with no Qt: the node's
// status.json, the publish/ spool it drains and the receipts/ it leaves. A port of
// src/relay_bridge.cpp, which the app's own --node path keeps until slice 5 retires it; the keys,
// the states and the order of the writes are the ones that file has.
#include "relay_api.h"
#include <algorithm>
#include <bmc/bytes.h>
#include <bmc/clock.h>
#include <bmc/json.h>
#include <bmc/path.h>
#include <bmc/pow.h>
#include <bmc/protocol.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;

bool writeFile(const fs::path &path, const std::string &text) {
    try {
        bmc::writeFileAtomic(path, bmc::view(text));
        return true;
    } catch (...) {
        return false; // the Qt one returned QSaveFile's false
    }
}

// The window passes its pid; a node whose window died (crash, kill) would keep the port and the
// object store, and the next window's node could not start.
bool parentGone() {
    static const int64_t parent = [] {
        const char *text = std::getenv("YNOTBIT_PARENT_PID");
        return text ? std::strtoll(text, nullptr, 10) : 0;
    }();
    if (parent <= 0)
        return false;
#ifdef _WIN32
    static HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, DWORD(parent));
    return !handle || WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
#else
    return getppid() != pid_t(parent);
#endif
}

// The ^[a-f0-9-]{36}$ of a spooled job's id.
bool isJobId(std::string_view id) {
    if (id.size() != 36)
        return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'f') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    return true;
}

// The status object QJsonObject wrote, key for key and in its sorted order.
std::string statusJson(int peers, int pending, int64_t now, int64_t objects, int64_t objectBytes,
                       const ntb_network_peer_stats &stats, int listenPort) {
    auto number = [](int64_t value) { return std::to_string(value); };
    return "{\"attempts\":" + number(int64_t(stats.attempts)) +
           ",\"eligible_addresses\":" + number(stats.eligible_addresses) +
           ",\"established_incoming\":" + number(stats.established_incoming) +
           ",\"established_outgoing\":" + number(stats.established_outgoing) +
           ",\"known_addresses\":" + number(stats.known_addresses) +
           ",\"listen_port\":" + number(listenPort) + ",\"object_bytes\":" + number(objectBytes) +
           ",\"objects\":" + number(objects) + ",\"peers\":" + number(peers) +
           ",\"pending\":" + number(pending) +
           ",\"pending_outgoing\":" + number(stats.pending_outgoing) +
           ",\"rejections\":" + number(int64_t(stats.rejections)) +
           ",\"setup_timeouts\":" + number(int64_t(stats.setup_timeouts)) +
           ",\"time\":" + number(now) + "}";
}

std::string receiptJson(const char *state, const std::string &hash, int64_t now) {
    return "{\"hash\":" + bmc::json::quote(hash) + ",\"state\":" + bmc::json::quote(state) +
           ",\"time\":" + std::to_string(now) + "}";
}

// The object, up to the 262145 bytes the Qt bridge read of it.
bool readObject(const fs::path &file, bmc::Bytes &out) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    std::string text(262145, '\0');
    in.read(text.data(), std::streamsize(text.size()));
    text.resize(size_t(in.gcount()));
    out.assign(text.begin(), text.end());
    return true;
}
} // namespace

extern "C" void ynotbit_relay_tick(struct ntb_network *nw, const char *directory) {
    static int64_t last = 0;
    const auto now = bmc::now();
    if (last == now || !directory)
        return;
    last = now;
    if (parentGone()) {
        std::raise(SIGTERM); // the main loop's own quit path: clean shutdown
        return;
    }
    try {
        const fs::path root = directory;
        std::error_code ignored;
        fs::create_directories(root / "publish", ignored);
        fs::create_directories(root / "receipts", ignored);
        const int peers = ntb_network_connected_peers(nw);
        const int pending = ntb_network_pending_objects(nw);
        ntb_network_peer_stats stats{};
        ntb_network_get_peer_stats(nw, &stats);
        int64_t objectCount = 0, objectBytes = 0;
        ntb_store_object_stats(nullptr, &objectCount, &objectBytes);
        writeFile(root / "status.json", statusJson(peers, pending, now, objectCount, objectBytes,
                                                   stats, ntb_network_listen_port(nw)));
        // QDir::Name order, which for these names is byte order, and the same eight at most.
        std::vector<fs::path> spooled;
        for (const auto &entry : fs::directory_iterator(root / "publish", ignored))
            if (entry.is_regular_file(ignored) &&
                entry.path().filename().string().ends_with(".object"))
                spooled.push_back(entry.path());
        std::sort(spooled.begin(), spooled.end());
        int count = 0;
        for (const auto &path : spooled) {
            if (++count > 8)
                break;
            const auto id = path.stem().string();
            if (!isJobId(id) || fs::is_symlink(path, ignored))
                continue;
            bmc::Bytes object;
            if (!readObject(path, object))
                continue;
            const auto hash = bmc::Protocol::inventoryHash(object);
            bool accepted = bmc::ProofOfWork::valid(object, now);
            if (accepted)
                accepted = ntb_network_submit(nw, object.data(), object.size()) != 0;
            // Keep the job until peers exist; re-offer after restart or a lost receipt.
            if (accepted && peers > 0) {
                const auto rawHash = bmc::unhex(hash);
                ntb_network_offer(nw, rawHash.data());
            }
            const char *state = !accepted ? "rejected" : peers > 0 ? "offered" : "accepted";
            if (writeFile(root / "receipts" / (id + ".json"), receiptJson(state, hash, now)) &&
                (!accepted || peers > 0))
                fs::remove(path, ignored);
        }
    } catch (...) { /* Preserve queued jobs for a later attempt. */
    }
}
