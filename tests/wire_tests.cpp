#include "protocol.h"
#include "protocol_wire.h"
#include <QCoreApplication>
#include <functional>
#include <iostream>
using namespace bm;
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
static void rejects(const std::function<void()> &f) {
    bool bad = false;
    try {
        f();
    } catch (const std::exception &) {
        bad = true;
    }
    require(bad, "expected exception");
}
static Identity fixed(int signing, int encryption, int version) {
    QByteArray s(32, 0), e(32, 0);
    s[31] = signing;
    e[31] = encryption;
    return Protocol::fromPrivate("fixture", s, e, version);
}
// Fixed independent Python cryptography fixtures. Scalars 1/2 sender, 3/4 recipient,
// ephemeral scalar 5, IV 000102...0f, timestamp 1700000000. SHA1 message/v3 pubkey.
static const QByteArray referenceMessage =
    QByteArray::fromHex("0000000000000000000000006553f100000000020101000102030405060708090a0b0c0d0e"
                        "0f02ca00202f8bde4d1a07"
                        "209355b4a7250a5c5128e88b84bddc619ab7cba8d569b240efe40020d8ac222636e5e3d6d4"
                        "dba9dda6c9c426f788271b"
                        "ab0d6840dca87d3aa6ac62d64a6c83dbc51996ffd5849fdce57205bfde33404ee140699459"
                        "fcec4f6a437840cfadc8a0"
                        "9b01a1f00a4ff900a334dc86cfa66e8c6b99258a7fa0137abb79413d076553a1b39e949731"
                        "631b2b4fe2e6c75ae07801"
                        "334b6f3b04f2b1fd8bbb4e93982242ea16beb099516dc01efb342bb34d256ee2d34d6eb60a"
                        "2ff5ed131f102d0b02fc6f"
                        "9f793968c5327f1ce43bdd0de49ccc28590c024da6ce65f0b42e4dc7000f779385632b1f58"
                        "05263cd27b98792b4985a3"
                        "001b52668ba66470e595dae38754e3b35ed9d46f5175235e836a136b9ab2f7a167b5f7d39e"
                        "540c6f0d423540111b258f"
                        "ceeb3bfeb9bf3e48a52bcc6bbcca78ee7296cc839b1aecf538962cb84fccc00c6aa008dcec"
                        "dfd265a40004b019f515e8"
                        "4473ed00193412ab8ea5007bd3773304e94936e29dc02b25af66ea9d70e49030f18ce29ec9"
                        "e5c0e739361dec747eedea"
                        "2d32cf8258b5826e2a8e00f64b758058c4a737ad7a156843efb3a90c2cbb203db1e4762a1e"
                        "dc967285dc964577767da1"
                        "af4105ca4781669db24017e468daa2582251ace3efd9c58e1473e986");
static const QByteArray badAckMessage =
    QByteArray::fromHex("0000000000000000000000006553f100000000020101000102030405060708090a0b0c0d0e"
                        "0f02ca00202f8bde4d1a07"
                        "209355b4a7250a5c5128e88b84bddc619ab7cba8d569b240efe40020d8ac222636e5e3d6d4"
                        "dba9dda6c9c426f788271b"
                        "ab0d6840dca87d3aa6ac62d64a6c83dbc51996ffd5849fdce57205bfde33404ee140699459"
                        "fcec4f6a437840cfadc8a0"
                        "9b01a1f00a4ff900a334dc86cfa66e8c6b99258a7fa0137abb79413d076553a1b39e949731"
                        "631b2b4fe2e6c75ae07801"
                        "334b6f3b04f2b1fd8bbb4e93982242ea16beb099516dc01efb342bb34d256ee2d34d6eb60a"
                        "2ff5ed131f102d0b02fc6f"
                        "9f793968c5327f1ce43bdd0de49ccc28590c024da6ce65f0b42e4dc7000f779385632b1f58"
                        "05263cd27b98792b4985a3"
                        "001b52668ba66470e595dae38754e3b35ed9d46f5175235e836a136b9ab2f7a167b5f7d39e"
                        "540c6f0d4235405c64b1d7"
                        "b4ecd511f3a82f062a0bd53b7a9320587244bff74841f605e542e1454dcaa01bac389504c6"
                        "2e3e3af17acdce3ef0ff1b"
                        "ba3d984e9786184698e596b3e5025acde4e1c1b4eff708559ee847cd01d79b70b1cf37a21b"
                        "8f7184efbeaecd7b821a32"
                        "7a836d18d0665e660914633a53a2430c5c0c5b21e2628cd7df051b3138f1e165c68def158b"
                        "e404db272bcb113b5abebb"
                        "3b9bc71299675dc7e152f50cee244bb3e9d30acfd4a096fa292cf552");
