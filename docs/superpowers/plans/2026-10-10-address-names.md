# Names for Addresses Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every Bitmessage address a stable three-word name, using a C++ port of coolname's default generator.

**Architecture:** coolname's vocabulary is vendored unchanged in `third_party/coolname/` and embedded as a Qt resource. `bm::CoolName` (src/coolname.{h,cpp}) rebuilds coolname's list tree from it and indexes it exactly as upstream does, so its names can be checked against upstream index by index. `bm::addressName()` hashes an address to an index into the 3-word names, with ynotbit's exclusions applied.

**Tech Stack:** C++17, Qt 6 Core (QJsonDocument, QCryptographicHash, QRegularExpression, resources via `qt_add_resources`), CMake, the repo's plain `require()` test style run by ctest.

**Spec:** `docs/superpowers/specs/2026-10-10-address-names-design.md`

## Global Constraints

- Vocabulary: upstream coolname commit `7f895ee` (2026-04-23), `data/config.json` and `data/*.txt` copied unchanged; never updated, because any change renames every address.
- Names have three words; coolname's rules apply: no word twice, no two words starting with the same 4 letters, slug at most 50 characters.
- Address scheme: trim, drop a leading `BM-` in any case, empty means no name; index = first 8 bytes (big-endian) of SHA-256 of `"ynotbit-name:" + address + ":" + k` modulo `combinations(3)`, for k = 0, 1, 2, … until the name keeps the rules.
- `third_party/coolname/LICENSE` (BSD-2-Clause, Alexander Lukanin) is kept, and its README names the upstream commit.
- Formatting: `.clang-format` (LLVM, IndentWidth 4, ColumnLimit 100); format only lines you changed: `/opt/local/bin/clang-format -i --lines=A:B file`.
- Build: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8`. Full suite: `ctest --test-dir build --output-on-failure` (21 tests before, 22 after).
- Commits happen only when the user asks; the "Commit" steps below are to be run then. Never stage the user's untracked `docs/superpowers/plans/2026-10-04-sqlite-object-store.md`, `reports/`, `research_notes/`.

## Review Focus

- A pasted address with spaces or a newline around it gets the same name as without them.
- `bm-2cX…` and `2cX…` get the same name as `BM-2cX…`, while a change of case in the base58 part changes the name.
- An empty address, or only `BM-`, gets an empty name at once, never a loop.
- An address containing `%1`, `%2` (QString::arg placeholders) is hashed literally.
- The first calls coming from several threads at once give the same names as one thread does.

All five are pinned by tests in Task 3.

---

### Task 1: coolname's generator, ported, with upstream parity

**Files:**
- Create: `third_party/coolname/LICENSE`, `third_party/coolname/README.md`, `third_party/coolname/data/` (config.json and 15 *.txt, copied)
- Create: `src/coolname.h`, `src/coolname.cpp`
- Create: `tests/coolname_tests.cpp`
- Modify: `CMakeLists.txt` (new block after `add_test(NAME i18n COMMAND i18n_tests)`)

**Interfaces:**
- Produces: `class bm::CoolName { explicit CoolName(const QSet<QString> &excluded = {}); quint64 combinations(int words) const; QStringList name(int words, quint64 index) const; bool acceptable(const QStringList &name) const; }`. Resources under `:/coolname/data/`.

- [ ] **Step 1: Vendor upstream's data**

```bash
cd /Users/user/w/ynotbit
SRC=$(mktemp -d)
git clone -q https://github.com/alexanderlukanin13/coolname.git "$SRC"
git -C "$SRC" checkout -q 7f895ee
mkdir -p third_party/coolname/data
cp "$SRC"/LICENSE third_party/coolname/LICENSE
cp "$SRC"/src/coolname/data/config.json "$SRC"/src/coolname/data/*.txt third_party/coolname/data/
ls third_party/coolname/data | wc -l   # 16
```

Write `third_party/coolname/README.md`:

```markdown
# coolname vocabulary

Word lists and patterns from [coolname](https://github.com/alexanderlukanin13/coolname)
by Alexander Lukanin (BSD-2-Clause, see LICENSE), copied unchanged from commit
`7f895ee` (2026-04-23): `data/config.json` and `data/*.txt`.

ynotbit names addresses with them (`src/coolname.cpp`). A name must never change,
so these files are never updated: a new word, a removed one or a reordered list
would rename every address. `excluded.txt` is ynotbit's own: words that read badly
attached to a person, left out of every name together with the phrases that
contain them.
```

- [ ] **Step 2: Write the failing test**

Create `tests/coolname_tests.cpp`:

```cpp
// Names for addresses: coolname's default generator, ported, checked against
// upstream (third_party/coolname, commit 7f895ee) value for value.
#include "coolname.h"
#include <QCoreApplication>
#include <iostream>
#include <stdexcept>

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
        std::cout << "PASS: coolname's names, counts and rules, as upstream\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
```

Add to `CMakeLists.txt`, after `add_test(NAME i18n COMMAND i18n_tests)`:

```cmake
# Names for addresses: coolname's vocabulary, pinned in third_party/coolname.
set(COOLNAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/coolname)
file(GLOB COOLNAME_DATA CONFIGURE_DEPENDS ${COOLNAME_DIR}/data/*)
qt_add_executable(coolname_tests tests/coolname_tests.cpp src/coolname.cpp src/coolname.h)
target_include_directories(coolname_tests PRIVATE src)
target_link_libraries(coolname_tests PRIVATE Qt6::Core)
qt_add_resources(coolname_tests "coolname_data" PREFIX "/coolname" BASE ${COOLNAME_DIR}
 FILES ${COOLNAME_DATA})
add_test(NAME coolname COMMAND coolname_tests)
```

- [ ] **Step 3: Run it to see it fail**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target coolname_tests`
Expected: FAIL to build: `coolname.h` not found (and `src/coolname.cpp` missing).

- [ ] **Step 4: Write the generator**

Create `src/coolname.h`:

```cpp
#pragma once
// Names from coolname's vocabulary (github.com/alexanderlukanin13/coolname,
// BSD-2-Clause), as its default generator makes them: the same lists, the same
// order, the same rules. The vocabulary is pinned in third_party/coolname.
#include <QSet>
#include <QStringList>
#include <memory>
namespace bm {
class CoolName {
  public:
    // coolname's default vocabulary, less these words and the phrases using them.
    explicit CoolName(const QSet<QString> &excluded = {});
    ~CoolName();
    // How many names there are of this many words (2, 3 or 4), rules aside.
    quint64 combinations(int words) const;
    // The index-th of them (index < combinations(words)), as its words; the
    // same name upstream has at that index.
    QStringList name(int words, quint64 index) const;
    // coolname's rules: no word twice, no two words starting with the same four
    // letters, at most 50 characters as a slug.
    bool acceptable(const QStringList &name) const;

  private:
    struct Tree;
    std::shared_ptr<const Tree> tree_;
};
} // namespace bm
```

Create `src/coolname.cpp`:

```cpp
#include "coolname.h"
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <deque>
namespace bm {
namespace {
QByteArray resource(const QString &path) {
    QFile file(":/coolname/" + path);
    if (!file.open(QIODevice::ReadOnly)) // a target built without the resource
        qFatal("coolname data missing: %s", qPrintable(path));
    return file.readAll();
}
// A word list's file: "#" starts a comment, "name = value" sets an option (none
// matters here), any other line is a word, or a phrase of words.
QList<QStringList> wordList(const QString &name) {
    static const QRegularExpression space("\\s+");
    QList<QStringList> items;
    for (auto line : QString::fromUtf8(resource("data/" + name + ".txt")).split('\n')) {
        line = line.section('#', 0, 0).trimmed();
        if (!line.isEmpty() && !line.contains('='))
            items << line.split(space, Qt::SkipEmptyParts);
    }
    return items;
}
} // namespace
// coolname's lists: words and phrases, a constant, lists one after another
// (nested) or one item from each list in turn (cartesian).
struct CoolName::Tree {
    struct List {
        enum Kind { Items, Constant, Nested, Cartesian } kind = Items;
        QList<QStringList> items;  // Items: a word, or a phrase's words, each
        QString value;             // Constant
        QList<const List *> lists; // Nested: longest first; Cartesian: in order
        QList<quint64> divisors;   // Cartesian: the lengths of the lists after each, multiplied
        quint64 length = 0;
        void append(quint64 i, QStringList &out) const {
            switch (kind) {
            case Items:
                out << items[qsizetype(i)];
                break;
            case Constant:
                out << value;
                break;
            case Nested:
                for (auto list : lists) {
                    if (i < list->length)
                        return list->append(i, out);
                    i -= list->length;
                }
                break;
            case Cartesian:
                for (qsizetype j = 0; j < lists.size(); ++j) {
                    lists[j]->append(i / divisors[j], out);
                    i %= divisors[j];
                }
                break;
            }
        }
    };
    std::deque<List> lists; // grows at the end only: the pointers to it stay valid
    QHash<QString, const List *> named;
    const List *make(const QJsonObject &config, const QString &key,
                     const QSet<QString> &excluded) {
        if (auto found = named.value(key))
            return found;
        auto &list = lists.emplace_back();
        const auto def = config.value(key).toObject();
        const auto type = def.value("type").toString();
        if (def.isEmpty()) { // not in config.json: a list of words of its own
            for (const auto &item : wordList(key))
                if (std::none_of(item.begin(), item.end(),
                                 [&](const QString &word) { return excluded.contains(word); }))
                    list.items << item;
            list.length = quint64(list.items.size());
        } else if (type == "const") {
            list.kind = List::Constant;
            list.value = def.value("value").toString();
            list.length = 1;
        } else {
            list.kind = type == "cartesian" ? List::Cartesian : List::Nested;
            for (const auto item : def.value("lists").toArray())
                list.lists << make(config, item.toString(), excluded);
            if (list.kind == List::Nested) {
                // Longest first, as upstream keeps them; ties stay in order.
                std::stable_sort(list.lists.begin(), list.lists.end(),
                                 [](const List *a, const List *b) { return a->length > b->length; });
                for (auto sub : list.lists)
                    list.length += sub->length;
            } else {
                list.divisors.resize(list.lists.size());
                quint64 after = 1;
                for (auto j = list.lists.size() - 1; j >= 0; --j) {
                    list.divisors[j] = after;
                    after *= list.lists[j]->length;
                }
                list.length = after;
            }
        }
        named.insert(key, &list);
        return &list;
    }
};
CoolName::CoolName(const QSet<QString> &excluded) {
    auto tree = std::make_shared<Tree>();
    const auto config = QJsonDocument::fromJson(resource("data/config.json")).object();
    for (const auto words : {"2", "3", "4"})
        tree->make(config, words, excluded);
    tree_ = std::move(tree);
}
CoolName::~CoolName() = default;
quint64 CoolName::combinations(int words) const {
    const auto list = tree_->named.value(QString::number(words));
    return list ? list->length : 0;
}
QStringList CoolName::name(int words, quint64 index) const {
    QStringList name;
    if (const auto list = tree_->named.value(QString::number(words)); list && index < list->length)
        list->append(index, name);
    return name;
}
bool CoolName::acceptable(const QStringList &name) const {
    QSet<QString> words, starts;
    auto length = name.size() - 1; // the hyphens
    for (const auto &word : name) {
        words << word;
        starts << word.left(4);
        length += word.size();
    }
    return words.size() == name.size() && starts.size() == name.size() && length <= 50;
}
} // namespace bm
```

- [ ] **Step 5: Run it to see it pass**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target coolname_tests && build/coolname_tests`
Expected: `PASS: coolname's names, counts and rules, as upstream`

- [ ] **Step 6: Commit (when the user asks)**

```bash
git add third_party/coolname src/coolname.h src/coolname.cpp tests/coolname_tests.cpp CMakeLists.txt
git commit -m "feat(names): coolname's generator in C++, name for name as upstream"
```

---

### Task 2: ynotbit's exclusions

**Files:**
- Create: `third_party/coolname/excluded.txt`
- Modify: `src/coolname.h`, `src/coolname.cpp`, `tests/coolname_tests.cpp`, `CMakeLists.txt` (the `qt_add_resources(coolname_tests …)` FILES)

**Interfaces:**
- Consumes: `bm::CoolName` (Task 1).
- Produces: `QSet<QString> bm::excludedWords();`, resource `:/coolname/excluded.txt`.

- [ ] **Step 1: Write the exclusion list**

Create `third_party/coolname/excluded.txt`:

```text
# Words ynotbit leaves out of names: they read badly attached to a person.
# A phrase containing one of them ("feral cat") goes with it. Changing this
# list renames addresses: see README.md.
# looks and body
bald bulky chubby curvy fat hairy lumpy meaty skinny thick shapeless girlish macho sexy hot juicy married desirable pleasurable
# identity, belief, origin
aboriginal native gay straight vegan devout orthodox heretic liberal illegal pygmy atheism feminism piety holiness conversion
# insults and bad character
annoying arrogant belligerent berserk cocky crazy daft daffy devious evasive fanatic greedy grumpy hysterical loutish manipulative naughty notorious outrageous pompous sarcastic sloppy sly snobbish uppish uptight vengeful demonic primitive ludicrous stereotyped tacky imperious aggressive dangerous unnatural kickass messy weird
# animals used as insults
pig hog swine warthog cow donkey mule rat weasel skunk snake viper leech worm earthworm slug vulture hyena jackal mongrel
# primates, long used as racist slurs
ape baboon bonobo capuchin chimpanzee galago gibbon gorilla lemur loris macaque mandrill marmoset monkey orangutan tamarin tarsier uakari
# fool slang
booby boobook turkey dodo loon coot cuckoo
# words that drop a whole phrase
feral naked killer blob foreign
# of-nouns: sexual, hostile
penetration domination ecstasy fertility anger fury attack criticism opposition superiority
# from-places
hell uranus
```

In `CMakeLists.txt`, change the resource line to include it:

```cmake
qt_add_resources(coolname_tests "coolname_data" PREFIX "/coolname" BASE ${COOLNAME_DIR}
 FILES ${COOLNAME_DATA} ${COOLNAME_DIR}/excluded.txt)
```

- [ ] **Step 2: Write the failing test**

In `tests/coolname_tests.cpp`, after `kUpstream`, add:

```cpp
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
```

and in `main`, before the `PASS` line:

```cpp
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
```

- [ ] **Step 3: Run it to see it fail**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target coolname_tests`
Expected: FAIL to build: `excludedWords` is not a member of `bm`.

- [ ] **Step 4: Load the list**

In `src/coolname.h`, after the class:

```cpp
// The words ynotbit leaves out of names (third_party/coolname/excluded.txt).
QSet<QString> excludedWords();
```

In `src/coolname.cpp`, before the closing `} // namespace bm`:

```cpp
QSet<QString> excludedWords() {
    static const QRegularExpression space("\\s+");
    QSet<QString> words;
    for (const auto &line : QString::fromUtf8(resource("excluded.txt")).split('\n'))
        for (const auto &word : line.section('#', 0, 0).split(space, Qt::SkipEmptyParts))
            words << word;
    return words;
}
```

- [ ] **Step 5: Run it to see it pass**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target coolname_tests && build/coolname_tests`
Expected: `PASS: coolname's names, counts and rules, as upstream`

- [ ] **Step 6: Commit (when the user asks)**

```bash
git add third_party/coolname/excluded.txt src/coolname.h src/coolname.cpp tests/coolname_tests.cpp CMakeLists.txt
git commit -m "feat(names): leave words that read badly on a person out of names"
```

---

### Task 3: A name for every address

**Files:**
- Modify: `src/coolname.h`, `src/coolname.cpp`, `tests/coolname_tests.cpp`

**Interfaces:**
- Consumes: `bm::CoolName`, `bm::excludedWords()` (Tasks 1–2).
- Produces: `QStringList bm::addressName(const QString &address);` (empty for an empty address).

- [ ] **Step 1: Write the failing test**

In `tests/coolname_tests.cpp`, add the includes `#include <atomic>`, `#include <thread>`, `#include <vector>`, and after `kFiltered`:

```cpp
struct Address {
    const char *address;
    QStringList name;
};
// The spec's scheme, computed in Python with upstream's generator and our list.
static const Address kAddresses[] = {
    {"BM-2cX8TF9vuQZEWvT7UrEeq1HN9dgiSUPLEN", {"positive", "zealous", "stoat"}},
    {"BM-5oSrHVwbsQYPKmePHfqQJZr6eibiqj9", {"industrious", "terrier", "of", "purring"}},
    {"BM-2cWnMMPofS3LHLT4dNZ6o1r5wbobcJvFh6", {"thundering", "alligator", "of", "acceptance"}},
    {"BM-%1%2", {"impressive", "coyote", "of", "modernism"}}, // hashed as written, % and all
    {"BM-retry-841", {"worthy", "brawny", "panda"}},          // its first candidate breaks a rule
};
```

and in `main`, before the `PASS` line (the threads come first: they are the first callers):

```cpp
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
                    bm::addressName("2cX8TF9vuQZEWvT7UrEeq1HN9dgiSUPLEN") == bm::addressName(digest),
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
        require(withOf == 3245 && withFrom == 267,
                "10,000 addresses fall into the patterns exactly as in the reference");
```

- [ ] **Step 2: Run it to see it fail**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target coolname_tests`
Expected: FAIL to build: `addressName` is not a member of `bm`.

- [ ] **Step 3: Name addresses**

In `src/coolname.h`, after `excludedWords()`:

```cpp
// An address's name: three words, the same for the same address in every
// ynotbit. A leading "BM-" (any case) and surrounding spaces do not count; an
// empty address has none.
QStringList addressName(const QString &address);
```

In `src/coolname.cpp`, add `#include <QCryptographicHash>`, and before the closing `} // namespace bm`:

```cpp
QStringList addressName(const QString &address) {
    static const CoolName names(excludedWords());
    auto key = address.trimmed();
    if (key.startsWith("BM-", Qt::CaseInsensitive))
        key = key.mid(3);
    if (key.isEmpty())
        return {};
    const auto count = names.combinations(3);
    // The first index, of a hash per attempt, whose name keeps the rules.
    for (int attempt = 0;; ++attempt) {
        const auto digest =
            QCryptographicHash::hash(("ynotbit-name:" + key + ":" + QString::number(attempt)).toUtf8(),
                                     QCryptographicHash::Sha256);
        quint64 index = 0;
        for (int i = 0; i < 8; ++i)
            index = index << 8 | quint8(digest[i]);
        if (const auto name = names.name(3, index % count); names.acceptable(name))
            return name;
    }
}
```

- [ ] **Step 4: Run it to see it pass**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target coolname_tests && build/coolname_tests`
Expected: `PASS: coolname's names, counts and rules, as upstream`

- [ ] **Step 5: Change the PASS line to what is now checked**

```cpp
        std::cout << "PASS: coolname's names as upstream, our exclusions, a name per address\n";
```

- [ ] **Step 6: Format, then run the full suite**

Run: `/opt/local/bin/clang-format -i src/coolname.h src/coolname.cpp tests/coolname_tests.cpp` (new files, so all of them), then the build, then `ctest --test-dir build --output-on-failure`.
Expected: `100% tests passed, 0 tests failed out of 22`

- [ ] **Step 7: Commit (when the user asks)**

```bash
git add src/coolname.h src/coolname.cpp tests/coolname_tests.cpp docs/superpowers/specs/2026-10-10-address-names-design.md docs/superpowers/plans/2026-10-10-address-names.md
git commit -m "feat(names): a stable three-word name for every address"
```
