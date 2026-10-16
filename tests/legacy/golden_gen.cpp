// Writes what the old code answered into tests/golden/. Run once, at the end of slice 1, while
// tests/legacy still holds the old code:
//   cmake --build build --target golden_gen && mkdir -p tests/golden && build/golden_gen
//   tests/golden
#include "oracle.h"
#include <array>
#include <bmc/bytes.h>
#include <bmc/pow.h>
#include <bmc/wire.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
using oracle::Bytes;
std::mt19937_64 rng(20261015);
constexpr int64_t kExpiry = 1700000000;

std::string hex(const Bytes &b) {
    return bmc::hex(b);
}
std::string hex(const std::string &s) {
    return bmc::hex(bmc::view(s));
}
std::string line(std::initializer_list<std::string> fields) {
    std::string out;
    bool first = true;
    for (const auto &f : fields) {
        if (!first)
            out += '|';
        first = false;
        out += f;
    }
    return out + "\n";
}
Bytes randomBytes(size_t n) {
    Bytes b(n);
    for (auto &c : b)
        c = uint8_t(rng());
    return b;
}
Bytes key32() {
    auto k = randomBytes(32);
    k[0] = uint8_t(1 + rng() % 0x7e); // below the group order
    return k;
}
std::string keys(const oracle::Id &id) {
    return hex(Bytes(id.keys.begin(), id.keys.end()));
}
oracle::Id identity(int version) {
    return oracle::fromPrivate("g", key32(), key32(), version, 1, false);
}
const char *flag(bool b) {
    return b ? "1" : "0";
}

void derivations(std::ofstream &out) {
    for (int version = 2; version <= 4; ++version) {
        Bytes s(32, 0), e(32, 0); // the small scalars of the Python fixtures, then random ones
        s[31] = 1;
        e[31] = 2;
        std::vector<std::pair<Bytes, Bytes>> pairs = {{s, e}};
        for (int i = 0; i < 4; ++i)
            pairs.push_back({key32(), key32()});
        for (const auto &[sign, enc] : pairs) {
            const auto id = oracle::fromPrivate("g", sign, enc, version, 1, false);
            out << line({"fromPrivate", hex(sign), hex(enc), std::to_string(version), "0",
                         id.address, keys(id)});
        }
    }
    const std::string phrases[] = {
        "general",
        "ynotbit test phrase",
        "a",
        "emoji \xf0\x9f\x98\x80",
        "\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c \xc3\xbcn\xc3\xaf"
        "code",
        "line\nbreak"};
    for (const auto &phrase : phrases) {
        for (int version : {3, 4}) {
            const auto id = oracle::channel(phrase, "g", version);
            out << line({"channel", hex(phrase), std::to_string(version), id.address, keys(id)});
        }
        for (uint64_t pair : {0, 1, 60, 252}) {
            const auto id = oracle::phraseIdentity(phrase, "g", pair);
            out << line({"phrase", hex(phrase), std::to_string(pair), id.address, keys(id)});
        }
        for (int zero : {1, 2}) {
            const auto found = oracle::firstPair(phrase, zero, 3000);
            out << line({"firstPair", hex(phrase), std::to_string(zero), "3000",
                         found ? std::to_string(*found) : "none"});
        }
    }
    for (const char *address : {"BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6",
                                "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN", "nonsense", "", "BM-"}) {
        out << line(
            {"zeroBytes", hex(std::string(address)), std::to_string(oracle::zeroBytes(address))});
        out << line(
            {"validAddress", hex(std::string(address)), flag(oracle::validAddress(address))});
    }
    for (int i = 0; i < 12; ++i) {
        const auto object = randomBytes(rng() % 600);
        out << line({"inventoryHash", hex(object), oracle::inventoryHash(object)});
    }
    for (int i = 0; i < 40; ++i) {
        std::array<uint8_t, 64> initial;
        for (auto &c : initial)
            c = uint8_t(rng());
        const uint64_t nonce = rng();
        out << line({"trialValue", std::to_string(nonce),
                     hex(Bytes(initial.begin(), initial.end())),
                     std::to_string(oracle::trialValue(nonce, initial))});
    }
    for (int version = 2; version <= 4; ++version) {
        const auto id = identity(version);
        out << line({"getPubkey", hex(id.address), std::to_string(kExpiry),
                     hex(oracle::getPubkey(id.address, kExpiry))});
    }
    const auto token = randomBytes(32);
    const auto ack = oracle::acknowledgment(token, kExpiry);
    out << line({"acknowledgment", hex(token), std::to_string(kExpiry), hex(ack)});
    out << line({"ackToken", hex(ack), hex(oracle::acknowledgmentToken(ack))});
    out << line({"frame", hex(ack), hex(oracle::frame(ack))});
    out << line({"ackToken", hex(randomBytes(40)), "none"});
}

