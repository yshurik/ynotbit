#include <bmc/bytes.h>
#include <bmc/clock.h>
#include <bmc/error.h>
#include <bmc/hash.h>
#include <bmc/identity.h>
#include <bmc/random.h>
#include <bmc/secret.h>
#include <bmc/test.h>
#include <chrono>
#include <set>
#include <string>

TEST(harness_checks_values_and_exceptions) {
    CHECK(1 + 1 == 2);
    CHECK_EQ(std::string("a") + "b", "ab");
    CHECK_THROWS(bmc::require(false, "x"));
}

TEST(error_carries_a_code_and_its_text) {
    try {
        bmc::require(false, "Invalid thing", bmc::Code::State);
        CHECK(false);
    } catch (const bmc::Error &e) {
        CHECK(e.code() == bmc::Code::State);
        CHECK_EQ(std::string(e.what()), "Invalid thing");
    }
    // An Error is a std::runtime_error: the Qt layer catches std::exception and reads what().
    try {
        throw bmc::Error(bmc::Code::Io, "x");
    } catch (const std::runtime_error &) {
        return;
    }
    CHECK(false);
}

TEST(clock_follows_the_system_and_can_be_replaced) {
    const auto before = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto now = bmc::now();
    CHECK(now >= before && now <= before + 2);
    bmc::setClockForTesting([] { return int64_t(1700000000); });
    CHECK_EQ(bmc::now(), 1700000000);
    bmc::setClockForTesting(nullptr);
    CHECK(bmc::now() >= before);
}

TEST(hex_round_trips_and_rejects_bad_text) {
    const bmc::Bytes bytes{0x00, 0x01, 0xab, 0xff};
    CHECK_EQ(bmc::hex(bytes), "0001abff");
    CHECK_EQ(bmc::unhex("0001abff"), bytes);
    CHECK_EQ(bmc::unhex("0001ABFF"), bytes);
    CHECK_EQ(bmc::unhex(""), bmc::Bytes{});
    CHECK_THROWS(bmc::unhex("abc"));
    CHECK_THROWS(bmc::unhex("zz"));
}

TEST(big_endian_and_variable_length_integers) {
    bmc::Bytes b;
    bmc::putBE(b, 0x0102030405060708ULL, 8);
    CHECK_EQ(bmc::hex(b), "0102030405060708");
    CHECK_EQ(bmc::getBE(b), 0x0102030405060708ULL);
    b.clear();
    bmc::putBE(b, 0xabcdef, 3);
    CHECK_EQ(bmc::hex(b), "abcdef");
    struct Case {
        uint64_t value;
        const char *hex;
    };
    const Case cases[] = {{0, "00"},
                          {252, "fc"},
                          {253, "fd00fd"},
                          {65535, "fdffff"},
                          {65536, "fe00010000"},
                          {4294967295ULL, "feffffffff"},
                          {4294967296ULL, "ff0000000100000000"}};
    for (const auto &c : cases) {
        bmc::Bytes v;
        bmc::putVarInt(v, c.value);
        CHECK_EQ(bmc::hex(v), c.hex);
    }
}

TEST(byte_helpers) {
    const bmc::Bytes a{1, 2}, b{3};
    CHECK_EQ(bmc::concat({a, b}), (bmc::Bytes{1, 2, 3}));
    bmc::Bytes c = a;
    bmc::append(c, b);
    CHECK_EQ(c, (bmc::Bytes{1, 2, 3}));
    CHECK(bmc::equalConstantTime(a, bmc::Bytes{1, 2}));
    CHECK(!bmc::equalConstantTime(a, b));
    CHECK(!bmc::equalConstantTime(a, bmc::Bytes{1, 3}));
    CHECK(bmc::equalConstantTime(bmc::Bytes{}, bmc::Bytes{}));
    CHECK_EQ(bmc::stringOf(bmc::view("hi")), "hi");
    CHECK_EQ(bmc::bytesOf("hi"), (bmc::Bytes{'h', 'i'}));
    bmc::wipe(c);
    CHECK(c.empty());
}

TEST(digests_match_the_published_vectors) {
    const auto abc = bmc::view("abc");
    CHECK_EQ(bmc::hex(bmc::hash(bmc::Digest::Sha1, abc)),
             "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK_EQ(bmc::hex(bmc::hash(bmc::Digest::Sha256, abc)),
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(
        bmc::hex(bmc::hash(bmc::Digest::Sha512, abc)),
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23"
        "a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    bmc::Hasher parts(bmc::Digest::Sha256);
    parts.add(bmc::view("a"));
    parts.add(bmc::view("bc"));
    CHECK_EQ(bmc::hex(parts.result()),
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(bmc::hex(bmc::hash(bmc::Digest::Sha256, {})),
             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(uuid_is_a_random_version_4_text) {
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        const auto id = bmc::uuid();
        CHECK_EQ(id.size(), size_t(36));
        for (size_t k = 0; k < id.size(); ++k) {
            if (k == 8 || k == 13 || k == 18 || k == 23)
                CHECK(id[k] == '-');
            else
                CHECK((id[k] >= '0' && id[k] <= '9') || (id[k] >= 'a' && id[k] <= 'f'));
        }
        CHECK(id[14] == '4');
        CHECK(id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b');
        seen.insert(id);
    }
    CHECK_EQ(seen.size(), size_t(200));
}

TEST(secret_is_zeroed_movable_and_wiped_on_clear) {
    bmc::Secret key(32);
    CHECK_EQ(key.size(), size_t(32));
    for (size_t i = 0; i < 32; ++i)
        CHECK(key.data()[i] == 0);
    key.data()[5] = 7;
    bmc::Secret moved = std::move(key);
    CHECK_EQ(moved.size(), size_t(32));
    CHECK(moved.data()[5] == 7);
    CHECK_EQ(key.size(), size_t(0)); // a moved-from Secret is empty by contract
    CHECK(key.data() == nullptr);
    moved.clear();
    CHECK_EQ(moved.size(), size_t(0));
    bmc::Secret none;
    CHECK(none.data() == nullptr);
}

TEST(identity_ref_borrows_an_identity) {
    bmc::Identity id;
    id.address = "BM-x";
    id.chan = true;
    id.keys.data()[0] = 9;
    bmc::IdentityRef ref = id;
    CHECK_EQ(std::string(ref.address), "BM-x");
    CHECK(ref.chan);
    CHECK(ref.keys == id.keys.data());
    CHECK_EQ(bmc::letterTextBytes("ab", "cde"), 8 + 2 + 6 + 3);
    CHECK_EQ(bmc::kMaxObjectBytes, 262144);
    CHECK_EQ(bmc::kMaxLetterText, 261000);
}
