// bm_vanity: a fast search for a Bitmessage address with a word in it, and the two keys of every
// address it finds. The method is in vanity.h; this is the program around it: the threads, the
// file of hits, and a few commands for what comes after a search.
//
//   bm_vanity --word WORD [-o hits.txt] [-j THREADS] [--pos-min 2] [--pos-max 5] [--exact]
//             [--min-exact N] [--hits N] [--seconds S] [--no-avx512]
//   bm_vanity --export ADDRESS -f HITS -o KEYS_FILE
//   bm_vanity --address-of KEYS_FILE
//   bm_vanity --selftest
//
// A line of the hits file is: the address; the place of the word (the characters after "BM-"
// before it); how many of its letters are in the case they were typed in; the signing key and the
// encryption key, in hex. The keys ARE the address, so the file is made secret (mode 600) and only
// addresses are ever printed. The searched kind is the one the bots of ynotbit use: version 4,
// stream 1, a RIPE hash of 20 bytes, which makes an address that starts BM-87.
#include "vanity.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <openssl/rand.h>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace vanity;

namespace {
std::atomic<bool> *stopFlag = nullptr;
void onSignal(int) {
    if (stopFlag)
        stopFlag->store(true);
}

std::string hex(const std::uint8_t *p, size_t n) {
    static const char *digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += digits[p[i] >> 4];
        s += digits[p[i] & 15];
    }
    return s;
}
bool unhex32(const std::string &text, std::uint8_t out[32]) {
    if (text.size() != 64)
        return false;
    for (int i = 0; i < 32; ++i) {
        int byte = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = text[2 * i + k];
            const int d = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
            if (d < 0)
                return false;
            byte = byte * 16 + d;
        }
        out[i] = std::uint8_t(byte);
    }
    return true;
}

// A private key: random, and valid (not zero, below the order).
void randomKey(std::uint8_t key[32]) {
    do {
        if (RAND_bytes(key, 32) != 1)
            throw std::runtime_error("no random numbers");
    } while (!ec::curve().valid(key));
}

std::vector<std::string> fieldsOf(const std::string &line) {
    std::vector<std::string> fields;
    std::istringstream in(line);
    std::string field;
    while (in >> field)
        fields.push_back(field);
    return fields;
}

struct Search {
    Pattern pattern;
    std::string output = "hits.txt";
    int minExact = 0;
    bool wide = true; // the AVX-512 hashes, where the processor has them
    std::uint64_t maxHits = 0;
    double maxSeconds = 0;
    std::atomic<std::uint64_t> done{0}, hits{0}, mismatches{0};
    std::atomic<bool> stop{false};
    std::mutex lock;
    int fd = -1;

    // A match is made in full by OpenSSL's code and has to show the word where the fast way said.
    void record(const std::uint8_t sign[32], const std::uint8_t start[32], std::uint64_t delta,
                bool negative, int place) {
        std::uint8_t enc[32];
        ec::curve().add(start, delta, enc);
        if (negative)
            ec::curve().negate(enc, enc);
        std::string address;
        if (!ec::address(sign, enc, address) || pattern.find(address) != place) {
            ++mismatches;
            return;
        }
        const int exact = pattern.exactLetters(address, place);
        if (exact < minExact)
            return;
        const std::string line = address + ' ' + std::to_string(place) + ' ' +
                                 std::to_string(exact) + ' ' + hex(sign, 32) + ' ' + hex(enc, 32) +
                                 '\n';
        std::lock_guard<std::mutex> guard(lock);
        if (::write(fd, line.data(), line.size()) != ssize_t(line.size())) {
            std::fprintf(stderr, "\ncannot write to %s\n", output.c_str());
            stop = true;
            return;
        }
        std::fprintf(stderr, "\nfound %s  (word at char %d, %d of %zu letters as typed)\n",
                     address.c_str(), place, exact, pattern.word().size());
        if (++hits >= maxHits && maxHits)
            stop = true;
    }

