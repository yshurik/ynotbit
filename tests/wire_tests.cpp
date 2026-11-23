// The Qt face of bmc::Wire (src/protocol_wire.cpp and src/qt_bridge.h): each function once, with the
// QByteArray, QString and Qt structs it takes and gives. What the wire format is, what it refuses and
// the independent Python fixtures it reads are tests/bmclient/wire_test.cpp's.
#include "protocol.h"
#include "protocol_wire.h"
#include <QCoreApplication>
#include <iostream>
using namespace bm;
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
static Identity fixed(int signing, int encryption, int version) {
    QByteArray s(32, 0), e(32, 0);
    s[31] = signing;
    e[31] = encryption;
    return Protocol::fromPrivate("fixture", s, e, version);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        require(sodium_init() >= 0, "sodium init");
        auto sender = fixed(1, 2, 3), recipient = fixed(3, 4, 4);
        auto chan = Protocol::channel("general", "general", 3);
        require(chan.address == "BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr", "reference chan address");
        auto cp = Wire::publicIdentity(chan);
        require(cp.chan && cp.signingKey.size() == 65 && cp.encryptionKey.size() == 65,
                "public chan");
        auto channelMsg = Wire::encodeMessage(sender, cp, "chan", "body", 1700000000);
        require(Wire::decodeMessage(channelMsg, chan)->message.folder == "Channels",
                "chan receive");
        QByteArray token;
        for (int i = 0; i < 32; ++i)
            token.append(char(i));
        auto ack = Wire::acknowledgment(token, 1700000000);
        ack[7] = 19;
        require(Wire::acknowledgmentToken(ack) == token, "ack token");
        require(Wire::acknowledgmentToken(ack + 'x').isEmpty(), "ack exact length");
        auto publicRecipient = Wire::publicIdentity(recipient);
        auto message = Wire::encodeMessage(sender, publicRecipient, "Hello", "Unicode \xc3\xa6",
                                           1700000000, ack);
        auto decoded = Wire::decodeMessage(message, recipient);
        require(decoded && decoded->message.subject == "Hello" &&
                    decoded->message.body == "Unicode \xc3\xa6" &&
                    decoded->sender.address == sender.address && decoded->acknowledgment == ack,
                "public recipient roundtrip");
        require(!Wire::decodeMessage(message, sender), "wrong recipient");
        {
            // The largest letter ynotbit sends still fits one object, whatever
            // the text's make-up; longer texts from other clients are read.
            const auto subject = QString("Full");
            const auto body = QString(kMaxLetterText - letterTextBytes(subject, {}), 'x');
            require(letterTextBytes(subject, body) == kMaxLetterText, "limit-sized letter");
            auto full = Wire::encodeMessage(sender, publicRecipient, subject, body, 1700000000, ack);
            require(full.size() <= kMaxObjectBytes, "a limit-sized msg fits in one object");
            auto back = Wire::decodeMessage(full, recipient);
            require(back && back->message.body.size() > 200000, "texts over 200,000 bytes are read");
            auto cast = Wire::encodeBroadcast(sender, subject, body, 1700000000);
            require(cast.size() <= kMaxObjectBytes, "a limit-sized broadcast fits in one object");
        }
        for (int v = 2; v <= 4; ++v) {
            auto i = fixed(1, 2, v), other = fixed(3, 4, v);
            require(Wire::validAddress(i.address), "valid address");
            auto pub = Wire::encodePubkey(i, 1700000000);
            auto p = Wire::decodePubkey(pub, i.address);
            require(p && p->address == i.address && p->nonceTrials == 1000 && p->extraBytes == 1000,
                    "pubkey roundtrip");
            require(!Wire::decodePubkey(pub, other.address), "pubkey address mismatch");
            pub[pub.size() - 1] ^= 1;
            require(!Wire::decodePubkey(pub, i.address), "tampered pubkey");
            auto request = Wire::getPubkey(i.address, 1700000000);
            request[7] = 7;
            require(Wire::requestsIdentity(request, i) && !Wire::requestsIdentity(request, other),
                    "getpubkey matching");
            auto broadcast = Wire::encodeBroadcast(i, "News", "Broadcast body", 1700000000);
            auto b = Wire::decodeBroadcast(broadcast, i.address);
            require(b && b->broadcast && b->message.subject == "News" &&
                        b->sender.address == i.address,
                    "broadcast roundtrip");
            require(!Wire::decodeBroadcast(broadcast, other.address), "wrong subscription");
        }
        auto huge = QByteArray(262145, 0);
        require(!Wire::header(huge) && !Wire::decodeMessage(huge, recipient), "object bound");
        require(!Wire::validAddress("not an address") &&
                    !Wire::validAddress(sender.address + QChar(0)),
                "invalid addresses");
        std::cout << "PASS: the Qt face of the wire format: messages, chan, ack frames, pubkeys "
                     "v2/v3/v4, broadcasts, the object bound\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
