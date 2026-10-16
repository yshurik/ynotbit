#include <bmc/bytes.h>
#include <bmc/clock.h>
#include <bmc/hash.h>
#include <bmc/phrase.h>
#include <bmc/protocol.h>
#include <bmc/test.h>
#include <optional>

namespace {
bmc::Identity fixed(int signing, int encryption, int version) {
    bmc::Bytes s(32, 0), e(32, 0);
    s[31] = uint8_t(signing);
    e[31] = uint8_t(encryption);
    return bmc::Protocol::fromPrivate("fixture", s, e, version);
}
} // namespace

TEST(channel_address_matches_the_reference) {
    const auto chan = bmc::Protocol::channel("general", "general", 3);
    CHECK_EQ(chan.address, "BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr");
    CHECK(chan.chan);
    CHECK_EQ(chan.label, "general");
    CHECK_THROWS(bmc::Protocol::channel("", "x", 4));
    CHECK_THROWS(bmc::Protocol::channel("x", "x", 2));
}

TEST(from_private_makes_a_stable_identity_and_rejects_bad_keys) {
    const auto a = fixed(1, 2, 3), again = fixed(1, 2, 3), other = fixed(3, 4, 3);
    CHECK_EQ(a.address, again.address);
    CHECK(a.address != other.address);
    CHECK(a.address.rfind("BM-", 0) == 0);
    CHECK(!a.chan);
    CHECK_EQ(a.label, "fixture");
    CHECK_EQ(a.keys.data()[31], 1);
    CHECK_EQ(a.keys.data()[63], 2);
    CHECK(bmc::Protocol::fromPrivate("c", bmc::Bytes(32, 1), bmc::Bytes(32, 2), 4, 1, true).chan);
    CHECK_THROWS(bmc::Protocol::fromPrivate("x", bmc::Bytes(31, 1), bmc::Bytes(32, 2)));
    CHECK_THROWS(bmc::Protocol::fromPrivate("x", bmc::Bytes(32, 1), bmc::Bytes(33, 2)));
    CHECK_THROWS(bmc::Protocol::fromPrivate("x", bmc::Bytes(32, 1), bmc::Bytes(32, 2), 1));
    CHECK_THROWS(bmc::Protocol::fromPrivate("x", bmc::Bytes(32, 1), bmc::Bytes(32, 2), 5));
    CHECK_THROWS(bmc::Protocol::fromPrivate("x", bmc::Bytes(32, 1), bmc::Bytes(32, 2), 4, 2));
    CHECK_THROWS(bmc::Protocol::fromPrivate("x", bmc::Bytes(32, 0), bmc::Bytes(32, 2)));
}

TEST(random_identity_has_a_zero_ripe_byte_and_keys_that_rebuild_its_address) {
    const auto id = bmc::Protocol::identity("random");
    CHECK(bmc::phrase::zeroBytes(id.address) >= 1);
    const auto rebuilt = bmc::Protocol::fromPrivate("random", bmc::ByteView(id.keys.data(), 32),
                                                    bmc::ByteView(id.keys.data() + 32, 32));
    CHECK_EQ(rebuilt.address, id.address);
}

TEST(inventory_hash_is_the_first_half_of_a_double_sha512) {
    const auto object = bmc::bytesOf("some object");
    const auto twice = bmc::hash(bmc::Digest::Sha512, bmc::hash(bmc::Digest::Sha512, object));
    CHECK_EQ(bmc::Protocol::inventoryHash(object), bmc::hex(bmc::ByteView(twice).first(32)));
    CHECK_EQ(bmc::Protocol::inventoryHash(object).size(), size_t(64));
    CHECK_EQ(bmc::Protocol::inventoryHash({}).size(), size_t(64));
}

