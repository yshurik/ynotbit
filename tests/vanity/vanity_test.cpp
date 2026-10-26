// The parts of the fast vanity search (tools/vanity/vanity.h) against OpenSSL, against the core's
// own addresses, and against slow ways of finding the same hits; and the program itself, run on
// a word that is easy to find. The numbers come from a generator with a fixed seed, so a failure
// happens again.
#include "vanity.h"
#include <array>
#include <bmc/phrase.h>
#include <bmc/test.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <openssl/bn.h>
#include <sys/stat.h>

using namespace vanity;

namespace {
using Key = std::array<uint8_t, 32>;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() { // xorshift64*
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    void bytes(uint8_t *out, size_t n) {
        for (size_t i = 0; i < n; i += 8) {
            const uint64_t v = next();
            for (size_t j = 0; j < 8 && i + j < n; ++j)
                out[i + j] = uint8_t(v >> (8 * j));
        }
    }
    // 32 bytes, any of them: a field element or a hash.
    Key any() {
        Key k;
        bytes(k.data(), 32);
        return k;
    }
    // A private key: the top bit cleared keeps it below the order, and it is not zero.
    Key key() {
        Key k = any();
        k[0] &= 0x7F;
        k[31] |= 1;
        return k;
    }
};

// What OpenSSL's big numbers say about the field, for the same inputs.
class Reference {
  public:
    Reference() : ctx_(BN_CTX_new()) {
        BN_hex2bn(&p_, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F");
    }
    ~Reference() {
        BN_free(p_);
        BN_CTX_free(ctx_);
    }
    // The number the bytes make, below p.
    BIGNUM *number(const Key &k) const {
        BIGNUM *n = BN_bin2bn(k.data(), 32, nullptr);
        BN_mod(n, n, p_, ctx_);
        return n;
    }
    Key bytesOf(const BIGNUM *n) const {
        Key k;
        BN_bn2binpad(n, k.data(), 32);
        return k;
    }
    template <class Op> Key apply(const Key &a, const Key &b, Op op) const {
        BIGNUM *x = number(a), *y = number(b), *r = BN_new();
        op(r, x, y);
        const auto out = bytesOf(r);
        BN_free(x);
        BN_free(y);
        BN_free(r);
        return out;
    }
    Key add(const Key &a, const Key &b) const {
        return apply(a, b, [&](BIGNUM *r, BIGNUM *x, BIGNUM *y) { BN_mod_add(r, x, y, p_, ctx_); });
    }
    Key sub(const Key &a, const Key &b) const {
        return apply(a, b, [&](BIGNUM *r, BIGNUM *x, BIGNUM *y) { BN_mod_sub(r, x, y, p_, ctx_); });
    }
    Key mul(const Key &a, const Key &b) const {
        return apply(a, b, [&](BIGNUM *r, BIGNUM *x, BIGNUM *y) { BN_mod_mul(r, x, y, p_, ctx_); });
    }
    // A 64-byte big-endian number modulo p.
    Key reduce(const uint8_t (&big)[64]) const {
        BIGNUM *n = BN_bin2bn(big, 64, nullptr);
        BN_mod(n, n, p_, ctx_);
        const auto out = bytesOf(n);
        BN_free(n);
        return out;
    }
    Key inverse(const Key &a) const {
        BIGNUM *x = number(a), *r = BN_new();
        BN_mod_inverse(r, x, p_, ctx_);
        const auto out = bytesOf(r);
        BN_free(x);
        BN_free(r);
        return out;
    }

  private:
    BN_CTX *ctx_;
    BIGNUM *p_ = nullptr;
};

Key bytesOf(const Fe &f) {
    Key k;
    feToBytes(f, k.data());
    return k;
}
Fe feOf(const Key &k) {
    return feFromBytes(k.data());
}
Key hexKey(const char *hex) {
    const auto raw = bmc::unhex(hex);
    CHECK_EQ(raw.size(), size_t(32));
    Key k;
    std::copy(raw.begin(), raw.end(), k.begin());
    return k;
}

// The numbers a field has to get right: the ends of its range, and what is next to them.
std::vector<Key> edgeValues() {
    return {hexKey("0000000000000000000000000000000000000000000000000000000000000000"),
            hexKey("0000000000000000000000000000000000000000000000000000000000000001"),
            hexKey("0000000000000000000000000000000000000000000000000000000000000002"),
            hexKey("00000000000000000000000000000000000000000000000000000001000003d1"),
            hexKey("0000000000000000ffffffffffffffff0000000000000000ffffffffffffffff"),
            hexKey("fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2e"), // p - 1
            hexKey("fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2d"), // p - 2
            hexKey("fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f"), // p
            hexKey("fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc30"), // p + 1
            hexKey("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")};
}

std::string hexOf(const Key &k) {
    return bmc::hex(bmc::ByteView(k.data(), 32));
}

std::array<uint8_t, 65> publicOf(const Key &priv) {
    std::array<uint8_t, 65> out;
    CHECK(ec::curve().publicKey(priv.data(), out.data()));
    return out;
}
Affine pointOf(const Key &priv) {
    const auto pub = publicOf(priv);
    return {feFromBytes(pub.data() + 1), feFromBytes(pub.data() + 33)};
}
Key plus(const Key &base, uint64_t delta) {
    Key out;
    ec::curve().add(base.data(), delta, out.data());
    return out;
}

// The place of the word in the characters after "BM-", found the slow and plain way: the first
// place from posMin to posMax where each letter is the typed one or, unless exact, its other case.
int naiveFind(const std::string &address, const std::string &word, int posMin, int posMax,
              bool exact) {
    for (int at = posMin; at <= posMax; ++at) {
        const size_t from = 3 + size_t(at);
        if (from + word.size() > address.size())
            break;
        bool same = true;
        for (size_t i = 0; i < word.size() && same; ++i) {
            const char a = address[from + i], b = word[i];
            same =
                exact ? a == b : std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
        }
        if (same)
            return at;
    }
    return -1;
}

Pattern patternFor(const std::string &word, int posMin, int posMax, bool exact) {
    Pattern p;
    std::string problem;
    CHECK(p.set(word, posMin, posMax, exact, &problem));
    CHECK(problem.empty());
    return p;
}
} // namespace

TEST(the_field_agrees_with_openssl) {
    const Reference ref;
    Rng rng(1);
    auto values = edgeValues();
    for (int i = 0; i < 200; ++i)
        values.push_back(rng.any());
    // Every pair of the edge values, then random pairs.
    std::vector<std::pair<Key, Key>> pairs;
    for (size_t i = 0; i < 10; ++i)
        for (size_t j = 0; j < 10; ++j)
            pairs.push_back({values[i], values[j]});
    for (int i = 0; i < 20000; ++i)
        pairs.push_back({rng.any(), rng.any()});
    for (const auto &[a, b] : pairs) {
        const Fe fa = feOf(a), fb = feOf(b);
        CHECK_EQ(hexOf(bytesOf(feAdd(fa, fb))), hexOf(ref.add(a, b)));
        CHECK_EQ(hexOf(bytesOf(feSub(fa, fb))), hexOf(ref.sub(a, b)));
        CHECK_EQ(hexOf(bytesOf(feMul(fa, fb))), hexOf(ref.mul(a, b)));
        CHECK_EQ(hexOf(bytesOf(feSqr(fa))), hexOf(ref.mul(a, a)));
    }
    // The bytes of a value at or above p come back reduced.
    for (const auto &v : edgeValues()) {
        BIGNUM *n = ref.number(v);
        CHECK_EQ(hexOf(bytesOf(feOf(v))), hexOf(ref.bytesOf(n)));
        BN_free(n);
    }
    for (size_t i = 1; i < values.size(); ++i) { // the inverse of zero is not asked for
        const Key a = values[i];
        if (hexOf(bytesOf(feOf(a))) == hexOf(values[0]))
            continue; // p itself is zero
        CHECK_EQ(hexOf(bytesOf(feInv(feOf(a)))), hexOf(ref.inverse(a)));
    }
}

TEST(a_number_of_any_size_below_2_to_the_512_is_folded_right) {
    // A product of two numbers below p never carries past 256 bits in the second fold; these do.
    const Reference ref;
    Rng rng(7);
    const auto check = [&](const u64 t[8]) {
        uint8_t big[64];
        for (int i = 0; i < 8; ++i)
            storeBE(big + 8 * (7 - i), t[i]);
        CHECK_EQ(hexOf(bytesOf(feReduce(t))), hexOf(ref.reduce(big)));
    };
    const u64 ones = ~0ULL;
    const u64 cases[][8] = {{ones, ones, ones, ones, ones, ones, ones, ones},
                            {ones, ones, ones, ones, 0, 0, 0, 0},
                            {0, 0, 0, 0, ones, ones, ones, ones},
                            {ones, ones, ones, ones, 1, 0, 0, 0},
                            {ones, ones, ones, ones, ones, 0, 0, 0},
                            {ones, ones, ones, ones, 0, 0, 0, ones},
                            {0, 0, 0, 0, 0, 0, 0, 0},
                            {kPrimeLow, ones, ones, ones, 0, 0, 0, 0},
                            {kPrimeLow, ones, ones, ones, kPrimeLow, ones, ones, ones}};
    for (const auto &t : cases)
        check(t);
    for (int i = 0; i < 20000; ++i) {
        u64 t[8];
        for (auto &limb : t)
            limb = rng.next();
        // Now and then a limb at the end of its range, where carries are.
        for (auto &limb : t)
            if (rng.next() % 4 == 0)
                limb = (rng.next() & 1) ? ones : 0;
        check(t);
    }
}

TEST(the_stepper_visits_every_key_around_its_centre) {
    const Stepper stepper;
    Rng rng(2);
    for (int round = 0; round < 3; ++round) {
        const Key start = rng.key();
        const Key centre = plus(start, Stepper::kHalf);
        std::map<int, Key> seenX, seenY;
        const bool ok = stepper.window(pointOf(centre), [&](int j, const Fe &x, const Fe &y) {
            CHECK(!seenX.count(j));
            seenX[j] = bytesOf(x);
            seenY[j] = bytesOf(y);
        });
        CHECK(ok);
        CHECK_EQ(int(seenX.size()), Stepper::kWidth);
        for (int j = -Stepper::kHalf; j <= Stepper::kHalf; ++j) {
            const auto pub = publicOf(plus(start, uint64_t(Stepper::kHalf + j)));
            CHECK_EQ(hexOf(seenX[j]), bmc::hex(bmc::ByteView(pub.data() + 1, 32)));
            CHECK_EQ(hexOf(seenY[j]), bmc::hex(bmc::ByteView(pub.data() + 33, 32)));
        }
        // The next centre is one window on.
        const Affine next = stepper.advance(pointOf(centre));
        const auto pub = publicOf(plus(centre, uint64_t(Stepper::kWidth)));
        CHECK_EQ(hexOf(bytesOf(next.x)), bmc::hex(bmc::ByteView(pub.data() + 1, 32)));
        CHECK_EQ(hexOf(bytesOf(next.y)), bmc::hex(bmc::ByteView(pub.data() + 33, 32)));
    }
}

TEST(only_numbers_from_1_to_the_order_less_1_are_private_keys) {
    Key order, zero{}, one{}, below, above;
    ec::curve().order(order.data());
    one[31] = 1;
    below = order;
    below[31] -= 1; // the order ends in ...D0364141
    above = order;
    above[31] += 1;
    CHECK(!ec::curve().valid(zero.data()));
    CHECK(ec::curve().valid(one.data()));
    CHECK(ec::curve().valid(below.data()));
    CHECK(!ec::curve().valid(order.data()));
    CHECK(!ec::curve().valid(above.data()));
    uint8_t pub[65];
    CHECK(!ec::curve().publicKey(order.data(), pub));
    // Sums go round at the order: order - 2, plus 5, is 3.
    Key near = order, sum;
    near[31] -= 2;
    ec::curve().add(near.data(), 5, sum.data());
    Key three{};
    three[31] = 3;
    CHECK(sum == three);
}

TEST(a_window_that_would_add_a_point_to_its_own_negative_is_refused) {
    // A centre of -5G meets 5G: the sum is the point at infinity and has no x and y to give. It
    // cannot happen to a random start, but what it would do must not be garbage.
    const Stepper stepper;
    Key centre;
    ec::curve().order(centre.data());
    centre[31] -= 5; // the order ends in ...D0364141: no borrow
    int calls = 0;
    CHECK(!stepper.window(pointOf(centre), [&](int, const Fe &, const Fe &) { ++calls; }));
    CHECK_EQ(calls, 0);
}

TEST(a_word_with_no_spelling_in_an_address_is_refused) {
    const auto reason = [](const std::string &word, int posMin, int posMax, bool exact) {
        Pattern p;
        std::string problem;
        CHECK(!p.set(word, posMin, posMax, exact, &problem));
        CHECK(!problem.empty());
        return problem;
    };
    // Base58 has no 0, no O, no I and no l: a letter is lost when only that case is asked for.
    CHECK(reason("nickbol", 2, 5, true).find("'l'") != std::string::npos);
    CHECK(reason("zero0", 2, 5, false).find("'0'") != std::string::npos); // no case of it
    CHECK(reason("O", 2, 5, true).find("'O'") != std::string::npos);
    CHECK(reason("Ni-k", 2, 5, false).find("'-'") != std::string::npos);
    CHECK(reason("", 2, 5, false).find("word") != std::string::npos);
    CHECK(reason("abc", 5, 2, false).find("pos") != std::string::npos);
    CHECK(reason("abc", -1, 2, false).find("pos") != std::string::npos);
    CHECK(reason(std::string(20, 'a'), 2, 5, false).find("long") != std::string::npos);
    // The letter must be typed as it is in an address when exact; otherwise either case will do.
    Pattern p;
    std::string problem;
    CHECK(p.set("Ink", 2, 5, false, &problem)); // I is not a digit, i is
    CHECK(!p.set("Ink", 2, 5, true, &problem));
    CHECK(p.set("nickbol", 2, 5, false, &problem)); // l is not a digit, L is
    CHECK(p.set("nickbot", 2, 5, true, &problem));
}

// A RIPE whose address starts with the given characters after "BM-": the number the address is,
// with those digits first and the rest random, cut back to the RIPE it holds. The checksum it
// really has changes nothing in the first characters. False when the digits are outside the kind
// of address searched.
bool ripeWithPrefix(const std::string &chars, Rng &rng, uint8_t ripe[20]) {
    std::string digits = chars;
    while (digits.size() < 35)
        digits += ec::kAlphabet[rng.next() % 58];
    uint8_t number[26] = {0};
    for (const char c : digits) {
        int carry = int(std::strchr(ec::kAlphabet, c) - ec::kAlphabet);
        for (int i = 25; i >= 0; --i) {
            carry += number[i] * 58;
            number[i] = uint8_t(carry & 0xFF);
            carry >>= 8;
        }
        if (carry)
            return false;
    }
    if (number[0] != 4 || number[1] != 1 || number[2] == 0)
        return false;
    std::memcpy(ripe, number + 2, 20);
    return true;
}

TEST(the_fast_match_agrees_with_the_address_it_stands_for) {
    Rng rng(3);
    struct Case {
        const char *word;
        int posMin, posMax;
        bool exact;
    };
    const Case cases[] = {
        {"a", 2, 5, false},          {"ab", 2, 5, false},     {"a", 0, 3, false},
        {"nick", 2, 5, false},       {"Zq", 2, 4, true},      {"z", 2, 5, true},
        {"nickbot", 2, 5, false},    {"k", 3, 3, false},      {"nickbot", 2, 2, true},
        {"abcdefghij", 2, 5, false}, {"nickbot", 5, 9, false}};
    for (const auto &c : cases) {
        const std::string word = c.word;
        const auto pattern = patternFor(word, c.posMin, c.posMax, c.exact);
        const auto check = [&](const uint8_t ripe[20]) {
            const auto address = addressOfRipe(ripe);
            CHECK_EQ(int(address.size()), 38); // BM-, and the 35 digits of this kind of address
            const int want = naiveFind(address, word, c.posMin, c.posMax, c.exact);
            CHECK_EQ(pattern.match(ripe), want);
            CHECK_EQ(pattern.find(address), want); // the check a hit goes through is the same
            return want >= 0;
        };
        // Any RIPE: most have no word in them, and the ones that do are the easy words'.
        int chance = 0;
        for (int i = 0; i < 40000; ++i) {
            uint8_t ripe[20];
            rng.bytes(ripe, 20);
            if (ripe[0] == 0)
                ripe[0] = 1;
            chance += check(ripe);
        }
        if (word.size() <= 2)
            CHECK(chance > 20);
        // And RIPEs made to have the word, at each place, in any case unless exact: all of the
        // ones the address holds are found.
        int made = 0, found = 0;
        for (int place = c.posMin; place <= c.posMax; ++place) {
            for (int i = 0; i < 600; ++i) {
                std::string chars = "87";
                for (int k = 2; k < place; ++k)
                    chars += ec::kAlphabet[rng.next() % 58];
                for (const char letter : word) {
                    const char lower = char(std::tolower((unsigned char)letter));
                    const char upper = char(std::toupper((unsigned char)letter));
                    char pick = c.exact ? letter : ((rng.next() & 1) ? lower : upper);
                    if (!std::strchr(ec::kAlphabet, pick))
                        pick = pick == lower ? upper : lower;
                    chars += pick;
                }
                uint8_t ripe[20];
                if (!ripeWithPrefix(chars, rng, ripe))
                    continue;
                ++made;
                found += check(ripe);
            }
        }
        CHECK(made > 100);
        CHECK_EQ(found, made);
    }
}

TEST(how_many_letters_are_in_the_case_they_were_typed_in) {
    const auto pattern = patternFor("nickbot", 2, 5, false);
    // i and o have no capitals in base 58, so no address has them any other way.
    CHECK_EQ(pattern.exactLetters("BM-87nickbotXYZ", 2), 7);
    CHECK_EQ(pattern.exactLetters("BM-87niCkBoTXYZ", 2), 4); // n, i, k, o
    CHECK_EQ(pattern.exactLetters("BM-87XnickbotYZ", 3), 7);
    CHECK_EQ(pattern.exactLetters("BM-87XNiCkBoTYZ", 3), 3); // i, k, o
    CHECK_EQ(pattern.exactLetters("BM-87XnickbotYZ", -1), 0);
}

TEST(the_hasher_makes_the_ripe_of_a_key_pair) {
    Rng rng(4);
    for (int i = 0; i < 300; ++i) {
        const Key sign = rng.key(), enc = rng.key();
        const auto signPub = publicOf(sign), encPub = publicOf(enc);
        PairHasher hasher(signPub.data());
        uint8_t ripe[20];
        hasher.ripe(feFromBytes(encPub.data() + 1), feFromBytes(encPub.data() + 33), ripe);
        uint8_t joined[130], sha[64], want[20];
        std::copy(signPub.begin(), signPub.end(), joined);
        std::copy(encPub.begin(), encPub.end(), joined + 65);
        SHA512(joined, 130, sha);
        RIPEMD160(sha, 64, want);
        CHECK_EQ(bmc::hex(bmc::ByteView(ripe, 20)), bmc::hex(bmc::ByteView(want, 20)));
        // The same hasher goes on to the next key.
        const Key enc2 = rng.key();
        const auto encPub2 = publicOf(enc2);
        hasher.ripe(feFromBytes(encPub2.data() + 1), feFromBytes(encPub2.data() + 33), ripe);
        std::copy(encPub2.begin(), encPub2.end(), joined + 65);
        SHA512(joined, 130, sha);
        RIPEMD160(sha, 64, want);
        CHECK_EQ(bmc::hex(bmc::ByteView(ripe, 20)), bmc::hex(bmc::ByteView(want, 20)));
    }
}

TEST(an_address_made_here_is_the_one_the_core_makes) {
    Rng rng(5);
    int stripped = 0;
    for (int i = 0; i < 3000 && stripped < 2; ++i) {
        const Key sign = rng.key(), enc = rng.key();
        std::string address;
        CHECK(ec::address(sign.data(), enc.data(), address));
        CHECK_EQ(address, bmc::phrase::fromKeys(sign.data(), enc.data()).address);
        // One in 256 has a zero byte first, which the address leaves out: the core's shorter ones.
        stripped += address.size() < 38;
    }
    CHECK(stripped >= 1);
}

TEST(a_walker_reports_the_keys_that_have_the_word_and_no_others) {
    const Stepper stepper;
    Rng rng(6);
    const Key sign = rng.key(), start = rng.key();
    const std::string word = "a";
    const auto pattern = patternFor(word, 2, 5, false);
    const auto signPub = publicOf(sign);

    Walker walker(pattern, stepper, sign.data(), start.data());
    std::map<uint64_t, int> found;
    const int windows = 6;
    for (int w = 0; w < windows; ++w)
        walker.next([&](uint64_t delta, int place) {
            CHECK(!found.count(delta));
            found[delta] = place;
        });

    // The same keys, one by one, through OpenSSL and the core.
    std::map<uint64_t, int> want;
    int zeroFirst = 0;
    for (uint64_t delta = 0; delta < uint64_t(windows) * Stepper::kWidth; ++delta) {
        const Key enc = plus(start, delta);
        std::string address;
        CHECK(ec::address(sign.data(), enc.data(), address));
        if (address.size() < 38) { // a RIPE with a zero byte first: another kind of address
            ++zeroFirst;
            continue;
        }
        const int place = naiveFind(address, word, 2, 5, false);
        if (place >= 0) {
            want[delta] = place;
            CHECK_EQ(address, bmc::phrase::fromKeys(sign.data(), enc.data()).address);
        }
    }
    CHECK(want.size() > 100);
    CHECK(zeroFirst > 0); // the walker was held to leaving those out
    CHECK(found == want);
}

// --- the program --------------------------------------------------------------------------------

namespace {
// Runs a command and returns what it printed.
std::string output(const std::string &command, int *status = nullptr) {
    std::string out;
    FILE *p = popen((command + " 2>/dev/null").c_str(), "r");
    CHECK(p != nullptr);
    char buffer[512];
    while (size_t n = fread(buffer, 1, sizeof buffer, p))
        out.append(buffer, n);
    const int code = pclose(p);
    if (status)
        *status = code;
    return out;
}
std::vector<std::string> fieldsOf(const std::string &line) {
    std::vector<std::string> fields;
    size_t at = 0;
    while (at < line.size()) {
        const auto end = line.find(' ', at);
        fields.push_back(line.substr(at, end == std::string::npos ? end : end - at));
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return fields;
}
int modeOf(const std::string &path) {
    struct stat st;
    CHECK(stat(path.c_str(), &st) == 0);
    return st.st_mode & 0777;
}
} // namespace

TEST(the_program_finds_hits_that_are_the_addresses_of_their_keys) {
    const std::string bin = BM_VANITY_BIN;
    const auto dir = bmc::test::tempDir();
    const auto hits = dir + "/hits.txt";
    int status = 0;
    output(bin + " -j 2 --word a --pos-min 2 --pos-max 3 --hits 6 --seconds 20 -o " + hits,
           &status);
    CHECK_EQ(status, 0);
    CHECK_EQ(modeOf(hits), 0600); // the keys of a hit are secrets
    const auto text = bmc::stringOf(bmc::test::readFile(hits));
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();) {
        const auto end = text.find('\n', at);
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    CHECK(lines.size() >= 6);
    for (const auto &line : lines) {
        // address place letters-in-case signing-key encryption-key
        const auto f = fieldsOf(line);
        CHECK_EQ(int(f.size()), 5);
        const auto sign = bmc::unhex(f[3]), enc = bmc::unhex(f[4]);
        CHECK_EQ(bmc::phrase::fromKeys(sign.data(), enc.data()).address, f[0]);
        const int place = std::stoi(f[1]);
        CHECK(place == 2 || place == 3);
        CHECK_EQ(naiveFind(f[0], "a", 2, 3, false), place);
        CHECK_EQ(std::stoi(f[2]), f[0][3 + place] == 'a' ? 1 : 0);
    }

    // The keys of one hit go to a file of their own, which libbmbot can open, and the file says the
    // same address.
    const auto first = fieldsOf(lines[0]);
    const auto keys = dir + "/keys.txt";
    output(bin + " --export " + first[0] + " -f " + hits + " -o " + keys, &status);
    CHECK_EQ(status, 0);
    CHECK_EQ(modeOf(keys), 0600);
    CHECK_EQ(bmc::stringOf(bmc::test::readFile(keys)), first[3] + "\n" + first[4] + "\n");
    CHECK_EQ(output(bin + " --address-of " + keys), first[0] + "\n");
    // A keys file may have comments and empty lines, as bmbot's reads them.
    const auto commented = dir + "/commented.txt";
    const std::string commentedText = "# the keys\n\n" + first[3] + "\n  " + first[4] + "  \n";
    bmc::test::writeFile(commented, bmc::view(commentedText));
    CHECK_EQ(output(bin + " --address-of " + commented), first[0] + "\n");
    const auto oneKey = dir + "/one.txt";
    const std::string oneKeyText = first[3] + "\n";
    bmc::test::writeFile(oneKey, bmc::view(oneKeyText));
    output(bin + " --address-of " + oneKey, &status);
    CHECK(status != 0);
    const auto threeKeys = dir + "/three.txt";
    const std::string threeKeysText = first[3] + "\n" + first[4] + "\n" + first[4] + "\n";
    bmc::test::writeFile(threeKeys, bmc::view(threeKeysText));
    output(bin + " --address-of " + threeKeys, &status);
    CHECK(status != 0);
    // It does not write over a file that is there, nor export an address it has not found.
    output(bin + " --export " + first[0] + " -f " + hits + " -o " + keys, &status);
    CHECK(status != 0);
    output(bin + " --export BM-nothing -f " + hits + " -o " + dir + "/other.txt", &status);
    CHECK(status != 0);
}

TEST(the_program_keeps_only_the_hits_with_enough_letters_in_the_case_typed) {
    const std::string bin = BM_VANITY_BIN;
    const auto hits = bmc::test::tempDir() + "/hits.txt";
    int status = 0;
    output(bin + " -j 2 --word ab --pos-min 2 --pos-max 3 --min-exact 2 --hits 3 --seconds 20 -o " +
               hits,
           &status);
    CHECK_EQ(status, 0);
    const auto text = bmc::stringOf(bmc::test::readFile(hits));
    int lines = 0;
    for (size_t at = 0; at < text.size();) {
        const auto end = text.find('\n', at);
        const auto f = fieldsOf(text.substr(at, end - at));
        CHECK_EQ(int(f.size()), 5);
        CHECK_EQ(std::stoi(f[2]), 2); // both letters as typed: ab, not aB, Ab or AB
        CHECK_EQ(f[0].substr(3 + std::stoi(f[1]), 2), std::string("ab"));
        ++lines;
        at = end + 1;
    }
    CHECK(lines >= 3);
}

TEST(the_program_checks_itself) {
    int status = 0;
    const auto text = output(std::string(BM_VANITY_BIN) + " --selftest", &status);
    CHECK_EQ(status, 0);
    CHECK(text.find("selftest ok") != std::string::npos);
}
