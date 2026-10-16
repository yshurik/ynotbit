#include "legacy/oracle.h"
#include <algorithm>
#include <bmc/bytes.h>
#include <bmc/protocol.h>
#include <bmc/test.h>
#include <bmc/wire.h>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <random>

namespace {
std::mt19937_64 rng(0x5eed000a);
constexpr int64_t kExpiry = 1700000000;

bmc::Bytes key32() {
    bmc::Bytes k(32);
    for (auto &b : k)
        b = uint8_t(rng());
    k[0] = uint8_t(1 + rng() % 0x7e); // below the group order
    return k;
}
bmc::Identity identityOf(int version) {
    return bmc::Protocol::fromPrivate("p", key32(), key32(), version);
}
bmc::Bytes token() {
    bmc::Bytes t(32);
    for (auto &b : t)
        b = uint8_t(rng());
    return t;
}
oracle::Id asOracle(const bmc::Identity &i) {
    oracle::Id id;
    id.label = i.label;
    id.address = i.address;
    id.chan = i.chan;
    std::copy(i.keys.data(), i.keys.data() + 64, id.keys.begin());
    return id;
}
oracle::Pub asOracle(const bmc::PublicIdentity &p) {
    return {p.address,    p.signingKey, p.encryptionKey, p.nonceTrials,
            p.extraBytes, p.behaviors,  p.chan};
}
template <class F> std::string errorOf(F &&f) {
    try {
        f();
    } catch (const std::exception &e) {
        return e.what();
    }
    return "<no error>";
}
std::string describe(const bmc::PublicIdentity &p) {
    return p.address + "|" + bmc::hex(p.signingKey) + "|" + bmc::hex(p.encryptionKey) + "|" +
           std::to_string(p.nonceTrials) + "|" + std::to_string(p.extraBytes) + "|" +
           std::to_string(p.behaviors) + "|" + (p.chan ? "chan" : "-");
}
std::string describe(const oracle::Pub &p) {
    return p.address + "|" + bmc::hex(p.signingKey) + "|" + bmc::hex(p.encryptionKey) + "|" +
           std::to_string(p.nonceTrials) + "|" + std::to_string(p.extraBytes) + "|" +
           std::to_string(p.behaviors) + "|" + (p.chan ? "chan" : "-");
}
std::string describe(const bmc::DecodedEnvelope &e) {
    const auto &m = e.message;
    return m.hash + "|" + m.from + "|" + m.to + "|" + m.subject + "|" + m.body + "|" + m.folder +
           "|" + describe(e.sender) + "|" + bmc::hex(e.acknowledgment) + "|" +
           (e.broadcast ? "broadcast" : "message");
}
std::string describe(const oracle::Env &e) {
    const auto &m = e.message;
    return m.hash + "|" + m.from + "|" + m.to + "|" + m.subject + "|" + m.body + "|" + m.folder +
           "|" + describe(e.sender) + "|" + bmc::hex(e.acknowledgment) + "|" +
           (e.broadcast ? "broadcast" : "message");
}
template <class T> std::string describe(const std::optional<T> &v) {
    return v ? describe(*v) : std::string("<none>");
}
std::string randomText(bool body) {
    const char *pieces[] = {"",
                            "plain",
                            "unicode \xc3\xa9 \xd0\x9f",
                            "emoji \xf0\x9f\x98\x80",
                            "tab\there",
                            "\xef\xbb\xbf"
                            "bom body",
                            "Subject:inner",
                            "x",
                            "  spaces  "};
    const char *lines[] = {"line1\nline2", "\r\n", "\nBody:odd"};
    std::string s = pieces[rng() % std::size(pieces)];
    if (body && rng() % 3 == 0)
        s += lines[rng() % std::size(lines)];
    return s;
}
void sameMessage(const bmc::Bytes &object, const bmc::Identity &recipient) {
    const auto ours = describe(bmc::Wire::decodeMessage(object, recipient));
    const auto old = describe(oracle::wireDecode(object, asOracle(recipient)));
    if (ours != old)
        bmc::test::fail(__FILE__, __LINE__,
                        "decodeMessage differs for " + bmc::hex(object) + "\n  old:  " + old +
                            "\n  core: " + ours);
}
} // namespace