    // One thread: keys of its own, windows of them until told to stop.
    void work(const Stepper &stepper) {
        std::uint8_t sign[32], start[32];
        randomKey(sign);
        randomKey(start);
        Walker walker(pattern, stepper, sign, start, wide);
        while (!stop) {
            walker.next([&](std::uint64_t delta, bool negative, int place) {
                record(sign, start, delta, negative, place);
            });
            done += Walker::kKeys;
        }
        OPENSSL_cleanse(sign, sizeof sign);
        OPENSSL_cleanse(start, sizeof start);
    }
};

void usage() {
    std::fprintf(
        stderr,
        "usage: bm_vanity --word WORD [-o hits.txt] [-j THREADS] [--pos-min 2] [--pos-max 5]\n"
        "                 [--exact] [--min-exact N] [--hits N] [--seconds S] [--no-avx512]\n"
        "       bm_vanity --export ADDRESS -f HITS -o KEYS_FILE\n"
        "       bm_vanity --address-of KEYS_FILE\n"
        "       bm_vanity --selftest\n"
        "Looks for addresses (BM-87..., version 4, stream 1) with WORD from --pos-min to "
        "--pos-max\n"
        "characters after BM-, in any case unless --exact; --min-exact N keeps those with at "
        "least\n"
        "N of its letters as typed. A line of the hits file: address, place, letters as typed,\n"
        "signing key, encryption key (hex; the file is secret). --export writes the two keys of "
        "one\n"
        "hit to a new file, which is what bmbot's keys_file takes.\n");
}

std::string slurp(const std::string &path, bool *ok) {
    std::ifstream f(path, std::ios::binary);
    *ok = bool(f);
    std::stringstream raw;
    raw << f.rdbuf();
    return raw.str();
}

// The two keys of a keys file: its first two lines that are not empty or comments, 64 hex digits.
bool readKeys(const std::string &path, std::uint8_t sign[32], std::uint8_t enc[32]) {
    bool ok = false;
    std::istringstream in(slurp(path, &ok));
    if (!ok)
        return false;
    std::vector<std::string> keys;
    std::string line;
    while (std::getline(in, line)) {
        const auto fields = fieldsOf(line);
        if (!fields.empty() && fields[0][0] != '#')
            keys.push_back(fields[0]);
    }
    return keys.size() == 2 && unhex32(keys[0], sign) && unhex32(keys[1], enc);
}

int addressOf(const std::string &path) {
    std::uint8_t sign[32], enc[32];
    std::string address;
    if (!readKeys(path, sign, enc) || !ec::address(sign, enc, address)) {
        std::fprintf(stderr, "%s does not hold two private keys, one to a line, in hex\n",
                     path.c_str());
        return 1;
    }
    std::printf("%s\n", address.c_str());
    return 0;
}

int exportKeys(const std::string &address, const std::string &hits, const std::string &out) {
    bool ok = false;
    std::istringstream in(slurp(hits, &ok));
    if (!ok) {
        std::fprintf(stderr, "cannot read %s\n", hits.c_str());
        return 1;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto f = fieldsOf(line);
        if (f.size() != 5 || f[0] != address)
            continue;
        // Never over a file that is there: the keys of another address may be in it.
        const int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            std::fprintf(stderr, "cannot make %s (is it there already?)\n", out.c_str());
            return 1;
        }
        const std::string text = f[3] + "\n" + f[4] + "\n";
        const bool written = ::write(fd, text.data(), text.size()) == ssize_t(text.size());
        ::close(fd);
        if (!written) {
            std::fprintf(stderr, "cannot write %s\n", out.c_str());
            return 1;
        }
        return 0;
    }
    std::fprintf(stderr, "%s has no hit for that address\n", hits.c_str());
    return 1;
}

