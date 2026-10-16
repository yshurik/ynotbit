#include "legacy/oracle.h"
#include <algorithm>
#include <bmc/bytes.h>
#include <bmc/phrase.h>
#include <bmc/protocol.h>
#include <bmc/test.h>
#include <random>

namespace {
std::mt19937_64 rng(0x5eed0009);

bmc::Bytes key32() {
    bmc::Bytes k(32);
    for (auto &b : k)
        b = uint8_t(rng());
    k[0] = uint8_t(1 + rng() % 0x7e); // below the group order
    return k;
}
oracle::Id asOracle(const bmc::Identity &i) {
    oracle::Id id;
    id.label = i.label;
    id.address = i.address;
    id.chan = i.chan;
    std::copy(i.keys.data(), i.keys.data() + 64, id.keys.begin());
    return id;
}
template <class F> std::string errorOf(F &&f) {
    try {
        f();
    } catch (const std::exception &e) {
        return e.what();
    }
    return "<no error>";
}
std::string describe(const bmc::Message &m) {
    return m.hash + "|" + m.from + "|" + m.to + "|" + m.subject + "|" + m.body + "|" + m.folder;
}
std::string describe(const oracle::Msg &m) {
    return m.hash + "|" + m.from + "|" + m.to + "|" + m.subject + "|" + m.body + "|" + m.folder;
}
std::string describe(const std::optional<bmc::Message> &m) {
    return m ? describe(*m) : "<none>";
}
std::string describe(const std::optional<oracle::Msg> &m) {
    return m ? describe(*m) : "<none>";
}
void sameDecode(const bmc::Bytes &object, const bmc::Identity &recipient) {
    const auto ours = describe(bmc::Protocol::decodeMessage(object, recipient));
    const auto old = describe(oracle::protocolDecode(object, asOracle(recipient)));
    if (ours != old)
        bmc::test::fail(__FILE__, __LINE__,
                        "decodeMessage differs for " + bmc::hex(object) + "\n  old:  " + old +
                            "\n  core: " + ours);
}
} // namespace

TEST(from_private_matches_the_old_code_including_its_errors) {
    for (int version = 2; version <= 4; ++version)
        for (int round = 0; round < 40; ++round) {
            const auto s = key32(), e = key32();
            const auto ours = bmc::Protocol::fromPrivate("l", s, e, version);
            const auto old = oracle::fromPrivate("l", s, e, version, 1, false);
            CHECK_EQ(ours.address, old.address);
            CHECK(std::equal(old.keys.begin(), old.keys.end(), ours.keys.data()));
        }
    const struct {
        bmc::Bytes s, e;
        int version, stream;
    } bad[] = {
        {bmc::Bytes(31, 1), bmc::Bytes(32, 2), 4, 1}, {bmc::Bytes(32, 1), bmc::Bytes(33, 2), 4, 1},
        {bmc::Bytes(32, 1), bmc::Bytes(32, 2), 1, 1}, {bmc::Bytes(32, 1), bmc::Bytes(32, 2), 5, 1},
        {bmc::Bytes(32, 1), bmc::Bytes(32, 2), 4, 2}, {bmc::Bytes(32, 0), bmc::Bytes(32, 2), 4, 1},
        {bmc::Bytes(32, 1), bmc::Bytes(32, 0), 3, 1}};
    for (const auto &b : bad)
        CHECK_EQ(errorOf([&] { bmc::Protocol::fromPrivate("x", b.s, b.e, b.version, b.stream); }),
                 errorOf([&] { oracle::fromPrivate("x", b.s, b.e, b.version, b.stream, false); }));
}

TEST(channel_matches_the_old_code) {
    for (const char *phrase : {"general", "ynotbit test phrase", "a", " ",
                               "\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c",
                               "emoji \xf0\x9f\x98\x80", "line\nbreak"})
        for (int version : {3, 4}) {
            const auto ours = bmc::Protocol::channel(phrase, "label", version);
            const auto old = oracle::channel(phrase, "label", version);
            CHECK_EQ(ours.address, old.address);
            CHECK(std::equal(old.keys.begin(), old.keys.end(), ours.keys.data()));
            CHECK(ours.chan && old.chan);
        }
    CHECK_EQ(errorOf([] { bmc::Protocol::channel("", "x", 4); }),
             errorOf([] { oracle::channel("", "x", 4); }));
    CHECK_EQ(errorOf([] { bmc::Protocol::channel("x", "x", 2); }),
             errorOf([] { oracle::channel("x", "x", 2); }));
}