TEST(wire_messages_cross_between_old_and_new) {
    for (int senderVersion = 2; senderVersion <= 4; ++senderVersion)
        for (int round = 0; round < 10; ++round) {
            const auto sender = identityOf(senderVersion);
            const auto recipient = round % 4 == 3
                                       ? bmc::Protocol::channel("room " + std::to_string(round),
                                                                "c", 3 + (round / 4) % 2)
                                       : identityOf(2 + int(rng() % 3));
            const auto to = bmc::Wire::publicIdentity(recipient);
            CHECK_EQ(describe(to), describe(oracle::publicIdentity(asOracle(recipient))));
            const bmc::Bytes ack =
                round % 2 ? bmc::Wire::acknowledgment(token(), kExpiry) : bmc::Bytes();
            const std::string subject = round % 3 ? randomText(false) : "";
            const std::string body = randomText(true);
            // new encodes
            const auto object = bmc::Wire::encodeMessage(sender, to, subject, body, kExpiry, ack);
            sameMessage(object, recipient);
            const auto fromNew = bmc::Wire::decodeMessage(object, recipient);
            CHECK(fromNew.has_value());
            CHECK_EQ(fromNew->message.subject, subject);
            CHECK_EQ(fromNew->message.body, body);
            CHECK_EQ(fromNew->acknowledgment, ack);
            // old encodes
            const auto oldObject =
                oracle::wireEncode(asOracle(sender), asOracle(to), subject, body, kExpiry, ack);
            sameMessage(oldObject, recipient);
            CHECK(bmc::Wire::decodeMessage(oldObject, recipient).has_value());
        }
}

TEST(wire_text_decoding_agrees_with_the_old_code_on_raw_text) {
    const auto sender = identityOf(4), recipient = identityOf(4);
    const auto to = bmc::Wire::publicIdentity(recipient);
    for (const char *raw :
         {"\xef\xbb\xbfplain",
          "Subject:s\nBody:\xef\xbb\xbf"
          "b",
          "Subject:s\nBody:\xff\xfe", "Subject:s\nBody:\xe2\x82", "no subject", "Subject:only",
          "Subject:a\nBody:b\nBody:c", "Subject:\xc3\nBody:x", "", "\xf0\x9f",
          "Subject:s\nBody:\xed\xa0\x80\xf4\x90\x80\x80", "Subject:\nBody:", "\xef\xbb\xbf"})
        sameMessage(bmc::Wire::encodeMessageText(sender, to, bmc::view(raw), kExpiry), recipient);
}

TEST(wire_pubkeys_requests_acks_and_frames_match_the_old_code) {
    for (int version = 2; version <= 4; ++version) {
        const auto id = identityOf(version), other = identityOf(version);
        const auto oid = asOracle(id);
        CHECK_EQ(bmc::Wire::validAddress(id.address), oracle::validAddress(id.address));
        CHECK_EQ(describe(bmc::Wire::publicIdentity(id)), describe(oracle::publicIdentity(oid)));
        // getpubkey, acknowledgment and frame have no randomness: byte for byte
        CHECK_EQ(bmc::Wire::getPubkey(id.address, kExpiry), oracle::getPubkey(id.address, kExpiry));
        const auto t = token();
        const auto ack = bmc::Wire::acknowledgment(t, kExpiry);
        CHECK_EQ(ack, oracle::acknowledgment(t, kExpiry));
        CHECK_EQ(bmc::Wire::frame(ack), oracle::frame(ack));
        CHECK_EQ(bmc::Wire::acknowledgmentToken(ack), oracle::acknowledgmentToken(ack));
        CHECK_EQ(bmc::Wire::acknowledgmentToken(ack), t);
        // pubkeys: version 2 has no randomness; every version crosses in both directions
        const auto pub = bmc::Wire::encodePubkey(id, kExpiry);
        const auto oldPub = oracle::encodePubkey(oid, kExpiry);
        if (version == 2)
            CHECK_EQ(pub, oldPub);
        for (const auto *object : {&pub, &oldPub}) {
            CHECK_EQ(describe(bmc::Wire::decodePubkey(*object, id.address)),
                     describe(oracle::decodePubkey(*object, id.address)));
            CHECK(bmc::Wire::decodePubkey(*object, id.address).has_value());
            CHECK_EQ(describe(bmc::Wire::decodePubkey(*object, other.address)),
                     describe(oracle::decodePubkey(*object, other.address)));
        }
        // requests for a pubkey
        const auto request = bmc::Wire::getPubkey(id.address, kExpiry);
        CHECK_EQ(bmc::Wire::requestsIdentity(request, id), oracle::requestsIdentity(request, oid));
        CHECK_EQ(bmc::Wire::requestsIdentity(request, other),
                 oracle::requestsIdentity(request, asOracle(other)));
        // the same errors
        CHECK_EQ(errorOf([&] { bmc::Wire::frame(bmc::Bytes{1, 2, 3}); }),
                 errorOf([&] { oracle::frame({1, 2, 3}); }));
        CHECK_EQ(errorOf([] { bmc::Wire::acknowledgment(bmc::Bytes(31, 0), kExpiry); }),
                 errorOf([] { oracle::acknowledgment(oracle::Bytes(31, 0), kExpiry); }));
        CHECK_EQ(errorOf([] { bmc::Wire::getPubkey("junk", kExpiry); }),
                 errorOf([] { oracle::getPubkey("junk", kExpiry); }));
        CHECK_EQ(errorOf([&] { bmc::Wire::encodePubkey(id, 0); }),
                 errorOf([&] { oracle::encodePubkey(oid, 0); }));
    }
}