// One thread's walk of the easy word against the same keys looked at one by one with OpenSSL.
bool walkAgrees(const Stepper &stepper, bool wide, std::size_t *hits) {
    Pattern easy;
    easy.set("a", 2, 5, false, nullptr);
    std::uint8_t sign[32], start[32];
    randomKey(sign);
    randomKey(start);
    Walker walker(easy, stepper, sign, start, wide);
    if (wide && !walker.wide())
        return true; // this processor has no wide hashes: nothing to compare
    std::map<std::uint64_t, int> found, want; // by delta * 2, and 1 more for the negative's key
    const int windows = 2;
    for (int w = 0; w < windows; ++w)
        walker.next([&](std::uint64_t delta, bool negative, int place) {
            found[delta * 2 + (negative ? 1 : 0)] = place;
        });
    for (std::uint64_t delta = 0; delta < std::uint64_t(windows) * Walker::kWidth; ++delta)
        for (int negative = 0; negative < 2; ++negative) {
            std::uint8_t enc[32];
            std::string address;
            ec::curve().add(start, delta, enc);
            if (negative)
                ec::curve().negate(enc, enc);
            if (!ec::address(sign, enc, address) || address.size() < 38)
                continue; // a RIPE with a zero byte first is an address of another length
            const int place = easy.find(address);
            if (place >= 0)
                want[delta * 2 + negative] = place;
        }
    if (hits)
        *hits = want.size();
    return found == want && !want.empty();
}

// The wide hashes against the scalar ones, on limbs that are not even keys: what a hash is given
// is any 32 bytes.
bool hashesAgree() {
    if (!avx512::supported())
        return true;
    for (int round = 0; round < 300; ++round) {
        std::uint8_t sign[32];
        randomKey(sign);
        std::uint8_t pub[65];
        ec::curve().publicKey(sign, pub);
        avx512::BatchHasher wide(pub);
        PairHasher scalar(pub);
        alignas(64) std::uint64_t xs[4][avx512::kLanes], ys[4][avx512::kLanes];
        alignas(64) std::uint32_t words[4][avx512::kLanes];
        RAND_bytes(reinterpret_cast<unsigned char *>(xs), sizeof xs);
        RAND_bytes(reinterpret_cast<unsigned char *>(ys), sizeof ys);
        if (round % 3 == 1) // limbs with all their bits set or none, where carries and shifts are
            for (int k = 0; k < 4; ++k)
                for (int lane = 0; lane < avx512::kLanes; ++lane) {
                    xs[k][lane] = (lane & 1) ? ~0ULL : 0;
                    ys[k][lane] = (lane & 2) ? ~0ULL : 0;
                }
        wide.hash(xs, ys, words);
        for (int lane = 0; lane < avx512::kLanes; ++lane) {
            std::uint8_t ripe[20];
            scalar.ripe(Fe{{xs[0][lane], xs[1][lane], xs[2][lane], xs[3][lane]}},
                        Fe{{ys[0][lane], ys[1][lane], ys[2][lane], ys[3][lane]}}, ripe);
            for (int w = 0; w < 4; ++w)
                for (int b = 0; b < 4; ++b)
                    if (ripe[4 * w + b] != std::uint8_t(words[w][lane] >> (8 * b)))
                        return false;
        }
    }
    return true;
}

// Addresses a second, in millions, for one thread looking for the real word.
double speed(const Stepper &stepper, bool wide) {
    Pattern real;
    real.set("nickbot", 2, 5, false, nullptr);
    std::uint8_t sign[32], start[32];
    randomKey(sign);
    randomKey(start);
    Walker timed(real, stepper, sign, start, wide);
    std::uint64_t looked = 0;
    const auto began = std::chrono::steady_clock::now();
    double seconds = 0;
    while (seconds < 0.5) {
        timed.next([](std::uint64_t, bool, int) {});
        looked += Walker::kKeys;
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    }
    return double(looked) / seconds / 1e6;
}