static const QByteArray wrongRipeMessage =
    QByteArray::fromHex("0000000000000000000000006553f100000000020101000102030405060708090a0b0c0d0e"
                        "0f02ca00202f8bde4d1a07"
                        "209355b4a7250a5c5128e88b84bddc619ab7cba8d569b240efe40020d8ac222636e5e3d6d4"
                        "dba9dda6c9c426f788271b"
                        "ab0d6840dca87d3aa6ac62d64a6c83dbc51996ffd5849fdce57205bfde33404ee140699459"
                        "fcec4f6a437840cfadc8a0"
                        "9b01a1f00a4ff900a334dc86cfa66e8c6b99258a7fa0137abb79413d076553a1b39e949731"
                        "631b2b4fe2e6c75ae07801"
                        "334b6f3b04f2b1fd8bbb4e93982242ea16beb099516dc01efb342bb34d256ee2d34d6eb60a"
                        "2ff5ed131f102dbdf5ffab"
                        "c3d22dc61324176ca0650855353cd55a509aacd925e3e4ba08218424d3609fc2674ed64492"
                        "5049eef585ac5e7ef7ead5"
                        "91d17c49fe857283af4cec31318368ab73f9a03aa183405429eb2ad2ec9d71e1e24064aca8"
                        "5cc3de0794acc277f9516e"
                        "615645aab8b7665d0212f85cfbbd8e27546f611580cd06453815bf3c150cdb987d40b10ab2"
                        "a52a633c95397a26c31ee1"
                        "0b22e374e319c6950866bffebf9792d0fa3600872ef65607061563f52cf31facdf14539171"
                        "34c476bbf9f62a15503ca4"
                        "b3f358f15f97caa55f0e76c3a2af86830cd3e1a24b725feddf0f82361c0551db70079bca9d"
                        "d4b91200956e6dce21a749"
                        "bc6067d7349fa3186fbb052c363f0ffdc2abc6d9b4a2cca288bf87e9");
static const QByteArray noncanonicalMessage =
    QByteArray::fromHex("0000000000000000000000006553f100000000020101000102030405060708090a0b0c0d0e"
                        "0f02ca00202f8bde4d1a07"
                        "209355b4a7250a5c5128e88b84bddc619ab7cba8d569b240efe40020d8ac222636e5e3d6d4"
                        "dba9dda6c9c426f788271b"
                        "ab0d6840dca87d3aa6ac62d634dcf1fd747afebb48e186da7bf1a341e47f0d989763dc7b30"
                        "48c2bb639537612086aea0"
                        "4f46315393bbe3f5b0f007d196b23992efbafbb1f3e0aca70532aa7829cd93e41c96aab08c"
                        "46ba18dd599424f8d6abfd"
                        "19eb9612b35f7baba70c19984e45f7b2ad98f2ed47392d99748621faa4cc0c8f27e1f7bdde"
                        "57f7d136573192e7080e2e"
                        "23aa85146d2cc8d8cc10e270ff97a81ed6e106c0e8429841b5dab4260a828c59f468df8d69"
                        "e715f4ca7bba2be1f47838"
                        "5d36ab8fd6dc48cf71582e979cbfdf42c21094f34141cabf339685d705c08ce5fa51dc21a5"
                        "fa055a66e3d00cad04d4ae"
                        "087eebc99cd159dc0cd6d6bd8f32bb1779e6d224e76ce740ec4b073cfb7507ca04e3ce3f30"
                        "f9fa4c2720714532249a16"
                        "c5aca101d146362664822da956956a272573f442fad0e1d5d687d626920099fa54f485e1fd"
                        "42640a1cc6633132e417cd"
                        "2331a4877496b54142190aa352a730a8348d5b251d8f4483de21c18f67ffeddc7a1b172977"
                        "245f34cfbfd4a2329430af"
                        "9ffed8a8959ec012dcb44d9c934c2b36a9cefcce90931a3190894211");
