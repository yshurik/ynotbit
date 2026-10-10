// Names for addresses: coolname's default generator, ported, checked against
// upstream (third_party/coolname, commit 7f895ee) value for value.
#include "coolname.h"
#include <QCoreApplication>
#include <QFile>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Sample {
    int words;
    quint64 index;
    QStringList name;
};
// Upstream's names at these indices (RandomGenerator._lists[words][index]): the
// ends of every 3-word pattern, and a few between.
static const Sample kUpstream[] = {
    {2, 0ull, {"acrid", "earthworm"}},
    {2, 1ull, {"acrid", "leech"}},
    {2, 370169ull, {"woodoo", "wyvern"}},
    {2, 123456ull, {"stalwart", "camel"}},
    {3, 0ull, {"acrid", "acrid", "earthworm"}},
    {3, 1ull, {"acrid", "acrid", "leech"}},
    {3, 25923578ull, {"tacky", "strict", "dingo"}},
    {3, 38888934ull, {"funny", "polite", "wildebeest"}},
    {3, 80986534ull, {"industrious", "versed", "fulmar"}},
    {3, 173856391ull, {"pompous", "psychedelic", "hamster"}},
    {3, 211969249ull, {"rare", "ultramarine", "hyrax"}},
    {3, 240271862ull, {"new", "woodoo", "wyvern"}},
    {3, 240271863ull, {"acrid", "earthworm", "of", "anger"}},
    {3, 342438782ull, {"woodoo", "wyvern", "of", "teaching"}},
    {3, 342438783ull, {"earthworm", "of", "absolute", "anger"}},
    {3, 349467786ull, {"tench", "of", "imaginary", "purring"}},
    {3, 355974208ull, {"wyvern", "of", "wondrous", "purring"}},
    {3, 355974209ull, {"acrid", "earthworm", "from", "venus"}},
    {3, 365598628ull, {"woodoo", "wyvern", "from", "wonderland"}},
    {3, 365598629ull, {"acrid", "atlantic", "puffin"}},
    {3, 365683492ull, {"new", "zebra", "shark"}},
    {3, 365683493ull, {"atlantic", "puffin", "of", "anger"}},
    {3, 365718820ull, {"zebra", "shark", "of", "teaching"}},
    {3, 365718821ull, {"earthworm", "from", "big", "corporation"}},
    {3, 365729788ull, {"wyvern", "from", "the", "stars"}},
    {3, 365729789ull, {"atlantic", "puffin", "from", "venus"}},
    {3, 365733116ull, {"zebra", "shark", "from", "wonderland"}},
    {4, 0ull, {"acrid", "acrid", "earthworm", "of", "anger"}},
    {4, 83564101797ull, {"new", "zebra", "shark", "from", "wonderland"}},
    {4, 41234567890ull, {"tough", "fierce", "phoenix", "of", "triumph"}},
};
// The same, with ynotbit's exclusions: upstream's filter_config with the same list.
static const Sample kFiltered[] = {
    {2, 0ull, {"acrid", "scorpion"}},
    {2, 1ull, {"acrid", "spider"}},
    {2, 306527ull, {"woodoo", "wyvern"}},
    {2, 123456ull, {"omniscient", "coati"}},
    {3, 0ull, {"acrid", "acrid", "scorpion"}},
    {3, 1ull, {"acrid", "acrid", "spider"}},
    {3, 25923578ull, {"cute", "astonishing", "roadrunner"}},
    {3, 38888934ull, {"adaptable", "eggplant", "panther"}},
    {3, 50535682ull, {"fair", "rare", "flamingo"}},
    {3, 80986534ull, {"conscious", "helpful", "cassowary"}},
    {3, 173856391ull, {"huge", "smart", "sunfish"}},
    {3, 179115351ull, {"new", "woodoo", "wyvern"}},
    {3, 179115352ull, {"acrid", "scorpion", "of", "bliss"}},
    {3, 211969249ull, {"provocative", "seahorse", "of", "bliss"}},
    {3, 259119159ull, {"woodoo", "wyvern", "of", "teaching"}},
    {3, 259119160ull, {"scorpion", "of", "absolute", "bliss"}},
    {3, 269814679ull, {"wyvern", "of", "wondrous", "purring"}},
    {3, 269814680ull, {"acrid", "scorpion", "from", "venus"}},
    {3, 277171351ull, {"woodoo", "wyvern", "from", "wonderland"}},
    {3, 277171352ull, {"acrid", "atlantic", "puffin"}},
    {3, 277235337ull, {"new", "zebra", "shark"}},
    {3, 277235338ull, {"atlantic", "puffin", "of", "bliss"}},
    {3, 277263264ull, {"zebra", "shark", "of", "teaching"}},
    {3, 277263265ull, {"scorpion", "from", "big", "corporation"}},
    {3, 277272740ull, {"wyvern", "from", "the", "stars"}},
    {3, 277272741ull, {"atlantic", "puffin", "from", "venus"}},
    {3, 277275308ull, {"zebra", "shark", "from", "wonderland"}},
    {4, 0ull, {"acrid", "acrid", "scorpion", "of", "bliss"}},
    {4, 59033406073ull, {"new", "zebra", "shark", "from", "wonderland"}},
    {4, 41234567890ull, {"simple", "witty", "raptor", "of", "acceptance"}},
};
struct Address {
    const char *address;
    QStringList name;
};
// The spec's scheme, computed in Python with upstream's generator and our list.
static const Address kAddresses[] = {
    {"BM-2cX8TF9vuQZEWvT7UrEeq1HN9dgiSUPLEN", {"mysterious", "poised", "shellfish"}},
    {"BM-5oSrHVwbsQYPKmePHfqQJZr6eibiqj9", {"imperial", "conscious", "grebe"}},
    {"BM-2cWnMMPofS3LHLT4dNZ6o1r5wbobcJvFh6", {"neat", "glistening", "adder"}},
    {"BM-%1%2", {"noble", "llama", "of", "discourse"}},     // hashed as written, % and all
    {"BM-retry-6", {"sweet", "burrowing", "hippogriff"}},   // its first pick has an excluded word
    {"BM-retry-369", {"wealthy", "sympathetic", "oyster"}}, // its first pick breaks a rule
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        const bm::CoolName upstream;
        require(upstream.combinations(2) == 370170 && upstream.combinations(3) == 365733117 &&
                    upstream.combinations(4) == 83564101798ull,
                "as many names of 2, 3 and 4 words as upstream");
        for (const auto &s : kUpstream)
            require(upstream.name(s.words, s.index) == s.name,
                    "upstream's name at every sampled index");
        require(upstream.name(3, upstream.combinations(3)).isEmpty() &&
                    upstream.combinations(5) == 0 && upstream.name(5, 0).isEmpty(),
                "no name past the last, and none of 5 words");
        require(upstream.acceptable({"kind", "red", "fox"}) &&
                    upstream.acceptable({QString(16, 'a'), QString(16, 'b'), QString(16, 'c')}),
                "names within coolname's rules pass, 50 characters included");
        require(!upstream.acceptable({"acrid", "acrid", "earthworm"}) &&
                    !upstream.acceptable({"super", "superb", "fox"}) &&
                    !upstream.acceptable({QString(17, 'a'), QString(16, 'b'), QString(16, 'c')}),
                "a repeated word, a shared 4-letter start, or 51 characters breaks the rules");
        const auto excluded = bm::excludedWords();
        require(excluded.size() == 136 && excluded.contains("manipulative") &&
                    excluded.contains("feral") && !excluded.contains("#"),
                "the exclusion list's 136 words, comments aside");
        const bm::CoolName names(excluded);
        require(names.combinations(2) == 306528 && names.combinations(3) == 277275309 &&
                    names.combinations(4) == 59033406074ull,
                "as many names as upstream's filter_config leaves");
        for (const auto &s : kFiltered)
            require(names.name(s.words, s.index) == s.name,
                    "upstream's filtered name at every sampled index");
        {
            // First use from several threads at once: one vocabulary, the same names.
            std::atomic<int> wrong = 0;
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t)
                threads.emplace_back([&] {
                    for (const auto &a : kAddresses)
                        if (bm::addressName(a.address) != a.name)
                            ++wrong;
                });
            for (auto &thread : threads)
                thread.join();
            require(wrong == 0, "every address has its fixed name, from any thread");
        }
        const QString digest = "BM-2cX8TF9vuQZEWvT7UrEeq1HN9dgiSUPLEN";
        require(bm::addressName("  " + digest + "\n") == bm::addressName(digest) &&
                    bm::addressName("bm-2cX8TF9vuQZEWvT7UrEeq1HN9dgiSUPLEN") ==
                        bm::addressName(digest) &&
                    bm::addressName("2cX8TF9vuQZEWvT7UrEeq1HN9dgiSUPLEN") ==
                        bm::addressName(digest),
                "spaces and the BM- prefix, in any case or none, do not change the name");
        require(bm::addressName("BM-2cx8TF9vuQZEWvT7UrEeq1HN9dgiSUPLEN") != bm::addressName(digest),
                "the rest of an address is case-sensitive, as base58 is");
        require(bm::addressName("").isEmpty() && bm::addressName("  BM-  ").isEmpty(),
                "an empty address has no name");
        int withOf = 0, withFrom = 0;
        for (int i = 0; i < 10000; ++i) {
            const auto name = bm::addressName(QString("BM-sample-%1").arg(i));
            require(names.acceptable(name), "every address's name keeps coolname's rules");
            for (const auto &word : name)
                require(!excluded.contains(word), "no address's name has an excluded word");
            withOf += name.contains("of");
            withFrom += name.contains("from");
        }
        require(withOf == 3249 && withFrom == 262,
                "10,000 addresses fall into the patterns exactly as in the reference");
        {
            // Words excluded later rename only the names that have them.
            const QSet<QString> none,
                some{"heavy", "cunning", "loose", "spicy", "simple", "exotic"};
            const auto hasSome = [&](const QStringList &name) {
                return std::any_of(name.begin(), name.end(),
                                   [&](const QString &word) { return some.contains(word); });
            };
            int renamed = 0;
            for (int i = 0; i < 2000; ++i) {
                const auto address = QString("BM-sample-%1").arg(i);
                const auto before = bm::addressName(address, none);
                const auto after = bm::addressName(address, some);
                require(!hasSome(after), "an excluded word leaves every name");
                require(hasSome(before) || after == before,
                        "excluding words renames only the names that have them");
                renamed += hasSome(before);
            }
            require(renamed > 0, "test sanity: some names had those words");
        }
        if (argc > 1) { // ctest passes the source tree
            // Builds carry coolname's notice with the others (BSD-2-Clause).
            const auto read = [root = QString::fromLocal8Bit(argv[1])](const QString &path) {
                QFile file(root + "/" + path);
                return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
            };
            require(!read("third_party/coolname/LICENSE").isEmpty() &&
                        read("licenses/coolname.txt") == read("third_party/coolname/LICENSE"),
                    "licenses/, which packaged builds copy, has coolname's license");
            require(
                read("THIRD_PARTY.md").contains("https://github.com/alexanderlukanin13/coolname") &&
                    read("THIRD_PARTY.md").contains("7f895eed330e39830d7042ee03395a332495480c"),
                "THIRD_PARTY.md names coolname at its pinned commit");
        }
        std::cout << "PASS: coolname's names as upstream, our exclusions, a name per address\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