// The method against OpenSSL on this machine, built as it is: the easy word's hits in two windows
// are the ones found key by key, with the scalar hashes and with the wide ones, the wide hashes are
// the scalar ones, and one thread's speed at the real thing.
int selftest() {
    const Stepper stepper;
    std::size_t hits = 0;
    if (!walkAgrees(stepper, false, &hits)) {
        std::printf(
            "selftest FAILED: the walk with the scalar hashes finds other hits than OpenSSL\n");
        return 1;
    }
    if (!hashesAgree()) {
        std::printf("selftest FAILED: the AVX-512 hashes differ from the scalar ones\n");
        return 1;
    }
    if (!walkAgrees(stepper, true, nullptr)) {
        std::printf(
            "selftest FAILED: the walk with the AVX-512 hashes finds other hits than OpenSSL\n");
        return 1;
    }
    std::printf("selftest ok: %zu hits of an easy word agree with OpenSSL; one thread looks at "
                "%.2f M addresses/s",
                hits, speed(stepper, false));
    if (avx512::supported())
        std::printf(", with AVX-512 %.2f M/s", speed(stepper, true));
    std::printf("\n");
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    Search s;
    std::string word, exportAddress, hitsFile, keysFile, addressOfFile;
    int posMin = 2, posMax = 5;
    bool exact = false, selftestOnly = false;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    bool haveOutput = false;
    std::string outputArg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        try {
            if (a == "--word")
                word = next();
            else if (a == "-o")
                outputArg = next(), haveOutput = true;
            else if (a == "-f")
                hitsFile = next();
            else if (a == "-j")
                threads = std::max(1, std::stoi(next()));
            else if (a == "--pos-min")
                posMin = std::stoi(next());
            else if (a == "--pos-max")
                posMax = std::stoi(next());
            else if (a == "--exact")
                exact = true;
            else if (a == "--no-avx512")
                s.wide = false;
            else if (a == "--min-exact")
                s.minExact = std::stoi(next());
            else if (a == "--hits")
                s.maxHits = std::stoull(next());
            else if (a == "--seconds")
                s.maxSeconds = std::stod(next());
            else if (a == "--export")
                exportAddress = next();
            else if (a == "--address-of")
                addressOfFile = next();
            else if (a == "--selftest")
                selftestOnly = true;
            else {
                usage();
                return 2;
            }
        } catch (const std::exception &) {
            usage();
            return 2;
        }
    }
    try {
        if (selftestOnly)
            return selftest();
        if (!addressOfFile.empty())
            return addressOf(addressOfFile);
        if (!exportAddress.empty()) {
            if (hitsFile.empty() || !haveOutput) {
                usage();
                return 2;
            }
            return exportKeys(exportAddress, hitsFile, outputArg);
        }
        if (word.empty()) {
            usage();
            return 2;
        }
        if (haveOutput)
            s.output = outputArg;
        std::string problem;
        if (!s.pattern.set(word, posMin, posMax, exact, &problem)) {
            std::fprintf(stderr, "%s\n", problem.c_str());
            return 1;
        }
        s.fd = ::open(s.output.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (s.fd < 0 || ::fchmod(s.fd, 0600) != 0) {
            std::perror(s.output.c_str());
            return 1;
        }
        stopFlag = &s.stop;
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        const Stepper stepper;
        std::fprintf(stderr, "searching on %u threads: '%s' (%s) at char %d..%d after BM-  -> %s\n",
                     threads, word.c_str(), exact ? "exact case" : "any case", posMin, posMax,
                     s.output.c_str());
        std::fprintf(stderr, "hashes: %s\n",
                     s.wide && avx512::supported() && posMax < avx512::kFilterDigits
                         ? "AVX-512"
                         : "scalar (OpenSSL's)");
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back([&] { s.work(stepper); });
        const auto began = std::chrono::steady_clock::now();
        while (!s.stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            static int tick = 0;
            if (++tick % 5 == 0)
                std::fprintf(stderr, "\r%.3fM addresses/s  looked at ~%.0fM  hits %llu   ",
                             s.done / std::max(secs, 1e-3) / 1e6, s.done / 1e6,
                             (unsigned long long)s.hits.load());
            if (s.maxSeconds > 0 && secs >= s.maxSeconds)
                s.stop = true;
        }
        for (auto &t : pool)
            t.join();
        ::close(s.fd);
        if (s.mismatches)
            std::fprintf(stderr, "\nWARNING: %llu matches did not check out and were dropped\n",
                         (unsigned long long)s.mismatches.load());
        std::fprintf(stderr, "\nstopped after looking at %.0fM addresses; %llu hits in %s\n",
                     s.done / 1e6, (unsigned long long)s.hits.load(), s.output.c_str());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "bm_vanity: %s\n", e.what());
        return 1;
    }
}