static const QByteArray referencePubkey2 =
    QByteArray::fromHex("0000000000000000000000006553f1000000000102010000000179be667ef9dcbbac55a062"
                        "95ce870b07029bfcdb2dce"
                        "28d959f2815b16f81798483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d0"
                        "8ffb10d4b8c6047f9441ed"
                        "7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee51ae168fea63dc339a3c584"
                        "19466ceaeef7f632653266"
                        "d0e1236431a950cfe52a");
static const QByteArray referenceBroadcast2 =
    QByteArray::fromHex("0000000000000000000000006553f100000000030401000102030405060708090a0b0c0d0e"
                        "0f02ca00202f8bde4d1a07"
                        "209355b4a7250a5c5128e88b84bddc619ab7cba8d569b240efe40020d8ac222636e5e3d6d4"
                        "dba9dda6c9c426f788271b"
                        "ab0d6840dca87d3aa6ac62d6544beeb1a05e2f506b830044d6ffc27ef333c1db1596077681"
                        "38e666c12b584dc34335fc"
                        "4d6289d2f75a0556df7a56d94fa3814783739f895c9f5e19a437ca574419bf5cebc84b9dce"
                        "ba8b394fa67ef7bd7688c7"
                        "def43c64e2b2d4770c8dcd8ffb1ccf884eed14c954ce27c97ef00402ec3ce003574c89ce22"
                        "2220e68890d4e4a16b027e"
                        "7a78819258bdaea7d78edb8a46cd957eb16b1d81fd98dc2761e26acd5d6aed6ef52b240fa5"
                        "ee9818a1519bc5e969a38a"
                        "52957658ba1a803ad4940d8073398d605728703223958df2114ae3f1e78b6fc15bf8a8fba8"
                        "0ba53f45cb06069b18584e"
                        "0c9b0746da0841971e639d75d3d9d16f9306aa38c44b4103660a7bfd98e43202211e2076c2"
                        "0fe69fb9cc395e8012b751"
                        "674088e371a8efbc152e8fd07d585e6d53d6e6dbf336defb4cdab955");
static const QByteArray referencePubkey3 = QByteArray::fromHex(
    "0000000000000000000000006553f1000000000103010000000179be667ef9dcbbac55a06295ce870b07029bfcdb2d"
    "ce"
    "28d959f2815b16f81798483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8c6047f9441"
    "ed"
    "7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee51ae168fea63dc339a3c58419466ceaeef7f6326532"
    "66"
    "d0e1236431a950cfe52afd03e8fd03e8473045022100f90dc197e3ad4ba9d9f3f2d3bc6e97a2474b9f87ec3a0e95b5"
    "26"
    "57c005d4211f022055db1eceaf2796d78f0c28b3ff40e4e6428faff73c4f46e15687d450f15bfb43");
static const QByteArray referenceBroadcast3 =
    QByteArray::fromHex("0000000000000000000000006553f100000000030401000102030405060708090a0b0c0d0e"
                        "0f02ca00202f8bde4d1a07"
                        "209355b4a7250a5c5128e88b84bddc619ab7cba8d569b240efe40020d8ac222636e5e3d6d4"
                        "dba9dda6c9c426f788271b"
                        "ab0d6840dca87d3aa6ac62d6fcf6dbcda0de099aefeec2e750e500df40f4c13e9e86a9fc73"
                        "71746558a1f1f1ffe25f87"
                        "26e1020472db41da4b46849edbebca6ef97c4ee98168b38a2fca1375eab2d90926797cfb31"
                        "9470b46ca48d34d909d362"
                        "e9d006241ea9d2ea62429343ff3404cc4a49555453bb2b62fb410adae2803fc9d8cfa1d714"
                        "513025a8077c25fe9b4ee2"
                        "df5c4bcddb53e25f2c6e632e0a8597b75086362ee0f1b8c27fc3563c78854dc1255c6e6952"
                        "3bcd9dfc06f8e170f74b5b"
                        "3fec9016c3cf59cbc349730913b79f5ab78b8903ac23d7eba8960e0fabe65dde97bd0fbe41"
                        "bb09d44e899650be75e16d"
                        "ef9191f989b22ee43bd2b9e639d60ae6ffdef481a4e0b69a0fa84d2e06e8d15ea017d86e35"
                        "079e398ebd5a774781e1ff"
                        "8125bac52db038443a8edf20200cda39d96e008a3094357dc43af1f7");