TEST(wire_broadcasts_cross_between_old_and_new) {
    for (int version = 2; version <= 4; ++version)
        for (int round = 0; round < 6; ++round) {
            const auto id = identityOf(version), other = identityOf(version);
            const auto subject = randomText(false), body = randomText(true);
            const auto object = bmc::Wire::encodeBroadcast(id, subject, body, kExpiry);
            const auto oldObject = oracle::encodeBroadcast(asOracle(id), subject, body, kExpiry);
            for (const auto *o : {&object, &oldObject}) {
                CHECK_EQ(describe(bmc::Wire::decodeBroadcast(*o, id.address)),
                         describe(oracle::decodeBroadcast(*o, id.address)));
                CHECK(bmc::Wire::decodeBroadcast(*o, id.address).has_value());
                CHECK_EQ(describe(bmc::Wire::decodeBroadcast(*o, other.address)),
                         describe(oracle::decodeBroadcast(*o, other.address)));
            }
            CHECK_EQ(bmc::Wire::decodeBroadcast(object, id.address)->message.body, body);
        }
}

TEST(wire_encoders_raise_the_same_errors_as_the_old_code) {
    const auto sender = identityOf(3), recipient = identityOf(4);
    const auto to = bmc::Wire::publicIdentity(recipient);
    auto badPoint = to;
    std::fill(badPoint.encryptionKey.begin(), badPoint.encryptionKey.end(), 0);
    auto badAddress = to;
    badAddress.address = sender.address;
    auto shortKey = to;
    shortKey.signingKey.resize(10);
    const std::string big(bmc::kMaxLetterText, 'x');
    const bmc::Bytes badAck =
        bmc::Wire::getPubkey(sender.address, kExpiry); // not an acknowledgment
    const auto same = [&](auto &&ours, auto &&old) {
        const auto o = errorOf(ours);
        CHECK(o != "<no error>");
        CHECK_EQ(o, errorOf(old));
    };
    const auto os = asOracle(sender);
    same([&] { bmc::Wire::encodeMessage(sender, badPoint, "", "", kExpiry); },
         [&] { oracle::wireEncode(os, asOracle(badPoint), "", "", kExpiry, {}); });
    same([&] { bmc::Wire::encodeMessage(sender, badAddress, "", "", kExpiry); },
         [&] { oracle::wireEncode(os, asOracle(badAddress), "", "", kExpiry, {}); });
    same([&] { bmc::Wire::encodeMessage(sender, shortKey, "", "", kExpiry); },
         [&] { oracle::wireEncode(os, asOracle(shortKey), "", "", kExpiry, {}); });
    same([&] { bmc::Wire::encodeMessage(sender, to, "a\nb", "", kExpiry); },
         [&] { oracle::wireEncode(os, asOracle(to), "a\nb", "", kExpiry, {}); });
    same([&] { bmc::Wire::encodeMessage(sender, to, "a\rb", "", kExpiry); },
         [&] { oracle::wireEncode(os, asOracle(to), "a\rb", "", kExpiry, {}); });
    same([&] { bmc::Wire::encodeMessage(sender, to, "", big, kExpiry); },
         [&] { oracle::wireEncode(os, asOracle(to), "", big, kExpiry, {}); });
    same([&] { bmc::Wire::encodeMessage(sender, to, "", "", kExpiry, badAck); },
         [&] { oracle::wireEncode(os, asOracle(to), "", "", kExpiry, badAck); });
    same([&] { bmc::Wire::encodeMessage(sender, to, "", "", 0); },
         [&] { oracle::wireEncode(os, asOracle(to), "", "", 0, {}); });
    same([&] { bmc::Wire::encodeBroadcast(sender, "a\nb", "", kExpiry); },
         [&] { oracle::encodeBroadcast(os, "a\nb", "", kExpiry); });
}

