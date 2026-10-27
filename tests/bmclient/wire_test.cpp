#include "wire_fixtures.h"
#include <algorithm>
#include <bmc/bytes.h>
#include <bmc/protocol.h>
#include <bmc/test.h>
#include <bmc/wire.h>

using bmc::Wire;

namespace {
bmc::Identity fixed(int signing, int encryption, int version) {
    bmc::Bytes s(32, 0), e(32, 0);
    s[31] = uint8_t(signing);
    e[31] = uint8_t(encryption);
    return bmc::Protocol::fromPrivate("fixture", s, e, version);
}
bmc::Bytes token() {
    bmc::Bytes t;
    for (int i = 0; i < 32; ++i)
        t.push_back(uint8_t(i));
    return t;
}
bmc::ByteView prefix(const bmc::Bytes &b, size_t n) {
    return bmc::ByteView(b).first(n);
}
bmc::Bytes ackObject() {
    auto ack = Wire::acknowledgment(token(), 1700000000);
    ack[7] = 19;
    return ack;
}
} // namespace

TEST(wire_chan_ack_and_public_recipient_round_trip) {
    const auto sender = fixed(1, 2, 3), recipient = fixed(3, 4, 4);
    const auto chan = bmc::Protocol::channel("general", "general", 3);
    CHECK_EQ(chan.address, "BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr"); // reference chan address
    const auto cp = Wire::publicIdentity(chan);
    CHECK(cp.chan && cp.signingKey.size() == 65 && cp.encryptionKey.size() == 65);
    const auto channelMsg = Wire::encodeMessage(sender, cp, "chan", "body", 1700000000);
    CHECK_EQ(Wire::decodeMessage(channelMsg, chan)->message.folder, "Channels");

    const auto ack = ackObject();
    CHECK_EQ(Wire::acknowledgmentToken(ack), token());
    auto longer = ack;
    longer.push_back('x');
    CHECK(Wire::acknowledgmentToken(longer).empty()); // exact length

    const auto publicRecipient = Wire::publicIdentity(recipient);
    const auto message =
        Wire::encodeMessage(sender, publicRecipient, "Hello", "Unicode \xc3\xa6", 1700000000, ack);
    const auto decoded = Wire::decodeMessage(message, recipient);
    CHECK(decoded.has_value());
    CHECK_EQ(decoded->message.subject, "Hello");
    CHECK_EQ(decoded->message.body, "Unicode \xc3\xa6");
    CHECK_EQ(decoded->sender.address, sender.address);
    CHECK_EQ(decoded->acknowledgment, ack);
    CHECK(!Wire::decodeMessage(message, sender)); // not for the sender
}

TEST(wire_largest_letter_fits_one_object) {
    // The largest letter ynotbit sends still fits one object, whatever the text's make-up.
    const auto sender = fixed(1, 2, 3), recipient = fixed(3, 4, 4);
    const auto publicRecipient = Wire::publicIdentity(recipient);
    const std::string subject = "Full";
    const std::string body(bmc::kMaxLetterText - bmc::letterTextBytes(subject, {}), 'x');
    CHECK_EQ(bmc::letterTextBytes(subject, body), bmc::kMaxLetterText);
    const auto full =
        Wire::encodeMessage(sender, publicRecipient, subject, body, 1700000000, ackObject());
    CHECK(full.size() <= size_t(bmc::kMaxObjectBytes));
    const auto back = Wire::decodeMessage(full, recipient);
    CHECK(back && back->message.body.size() > 200000);
    const auto cast = Wire::encodeBroadcast(sender, subject, body, 1700000000);
    CHECK(cast.size() <= size_t(bmc::kMaxObjectBytes));
}

// The scan tells a broadcast's address from its tag, as the thousands of addresses a mailbox may
// listen for are too many to try one by one.
TEST(a_broadcast_is_told_from_its_address_by_its_tag) {
    for (int v = 2; v <= 4; ++v) {
        const auto i = fixed(1, 2, v), other = fixed(3, 4, v);
        const auto &fixture = v == 2   ? referenceBroadcast2
                              : v == 3 ? referenceBroadcast3
                                       : referenceBroadcast4;
        const auto broadcast = Wire::encodeBroadcast(i, "News", "Body", 1700000000);
        const auto mine = Wire::tagOfAddress(i.address);
        CHECK(mine.has_value());
        if (v < 4) {
            // Before version 4 an address's broadcasts are version 4 objects, which carry no tag.
            CHECK(mine->empty());
            CHECK(Wire::tagOfBroadcast(broadcast).empty());
            CHECK(Wire::tagOfBroadcast(fixture).empty());
        } else {
            CHECK_EQ(mine->size(), size_t(32));
            CHECK(Wire::tagOfBroadcast(broadcast) == *mine);
            CHECK(Wire::tagOfBroadcast(fixture) == *mine); // the independent Python broadcast
            CHECK(*Wire::tagOfAddress(other.address) != *mine);
        }
    }
    CHECK(!Wire::tagOfAddress("BM-garbage").has_value());
    CHECK(!Wire::tagOfAddress("").has_value());
    CHECK(Wire::tagOfBroadcast(referenceMessage).empty());    // an object, but not a broadcast
    CHECK(Wire::tagOfBroadcast(bmc::Bytes{1, 2, 3}).empty()); // no object at all
    auto cut = Wire::encodeBroadcast(fixed(1, 2, 4), "News", "Body", 1700000000);
    cut.resize(30); // a broadcast with no room for its tag
    CHECK(Wire::tagOfBroadcast(cut).empty());
}