static const QByteArray referencePubkey4 =
    QByteArray::fromHex("0000000000000000000000006553f100000000010401a33fff0f77ee3e8d149fd499ccfd25"
                        "cf7154cbe03cfb466ecd01"
                        "21eaeeb30be6000102030405060708090a0b0c0d0e0f02ca00202f8bde4d1a07209355b4a7"
                        "250a5c5128e88b84bddc61"
                        "9ab7cba8d569b240efe40020d8ac222636e5e3d6d4dba9dda6c9c426f788271bab0d6840dc"
                        "a87d3aa6ac62d6ccfb706e"
                        "625a5595db82c108e0f2011de411590809c7d4caeaf33dbec351f745b2c8777409d167356d"
                        "00527744911f687bf94860"
                        "92d2052a45dfa1b4c3543074cf1361eac71c0f2d1d456f472c669c7af4b57374dbc8296fec"
                        "a3ba4be45339edccdded62"
                        "be5f63e8c3a558f76700e418105fe14018dc3140875105a11cb1e5de0cf40e26f8833162de"
                        "191dba4af962f25e403e80"
                        "1d420fc6a648f9294fd946869c966b420c7da12d897e8e61bb39e60f06b063ed1e86363d78"
                        "656e7ec2faf0f5b351a17f"
                        "60986c8adb4639fc84fe673d0e0d518a25c904570f68d1134a705a4422c6c0ab55c5e2fa75"
                        "b157395716c80d45ad07fe"
                        "0cfd7398cbb0eb0318b88fd3");