TEST(wire_decode_cost_is_reported) {
    // Not a pass/fail check but a look: scanning is one failing decode per object and identity.
    const auto sender = identityOf(4), recipient = identityOf(4), other = identityOf(4);
    const auto object = bmc::Wire::encodeMessage(sender, bmc::Wire::publicIdentity(recipient), "s",
                                                 "body", kExpiry);
    const auto oldOther = asOracle(other);
    using Clock = std::chrono::steady_clock;
    const auto time = [](const std::function<void()> &f) {
        const auto start = Clock::now();
        for (int i = 0; i < 3000; ++i)
            f();
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    };
    const auto ours = time([&] { (void)bmc::Wire::decodeMessage(object, other); });
    const auto old = time([&] { (void)oracle::wireDecode(object, oldOther); });
    std::printf("3000 decodes that fail: core %lld ms, old %lld ms\n", static_cast<long long>(ours),
                static_cast<long long>(old));
    CHECK(ours < old * 3 + 200); // only a gross slowdown fails
}

TEST(wire_decoders_agree_with_the_old_code_on_damaged_objects) {
    std::deque<bmc::Identity> ids;
    struct Sample {
        const char *kind;
        bmc::Bytes object;
        std::function<std::string(const bmc::Bytes &)> ours, old;
    };
    std::vector<Sample> samples;
    auto &recipient = ids.emplace_back(identityOf(4));
    auto &sender = ids.emplace_back(identityOf(3));
    const auto to = bmc::Wire::publicIdentity(recipient);
    const bmc::Identity *r = &recipient;
    samples.push_back(
        {"message",
         bmc::Wire::encodeMessage(sender, to, "subject", "body text", kExpiry,
                                  bmc::Wire::acknowledgment(token(), kExpiry)),
         [r](const bmc::Bytes &o) { return describe(bmc::Wire::decodeMessage(o, *r)); },
         [r](const bmc::Bytes &o) { return describe(oracle::wireDecode(o, asOracle(*r))); }});
    for (int v = 2; v <= 4; ++v) {
        const bmc::Identity *id = &ids.emplace_back(identityOf(v));
        samples.push_back(
            {"pubkey", bmc::Wire::encodePubkey(*id, kExpiry),
             [id](const bmc::Bytes &o) {
                 return describe(bmc::Wire::decodePubkey(o, id->address));
             },
             [id](const bmc::Bytes &o) { return describe(oracle::decodePubkey(o, id->address)); }});
        samples.push_back({"broadcast",
                           bmc::Wire::encodeBroadcast(*id, "News", "Broadcast body", kExpiry),
                           [id](const bmc::Bytes &o) {
                               return describe(bmc::Wire::decodeBroadcast(o, id->address));
                           },
                           [id](const bmc::Bytes &o) {
                               return describe(oracle::decodeBroadcast(o, id->address));
                           }});
        samples.push_back(
            {"request", bmc::Wire::getPubkey(id->address, kExpiry),
             [id](const bmc::Bytes &o) {
                 return std::string(bmc::Wire::requestsIdentity(o, *id) ? "yes" : "no");
             },
             [id](const bmc::Bytes &o) {
                 return std::string(oracle::requestsIdentity(o, asOracle(*id)) ? "yes" : "no");
             }});
    }
    samples.push_back(
        {"ack", bmc::Wire::acknowledgment(token(), kExpiry),
         [](const bmc::Bytes &o) {
             const auto h = bmc::Wire::header(o);
             return bmc::hex(bmc::Wire::acknowledgmentToken(o)) +
                    (h ? std::to_string(h->headerSize) + "/" + std::to_string(h->expires) : "-");
         },
         [](const bmc::Bytes &o) {
             const auto h = oracle::header(o);
             return bmc::hex(oracle::acknowledgmentToken(o)) +
                    (h ? std::to_string(h->headerSize) + "/" + std::to_string(h->expires) : "-");
         }});
    for (const auto &sample : samples) {
        const auto check = [&](const bmc::Bytes &object) {
            const auto ours = sample.ours(object), old = sample.old(object);
            if (ours != old)
                bmc::test::fail(__FILE__, __LINE__,
                                std::string(sample.kind) + " differs for " + bmc::hex(object) +
                                    "\n  old:  " + old + "\n  core: " + ours);
        };
        check(sample.object);
        for (size_t n = 0; n < sample.object.size(); ++n) // every truncation
            check(bmc::Bytes(sample.object.begin(), sample.object.begin() + n));
        for (int round = 0; round < 300; ++round) { // damage anywhere
            auto damaged = sample.object;
            for (int edits = 1 + int(rng() % 3); edits > 0; --edits)
                damaged[rng() % damaged.size()] ^= uint8_t(1 << (rng() % 8));
            if (rng() % 8 == 0)
                damaged.resize(rng() % damaged.size());
            else if (rng() % 8 == 0)
                damaged.push_back(uint8_t(rng()));
            check(damaged);
        }
        check(bmc::Bytes(262145, 0));
    }
}