TEST(inventory_hash_matches_the_old_code) {
    for (int round = 0; round < 300; ++round) {
        bmc::Bytes object(rng() % 1000);
        for (auto &b : object)
            b = uint8_t(rng());
        CHECK_EQ(bmc::Protocol::inventoryHash(object), oracle::inventoryHash(object));
    }
}

TEST(protocol_messages_cross_between_old_and_new) {
    const char *texts[] = {"", "plain", "unicode \xc3\xa9 \xd0\x9f \xf0\x9f\x98\x80", "line1\nline2\ttab",
                           "Subject:inside\nBody:odd", "x\xef\xbb\xbf" "bom inside" /* a leading one would be dropped by the oracle's QString::fromUtf8 before encoding */};
    for (int version = 2; version <= 4; ++version)
        for (int round = 0; round < 12; ++round) {
            const auto sender = bmc::Protocol::fromPrivate("s", key32(), key32(), version);
            const auto recipient =
                bmc::Protocol::fromPrivate("r", key32(), key32(), 2 + int(rng() % 3));
            const std::string subject = round % 3 ? "subject " + std::to_string(round) : "";
            const std::string body = texts[rng() % std::size(texts)];
            // new encodes, old decodes
            const auto object =
                bmc::Protocol::encodeMessage(sender, recipient, subject, body, 1700000000);
            const auto inOld = oracle::protocolDecode(object, asOracle(recipient));
            CHECK(inOld.has_value());
            sameDecode(object, recipient);
            // old encodes, new decodes
            const auto oldObject = oracle::protocolEncode(asOracle(sender), asOracle(recipient),
                                                          subject, body, 1700000000);
            const auto inNew = bmc::Protocol::decodeMessage(oldObject, recipient);
            CHECK(inNew.has_value());
            sameDecode(oldObject, recipient);
            CHECK_EQ(inNew->subject, subject);
            CHECK_EQ(inNew->body, body);
            // and the same error for the same bad input
            CHECK_EQ(
                errorOf([&] { bmc::Protocol::encodeMessage(sender, recipient, "a\nb", body, 1); }),
                errorOf([&] {
                    oracle::protocolEncode(asOracle(sender), asOracle(recipient), "a\nb", body, 1);
                }));
        }
}

TEST(protocol_decode_agrees_with_the_old_code_on_damaged_objects) {
    const auto sender = bmc::Protocol::fromPrivate("s", key32(), key32(), 4);
    const auto recipient = bmc::Protocol::fromPrivate("r", key32(), key32(), 4);
    const auto object =
        bmc::Protocol::encodeMessage(sender, recipient, "subject", "body text", 1700000000);
    for (size_t n = 0; n <= object.size(); ++n)
        sameDecode(bmc::Bytes(object.begin(), object.begin() + n), recipient);
    for (int round = 0; round < 1500; ++round) {
        auto damaged = object;
        for (int edits = 1 + int(rng() % 3); edits > 0; --edits)
            damaged[rng() % damaged.size()] ^= uint8_t(1 << (rng() % 8));
        if (rng() % 8 == 0)
            damaged.resize(rng() % damaged.size());
        sameDecode(damaged, recipient);
    }
    sameDecode(bmc::Bytes(262145, 0), recipient);
}

TEST(phrase_functions_match_the_old_code) {
    for (const char *phrase : {"ynotbit test phrase", "general", "",
                               "\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c \xc3\xbcn\xc3\xaf"
                               "code"}) {
        for (uint64_t pair : {0, 1, 2, 60, 252, 1000}) {
            const auto ours = bmc::phrase::identity(phrase, "l", pair);
            const auto old = oracle::phraseIdentity(phrase, "l", pair);
            CHECK_EQ(ours.address, old.address);
            CHECK(std::equal(old.keys.begin(), old.keys.end(), ours.keys.data()));
            CHECK_EQ(bmc::phrase::zeroBytes(ours.address), oracle::zeroBytes(ours.address));
        }
        CHECK(bmc::phrase::firstPair(phrase, 1, 3000) == oracle::firstPair(phrase, 1, 3000));
        CHECK(bmc::phrase::firstPair(phrase, 2, 3000) == oracle::firstPair(phrase, 2, 3000));
    }
    for (const char *address : {"", "nonsense", "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN", "BM-",
                                "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6 "})
        CHECK_EQ(bmc::phrase::zeroBytes(address), oracle::zeroBytes(address));
}