TEST(wire_and_the_simple_protocol_read_each_other) {
    const auto sender = fixed(1, 2, 3), recipient = fixed(3, 4, 4);
    const auto message = Wire::encodeMessage(sender, Wire::publicIdentity(recipient), "Hello", "x",
                                             1700000000, ackObject());
    const auto oldDecoded = bmc::Protocol::decodeMessage(message, recipient);
    CHECK(oldDecoded && oldDecoded->subject == "Hello"); // legacy notbit decrypt compatibility
    const auto oldEncoded =
        bmc::Protocol::encodeMessage(sender, recipient, "Legacy", "old engine", 1700000000);
    CHECK_EQ(Wire::decodeMessage(oldEncoded, recipient)->message.subject, "Legacy");
}

TEST(wire_reads_the_independent_python_fixtures_and_refuses_the_forged) {
    const auto sender = fixed(1, 2, 3), recipient = fixed(3, 4, 4);
    const auto reference = Wire::decodeMessage(referenceMessage, recipient);
    CHECK(reference.has_value());
    CHECK_EQ(reference->message.subject, "Reference");
    CHECK_EQ(reference->message.body, "Independent Python fixture");
    CHECK_EQ(reference->sender.address, sender.address);
    CHECK_EQ(Wire::acknowledgmentToken(reference->acknowledgment), token());
    CHECK(!Wire::decodeMessage(badAckMessage, recipient));    // signed invalid nested ack checksum
    CHECK(!Wire::decodeMessage(wrongRipeMessage, recipient)); // signed wrong destination RIPE
    CHECK(!Wire::decodeMessage(noncanonicalMessage,
                               recipient)); // signed noncanonical plaintext varint
    for (size_t n = 0; n < referenceMessage.size(); ++n)
        CHECK(!Wire::decodeMessage(prefix(referenceMessage, n), recipient)); // truncated message
}

TEST(wire_pubkeys_and_broadcasts_of_every_version) {
    for (int v = 2; v <= 4; ++v) {
        const auto i = fixed(1, 2, v), other = fixed(3, 4, v);
        CHECK(Wire::validAddress(i.address));
        auto pub = Wire::encodePubkey(i, 1700000000);
        auto p = Wire::decodePubkey(pub, i.address);
        CHECK(p && p->address == i.address && p->nonceTrials == 1000 && p->extraBytes == 1000);
        CHECK(!Wire::decodePubkey(pub, other.address)); // address mismatch
        const auto &fixture = v == 2   ? referencePubkey2
                              : v == 3 ? referencePubkey3
                                       : referencePubkey4;
        CHECK(Wire::decodePubkey(fixture, i.address)
                  .has_value()); // independent Python pubkey fixture
        pub.back() ^= 1;
        CHECK(!Wire::decodePubkey(pub, i.address)); // tampered pubkey
        auto request = Wire::getPubkey(i.address, 1700000000);
        request[7] = 7;
        CHECK(Wire::requestsIdentity(request, i) && !Wire::requestsIdentity(request, other));
        auto longRequest = request;
        longRequest.push_back('x');
        CHECK(!Wire::requestsIdentity(longRequest, i)); // getpubkey trailing data
        auto broadcast = Wire::encodeBroadcast(i, "News", "Broadcast body", 1700000000);
        auto b = Wire::decodeBroadcast(broadcast, i.address);
        CHECK(b && b->broadcast && b->message.subject == "News" && b->sender.address == i.address);
        CHECK(!Wire::decodeBroadcast(broadcast, other.address)); // wrong subscription
        const auto &bf = v == 2   ? referenceBroadcast2
                         : v == 3 ? referenceBroadcast3
                                  : referenceBroadcast4;
        CHECK(Wire::decodeBroadcast(bf, i.address)
                  .has_value()); // independent Python broadcast fixture
        broadcast.back() ^= 1;
        CHECK(!Wire::decodeBroadcast(broadcast, i.address)); // tampered broadcast
        for (size_t n = 0; n < fixture.size(); ++n)
            CHECK(!Wire::decodePubkey(prefix(fixture, n), i.address)); // truncated pubkey
        for (size_t n = 0; n < bf.size(); ++n)
            CHECK(!Wire::decodeBroadcast(prefix(bf, n), i.address)); // truncated broadcast
    }
}

