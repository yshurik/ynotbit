// What the old Qt code answered, kept as files (made by tests/legacy/golden_gen.cpp at the end of
// slice 1, from the old protocol, wire and proof-of-work code). Fields are separated by '|'; text
// and bytes are hexadecimal.
#include <algorithm>
#include <bmc/bytes.h>
#include <bmc/phrase.h>
#include <bmc/pow.h>
#include <bmc/protocol.h>
#include <bmc/test.h>
#include <bmc/wire.h>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef BMC_GOLDEN_DIR
#error "BMC_GOLDEN_DIR must name tests/golden"
#endif

namespace {
using Record = std::vector<std::string>;

std::vector<Record> records(const char *file) {
    std::ifstream in(std::string(BMC_GOLDEN_DIR) + "/" + file);
    if (!in)
        bmc::test::fail(__FILE__, __LINE__, std::string("cannot read ") + file);
    std::vector<Record> all;
    std::string text;
    while (std::getline(in, text)) {
        Record r;
        std::stringstream fields(text);
        std::string field;
        while (std::getline(fields, field, '|'))
            r.push_back(field);
        if (!text.empty() && text.back() == '|')
            r.push_back("");
        all.push_back(std::move(r));
    }
    return all;
}
std::string raw(const std::string &hexText) {
    return bmc::stringOf(bmc::unhex(hexText));
}
bmc::Identity identityOf(const std::string &address, const std::string &chan,
                         const std::string &keysHex) {
    bmc::Identity id;
    id.address = address;
    id.chan = chan == "1";
    const auto keys = bmc::unhex(keysHex);
    CHECK_EQ(keys.size(), size_t(64));
    std::copy(keys.begin(), keys.end(), id.keys.data());
    return id;
}
std::string keysOf(const bmc::Identity &id) {
    return bmc::hex(bmc::ByteView(id.keys.data(), 64));
}
} // namespace

TEST(golden_derivations_and_deterministic_objects_are_what_the_old_code_made) {
    int seen = 0;
    for (const auto &r : records("derivations.txt")) {
        ++seen;
        const auto &kind = r.at(0);
        if (kind == "fromPrivate") {
            const auto id =
                bmc::Protocol::fromPrivate("g", bmc::unhex(r.at(1)), bmc::unhex(r.at(2)),
                                           std::stoi(r.at(3)), 1, r.at(4) == "1");
            CHECK_EQ(id.address, r.at(5));
            CHECK_EQ(keysOf(id), r.at(6));
        } else if (kind == "channel") {
            const auto id = bmc::Protocol::channel(raw(r.at(1)), "g", std::stoi(r.at(2)));
            CHECK_EQ(id.address, r.at(3));
            CHECK_EQ(keysOf(id), r.at(4));
        } else if (kind == "phrase") {
            const auto id = bmc::phrase::identity(raw(r.at(1)), "g", std::stoull(r.at(2)));
            CHECK_EQ(id.address, r.at(3));
            CHECK_EQ(keysOf(id), r.at(4));
        } else if (kind == "firstPair") {
            const auto found =
                bmc::phrase::firstPair(raw(r.at(1)), std::stoi(r.at(2)), std::stoull(r.at(3)));
            CHECK_EQ(found ? std::to_string(*found) : std::string("none"), r.at(4));
        } else if (kind == "zeroBytes") {
            CHECK_EQ(std::to_string(bmc::phrase::zeroBytes(raw(r.at(1)))), r.at(2));
        } else if (kind == "validAddress") {
            CHECK_EQ(std::string(bmc::Wire::validAddress(raw(r.at(1))) ? "1" : "0"), r.at(2));
        } else if (kind == "inventoryHash") {
            CHECK_EQ(bmc::Protocol::inventoryHash(bmc::unhex(r.at(1))), r.at(2));
        } else if (kind == "trialValue") {
            const auto initial = bmc::unhex(r.at(2));
            CHECK_EQ(
                std::to_string(bmc::ProofOfWork::trialValue(std::stoull(r.at(1)), initial.data())),
                r.at(3));
        } else if (kind == "getPubkey") {
            CHECK_EQ(bmc::hex(bmc::Wire::getPubkey(raw(r.at(1)), std::stoll(r.at(2)))), r.at(3));
        } else if (kind == "acknowledgment") {
            CHECK_EQ(bmc::hex(bmc::Wire::acknowledgment(bmc::unhex(r.at(1)), std::stoll(r.at(2)))),
                     r.at(3));
        } else if (kind == "ackToken") {
            const auto token = bmc::Wire::acknowledgmentToken(bmc::unhex(r.at(1)));
            CHECK_EQ(token.empty() ? std::string("none") : bmc::hex(token), r.at(2));
        } else if (kind == "frame") {
            CHECK_EQ(bmc::hex(bmc::Wire::frame(bmc::unhex(r.at(1)))), r.at(2));
        } else if (kind == "powValid") {
            const auto object = bmc::unhex(r.at(1));
            const auto header = bmc::Wire::header(object);
            CHECK(header.has_value());
            const bool valid =
                bmc::ProofOfWork::valid(object, header->expires + std::stoll(r.at(2)),
                                        std::stoull(r.at(3)), std::stoull(r.at(4)));
            CHECK_EQ(std::string(valid ? "1" : "0"), r.at(5));
        } else {
            bmc::test::fail(__FILE__, __LINE__, "unknown record " + kind);
        }
    }
    CHECK(seen > 300);
}