// The core solves one object, because the old code can only judge work; what the old code says of
// it under many requirements is kept, with the time as an offset from the object's expiry.
void work(std::ofstream &out) {
    const int64_t at = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const int64_t expires = at + 600;
    bmc::ProofOfWork pow;
    pow.start(bmc::Wire::acknowledgment(bmc::Bytes(32, 'q'), expires));
    while (!pow.done())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto solved = pow.take();
    for (int64_t offset : {-3000600, -700, -600, -1, 0, 1, 99400})
        for (uint64_t trials : {0ull, 1ull, 1000ull, 1001ull, 5000ull, 1000000ull, 1000001ull})
            for (uint64_t extra : {0ull, 1000ull, 4000ull, 1000000ull, 1000001ull})
                out << line({"powValid", hex(solved), std::to_string(offset),
                             std::to_string(trials), std::to_string(extra),
                             flag(oracle::powValid(solved, expires + offset, trials, extra))});
}

// Up to eight damaged copies of the object that the old code refuses.
template <class Accepts>
void rejects(std::ofstream &out, const char *kind, const std::vector<std::string> &prefix,
             const Bytes &object, Accepts &&accepts) {
    int written = 0;
    for (int attempt = 0; attempt < 200 && written < 8; ++attempt) {
        auto damaged = object;
        if (attempt % 4 == 3)
            damaged.resize(rng() % damaged.size());
        else
            damaged[rng() % damaged.size()] ^= uint8_t(1 << (rng() % 8));
        if (accepts(damaged))
            continue;
        std::string fields = kind;
        for (const auto &p : prefix)
            fields += "|" + p;
        out << fields << "|" << hex(damaged) << "\n";
        ++written;
    }
}

void objects(std::ofstream &out) {
    int index = 0;
    for (int senderVersion = 2; senderVersion <= 4; ++senderVersion) {
        const auto sender = identity(senderVersion);
        for (int kind = 0; kind < 4; ++kind) { // a recipient of version 2, 3, 4, and a chan
            const auto recipient = kind < 3
                                       ? identity(2 + kind)
                                       : oracle::channel("room " + std::to_string(senderVersion),
                                                         "c", 3 + senderVersion % 2);
            const auto to = oracle::publicIdentity(recipient);
            for (int variant = 0; variant < 2; ++variant, ++index) {
                const Bytes ack =
                    variant ? oracle::acknowledgment(randomBytes(32), kExpiry) : Bytes();
                const std::string subject =
                    variant ? "Subject \xc3\xa9 " + std::to_string(index) : "";
                const std::string body =
                    variant ? std::string("Body with unicode \xf0\x9f\x98\x80\nsecond line")
                            : "plain body " + std::to_string(index);
                const auto object = oracle::wireEncode(sender, to, subject, body, kExpiry, ack);
                const auto decoded = oracle::wireDecode(object, recipient);
                if (!decoded)
                    throw std::runtime_error("the old code cannot read its own message");
                const auto &m = decoded->message;
                const std::vector<std::string> who = {recipient.address, flag(recipient.chan),
                                                      keys(recipient)};
                out << line({"message", who[0], who[1], who[2], hex(object), hex(m.subject),
                             hex(m.body), m.from, hex(decoded->acknowledgment), m.hash});
                rejects(out, "reject-message", who, object, [&](const Bytes &d) {
                    return oracle::wireDecode(d, recipient).has_value();
                });
                const auto simple =
                    oracle::protocolEncode(sender, recipient, subject, body, kExpiry);
                const auto legacy = oracle::protocolDecode(simple, recipient);
                if (!legacy)
                    throw std::runtime_error("the old code cannot read its own simple message");
                out << line({"legacy", who[0], who[1], who[2], hex(simple), hex(legacy->subject),
                             hex(legacy->body), legacy->from, legacy->folder, legacy->hash});
            }
        }
    }
    for (int version = 2; version <= 4; ++version)
        for (int round = 0; round < 2; ++round) {
            const auto id = identity(version);
            const auto pub = oracle::encodePubkey(id, kExpiry);
            const auto p = oracle::decodePubkey(pub, id.address);
            if (!p)
                throw std::runtime_error("the old code cannot read its own pubkey");
            out << line({"pubkey", id.address, hex(pub), hex(p->signingKey), hex(p->encryptionKey),
                         std::to_string(p->nonceTrials), std::to_string(p->extraBytes),
                         std::to_string(p->behaviors)});
            rejects(out, "reject-pubkey", {id.address}, pub, [&](const Bytes &d) {
                return oracle::decodePubkey(d, id.address).has_value();
            });
            const auto cast =
                oracle::encodeBroadcast(id, "News \xc3\xa9", "Broadcast body\nline 2", kExpiry);
            const auto b = oracle::decodeBroadcast(cast, id.address);
            if (!b)
                throw std::runtime_error("the old code cannot read its own broadcast");
            out << line({"broadcast", id.address, hex(cast), hex(b->message.subject),
                         hex(b->message.body), b->message.hash});
            rejects(out, "reject-broadcast", {id.address}, cast, [&](const Bytes &d) {
                return oracle::decodeBroadcast(d, id.address).has_value();
            });
        }
}
} // namespace

int main(int argc, char **argv) try {
    const std::string dir = argc > 1 ? argv[1] : "tests/golden";
    std::ofstream derivationsFile(dir + "/derivations.txt"), objectsFile(dir + "/objects.txt");
    if (!derivationsFile || !objectsFile)
        throw std::runtime_error("cannot write in " + dir);
    derivations(derivationsFile);
    work(derivationsFile);
    objects(objectsFile);
    return 0;
} catch (const std::exception &e) {
    std::cerr << "golden_gen: " << e.what() << "\n";
    return 1;
}