TEST(wire_refuses_noncanonical_oversized_and_malformed_input) {
    const auto sender = fixed(1, 2, 3), recipient = fixed(3, 4, 4);
    const auto publicRecipient = Wire::publicIdentity(recipient);
    const auto ack = ackObject();
    const auto message =
        Wire::encodeMessage(sender, publicRecipient, "Hello", "x", 1700000000, ack);
    for (size_t n = 8; n < message.size(); n += 7) {
        auto corrupt = message;
        corrupt[n] ^= 1;
        CHECK(!Wire::decodeMessage(corrupt, recipient)); // tampered message header or ECIES
    }
    bmc::Bytes noncanonical(ack.begin(), ack.begin() + 20);
    bmc::append(noncanonical, bmc::unhex("fd000101"));
    bmc::append(noncanonical, token());
    CHECK(!Wire::header(noncanonical)); // a noncanonical header varint
    const bmc::Bytes huge(262145, 0);
    CHECK(!Wire::header(huge) && !Wire::decodeMessage(huge, recipient)); // object bound
    auto badPoint = publicRecipient;
    std::fill(badPoint.encryptionKey.begin(), badPoint.encryptionKey.end(), 0);
    CHECK_THROWS(Wire::encodeMessage(sender, badPoint, "", "", 1700000000));
    auto badAddress = publicRecipient;
    badAddress.address = sender.address;
    CHECK_THROWS(Wire::encodeMessage(sender, badAddress, "", "", 1700000000));
    CHECK_THROWS(Wire::encodeMessage(sender, publicRecipient, "bad\nsubject", "", 1700000000));
    CHECK_THROWS(Wire::encodeMessage(sender, publicRecipient, "",
                                     std::string(bmc::kMaxLetterText, 'x'), 1700000000));
    CHECK_THROWS(Wire::encodeMessage(sender, publicRecipient, "", "\xff", 1700000000)); // not UTF-8
    CHECK_THROWS(Wire::acknowledgment(bmc::Bytes(31, 0), 1700000000));
    CHECK(!Wire::validAddress("not an address"));
    CHECK(!Wire::validAddress(sender.address + std::string(1, '\0')));
    CHECK(
        !Wire::validAddress(sender.address.substr(0, 10) + "\xc3\xa9" + sender.address.substr(12)));
    CHECK(!Wire::validAddress(""));
}

TEST(wire_text_is_read_the_way_qstring_read_it) {
    const auto sender = fixed(1, 2, 3), recipient = fixed(3, 4, 4);
    const auto to = Wire::publicIdentity(recipient);
    const auto read = [&](const std::string &raw) {
        const auto object = Wire::encodeMessageText(sender, to, bmc::view(raw), 1700000000);
        return Wire::decodeMessage(object, recipient)->message;
    };
    CHECK_EQ(read("\xef\xbb\xbfplain").body, "plain"); // a leading byte order mark is dropped
    CHECK_EQ(read("Subject:s\nBody:\xef\xbb\xbf"
                  "b")
                 .body,
             "\xef\xbb\xbf"
             "b"); // one further on is kept
    CHECK_EQ(read("Subject:s\nBody:\xff\xfe").body,
             "\xef\xbf\xbd\xef\xbf\xbd"); // one U+FFFD per bad byte
    CHECK_EQ(read("Subject:s\nBody:\xe2\x82").body, "\xef\xbf\xbd\xef\xbf\xbd");
    CHECK_EQ(read("no subject at all").body, "no subject at all");
    CHECK_EQ(read("Subject:only").body, "Subject:only"); // no "\nBody:": the whole text is the body
    CHECK_EQ(read("Subject:a\nBody:b\nBody:c").subject, "a"); // the first "\nBody:" splits
    CHECK_EQ(read("Subject:a\nBody:b\nBody:c").body, "b\nBody:c");
    CHECK_EQ(read("Subject:\nBody:").body, "");
    // A body that begins with U+FEFF is kept as sent when it follows "Subject:...\nBody:".
    const auto bom = Wire::encodeMessage(sender, to, "",
                                         "\xef\xbb\xbf"
                                         "kept",
                                         1700000000);
    CHECK_EQ(Wire::decodeMessage(bom, recipient)->message.body, "\xef\xbb\xbf"
                                                                "kept");
}