TEST(protocol_message_round_trips_and_resists_damage) {
    bmc::setClockForTesting([] { return int64_t(1700000000); });
    const auto sender = fixed(1, 2, 3), recipient = fixed(3, 4, 4);
    const auto object = bmc::Protocol::encodeMessage(sender, recipient, "Legacy",
                                                     "old engine \xc3\xa9", 1700000000);
    const auto decoded = bmc::Protocol::decodeMessage(object, recipient);
    CHECK(decoded.has_value());
    CHECK_EQ(decoded->subject, "Legacy");
    CHECK_EQ(decoded->body, "old engine \xc3\xa9");
    CHECK_EQ(decoded->from, sender.address);
    CHECK_EQ(decoded->to, recipient.address);
    CHECK_EQ(decoded->folder, "Inbox");
    CHECK_EQ(decoded->received, 1700000000);
    CHECK_EQ(decoded->hash, bmc::Protocol::inventoryHash(object));
    CHECK(!bmc::Protocol::decodeMessage(object, sender)); // not for the sender
    const auto chan = bmc::Protocol::channel("general", "general", 3);
    const auto toChan = bmc::Protocol::encodeMessage(sender, chan, "", "to the chan", 1700000000);
    CHECK_EQ(bmc::Protocol::decodeMessage(toChan, chan)->folder, "Channels");
    for (size_t n = 0; n < object.size(); ++n)
        CHECK(!bmc::Protocol::decodeMessage(bmc::ByteView(object).first(n), recipient));
    for (size_t n = 8; n < object.size(); n += 7) {
        auto corrupt = object;
        corrupt[n] ^= 1;
        CHECK(!bmc::Protocol::decodeMessage(corrupt, recipient));
    }
    CHECK(!bmc::Protocol::decodeMessage(bmc::Bytes(262145, 0), recipient));
    CHECK_THROWS(bmc::Protocol::encodeMessage(sender, recipient, "bad\nsubject", "", 1700000000));
    CHECK_THROWS(bmc::Protocol::encodeMessage(sender, recipient, "bad\rsubject", "", 1700000000));
    CHECK_THROWS(bmc::Protocol::encodeMessage(sender, recipient, "",
                                              std::string(bmc::kMaxLetterText, 'x'), 1700000000));
    bmc::setClockForTesting(nullptr);
}

TEST(phrase_keys_match_the_reference_addresses) {
    // Keys from a phrase, as PyBitmessage and scripts/keys-from-phrase.py (big1) derive them.
    const std::string phrase = "ynotbit test phrase";
    CHECK(bmc::phrase::firstPair(phrase, 1) == std::optional<uint64_t>(60));
    CHECK_EQ(bmc::phrase::identity(phrase, "x", 60).address,
             "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN");
    CHECK(bmc::phrase::firstPair(phrase, 2) == std::optional<uint64_t>(27122));
    CHECK_EQ(bmc::phrase::identity(phrase, "x", 27122).address,
             "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6");
    // A phrase is its UTF-8 bytes, nothing normalised.
    const std::string unicode = "пароль ünïcode фраза";
    CHECK(bmc::phrase::firstPair(unicode, 1) == std::optional<uint64_t>(252));
    CHECK_EQ(bmc::phrase::identity(unicode, "x", 252).address,
             "BM-2cUqyPSNiBCvTg51hztE4uWrxET3386Ezo");
    const auto general = bmc::phrase::firstPair("general", 1);
    CHECK(general.has_value());
    const auto identity = bmc::phrase::identity("general", "Mine", *general);
    CHECK_EQ(identity.address, "BM-2cW67GEKkHGonXKZLCzouLLxnLym3azS8r");
    CHECK(!identity.chan);
    CHECK_EQ(identity.label, "Mine");
    CHECK_EQ(identity.address, bmc::Protocol::channel("general", "[chan] general").address);
    CHECK_EQ(bmc::phrase::zeroBytes("BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6"), 2);
    CHECK_EQ(bmc::phrase::zeroBytes("BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN"), 1);
    CHECK_EQ(bmc::phrase::zeroBytes("nonsense"), 0);
    CHECK(!bmc::phrase::firstPair(phrase, 6, 2000).has_value()); // none within the limit
}