static const QByteArray referenceBroadcast4 =
    QByteArray::fromHex("0000000000000000000000006553f100000000030501a33fff0f77ee3e8d149fd499ccfd25"
                        "cf7154cbe03cfb466ecd01"
                        "21eaeeb30be6000102030405060708090a0b0c0d0e0f02ca00202f8bde4d1a07209355b4a7"
                        "250a5c5128e88b84bddc61"
                        "9ab7cba8d569b240efe40020d8ac222636e5e3d6d4dba9dda6c9c426f788271bab0d6840dc"
                        "a87d3aa6ac62d6e518ddb9"
                        "3da43da68485c3c04945ac8bdf3fae186c8c1dc3a2160b989359733de36ae98af22fad3857"
                        "20c3f7298e6932a3e24d29"
                        "1b4310a6166550cf1a6185a501bb357ccb7f2bce94ad770a7dff01d61e1bb204e3ec809089"
                        "568ab0211a260ad2cb7010"
                        "e22029406728f1d6189c239202ddfe329331956f04cbc39b0894a65ec0c2e9b99600fc7e90"
                        "49e991e82d2f7d5e39c605"
                        "d14c9c34444acf31ecb89155e8970661549efbe8ccc51461ebe266e982bbb3eb60f2d130f1"
                        "5101e888b8a1f516ae0f64"
                        "a6d76d8044e1de662f280715fca50f4745a78040d7f2eee08ea35834972b3564cabf4e7099"
                        "2c1208f9e18ea943254886"
                        "d0285eed49892dbb3659d799aff51177d909fd4a2e74dd91772563affa2e9351e9b0559f3b"
                        "5e90aa457c65eee38f79f1"
                        "7f7b3b6bd3f2169b7843408b");
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
        auto oldDecoded = Protocol::decodeMessage(message, recipient);
        require(oldDecoded && oldDecoded->subject == "Hello",
                "legacy notbit decrypt compatibility");
        auto oldEncoded =
            Protocol::encodeMessage(sender, recipient, "Legacy", "old engine", 1700000000);
        require(Wire::decodeMessage(oldEncoded, recipient)->message.subject == "Legacy",
                "legacy notbit encrypt compatibility");
        auto reference = Wire::decodeMessage(referenceMessage, recipient);
        require(reference && reference->message.subject == "Reference" &&
                    reference->message.body == "Independent Python fixture" &&
                    reference->sender.address == sender.address &&
                    Wire::acknowledgmentToken(reference->acknowledgment) == token,
                "independent Python message SHA1 fixture");
        require(!Wire::decodeMessage(badAckMessage, recipient),
                "signed invalid nested ack checksum");
        require(!Wire::decodeMessage(wrongRipeMessage, recipient), "signed wrong destination RIPE");
        require(!Wire::decodeMessage(noncanonicalMessage, recipient),
                "signed noncanonical plaintext varint");
        for (int v = 2; v <= 4; ++v) {
            auto i = fixed(1, 2, v), other = fixed(3, 4, v);
            require(Wire::validAddress(i.address), "valid address");
            auto pub = Wire::encodePubkey(i, 1700000000);
            auto p = Wire::decodePubkey(pub, i.address);
            require(p && p->address == i.address && p->nonceTrials == 1000 && p->extraBytes == 1000,
                    "pubkey roundtrip");
            require(!Wire::decodePubkey(pub, other.address), "pubkey address mismatch");
            auto fixture = v == 2 ? referencePubkey2 : v == 3 ? referencePubkey3 : referencePubkey4;
            require(Wire::decodePubkey(fixture, i.address).has_value(),
                    "independent Python pubkey fixture");
            pub[pub.size() - 1] ^= 1;
            require(!Wire::decodePubkey(pub, i.address), "tampered pubkey");
            auto request = Wire::getPubkey(i.address, 1700000000);
            request[7] = 7;
            require(Wire::requestsIdentity(request, i) && !Wire::requestsIdentity(request, other),
                    "getpubkey matching");
            require(!Wire::requestsIdentity(request + 'x', i), "getpubkey trailing data");
            auto broadcast = Wire::encodeBroadcast(i, "News", "Broadcast body", 1700000000);
            auto b = Wire::decodeBroadcast(broadcast, i.address);
            require(b && b->broadcast && b->message.subject == "News" &&
                        b->sender.address == i.address,
                    "broadcast roundtrip");
            require(!Wire::decodeBroadcast(broadcast, other.address), "wrong subscription");
            auto bf = v == 2   ? referenceBroadcast2
                      : v == 3 ? referenceBroadcast3
                               : referenceBroadcast4;
            require(Wire::decodeBroadcast(bf, i.address).has_value(),
                    "independent Python broadcast fixture");
            broadcast[broadcast.size() - 1] ^= 1;
            require(!Wire::decodeBroadcast(broadcast, i.address), "tampered broadcast");
            for (int n = 0; n < fixture.size(); ++n)
                require(!Wire::decodePubkey(fixture.left(n), i.address), "truncated pubkey");
            for (int n = 0; n < bf.size(); ++n)
                require(!Wire::decodeBroadcast(bf.left(n), i.address), "truncated broadcast");
        }
        for (int n = 0; n < referenceMessage.size(); ++n)
            require(!Wire::decodeMessage(referenceMessage.left(n), recipient), "truncated message");
        for (int n = 8; n < message.size(); n += 7) {
            auto corrupt = message;
            corrupt[n] ^= 1;
            require(!Wire::decodeMessage(corrupt, recipient), "tampered message header or ECIES");
        }
        auto nc = ack.left(20) + QByteArray::fromHex("fd000101") + token;
        require(!Wire::header(nc), "noncanonical header varint");
        auto huge = QByteArray(262145, 0);
        require(!Wire::header(huge) && !Wire::decodeMessage(huge, recipient), "object bound");
        auto badPoint = publicRecipient;
        badPoint.encryptionKey.fill(0);
        rejects([&] { Wire::encodeMessage(sender, badPoint, "", "", 1700000000); });
        auto badAddress = publicRecipient;
        badAddress.address = sender.address;
        rejects([&] { Wire::encodeMessage(sender, badAddress, "", "", 1700000000); });
        rejects(
            [&] { Wire::encodeMessage(sender, publicRecipient, "bad\nsubject", "", 1700000000); });
        rejects([&] {
            Wire::encodeMessage(sender, publicRecipient, "", QString(200001, 'x'), 1700000000);
        });
        rejects([&] { Wire::acknowledgment(QByteArray(31, 0), 1700000000); });
        require(!Wire::validAddress("not an address") &&
                    !Wire::validAddress(sender.address + QChar(0)),
                "invalid addresses");
        std::cout
            << "PASS: public-recipient messages, chan, ack frames, pubkeys v2/v3/v4, broadcasts "
               "v4/v5, "
               "independent Python fixtures, legacy notbit interoperability, malformed inputs\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