TEST(golden_objects_made_by_the_old_code_are_read_and_the_damaged_refused) {
    int messages = 0, legacy = 0, pubkeys = 0, broadcasts = 0, rejected = 0;
    for (const auto &r : records("objects.txt")) {
        const auto &kind = r.at(0);
        if (kind == "message") {
            const auto recipient = identityOf(r.at(1), r.at(2), r.at(3));
            const auto decoded = bmc::Wire::decodeMessage(bmc::unhex(r.at(4)), recipient);
            CHECK(decoded.has_value());
            CHECK_EQ(decoded->message.subject, raw(r.at(5)));
            CHECK_EQ(decoded->message.body, raw(r.at(6)));
            CHECK_EQ(decoded->message.from, r.at(7));
            CHECK_EQ(bmc::hex(decoded->acknowledgment), r.at(8));
            CHECK_EQ(decoded->message.hash, r.at(9));
            CHECK_EQ(decoded->message.to, recipient.address);
            CHECK_EQ(decoded->message.folder, std::string(recipient.chan ? "Channels" : "Inbox"));
            ++messages;
        } else if (kind == "legacy") {
            const auto recipient = identityOf(r.at(1), r.at(2), r.at(3));
            const auto decoded = bmc::Protocol::decodeMessage(bmc::unhex(r.at(4)), recipient);
            CHECK(decoded.has_value());
            CHECK_EQ(decoded->subject, raw(r.at(5)));
            CHECK_EQ(decoded->body, raw(r.at(6)));
            CHECK_EQ(decoded->from, r.at(7));
            CHECK_EQ(decoded->folder, r.at(8));
            CHECK_EQ(decoded->hash, r.at(9));
            ++legacy;
        } else if (kind == "pubkey") {
            const auto p = bmc::Wire::decodePubkey(bmc::unhex(r.at(2)), r.at(1));
            CHECK(p.has_value());
            CHECK_EQ(p->address, r.at(1));
            CHECK_EQ(bmc::hex(p->signingKey), r.at(3));
            CHECK_EQ(bmc::hex(p->encryptionKey), r.at(4));
            CHECK_EQ(std::to_string(p->nonceTrials), r.at(5));
            CHECK_EQ(std::to_string(p->extraBytes), r.at(6));
            CHECK_EQ(std::to_string(p->behaviors), r.at(7));
            ++pubkeys;
        } else if (kind == "broadcast") {
            const auto b = bmc::Wire::decodeBroadcast(bmc::unhex(r.at(2)), r.at(1));
            CHECK(b.has_value() && b->broadcast);
            CHECK_EQ(b->message.subject, raw(r.at(3)));
            CHECK_EQ(b->message.body, raw(r.at(4)));
            CHECK_EQ(b->message.hash, r.at(5));
            ++broadcasts;
        } else if (kind == "reject-message") {
            const auto recipient = identityOf(r.at(1), r.at(2), r.at(3));
            CHECK(!bmc::Wire::decodeMessage(bmc::unhex(r.at(4)), recipient));
            ++rejected;
        } else if (kind == "reject-pubkey") {
            CHECK(!bmc::Wire::decodePubkey(bmc::unhex(r.at(2)), r.at(1)));
            ++rejected;
        } else if (kind == "reject-broadcast") {
            CHECK(!bmc::Wire::decodeBroadcast(bmc::unhex(r.at(2)), r.at(1)));
            ++rejected;
        } else {
            bmc::test::fail(__FILE__, __LINE__, "unknown record " + kind);
        }
    }
    CHECK(messages >= 24 && legacy >= 24 && pubkeys >= 6 && broadcasts >= 6 && rejected >= 60);
}
