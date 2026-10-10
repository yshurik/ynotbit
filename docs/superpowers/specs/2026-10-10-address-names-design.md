# Names for addresses — design (2026-10-10)

## Why

A Bitmessage address is 36 characters of base58. Identicons already give each
address a picture that stays the same everywhere; a name would do the same in
words: `lush-space-shellfish` is easier to recognise and to say than
`BM-2cX8TF9vuQZEWvT7UrEeq1HN9dgiSUPLEN`.

[coolname](https://github.com/alexanderlukanin13/coolname) (Python, BSD-2-Clause)
makes such names from a hand-curated vocabulary. No C++ port exists (a Go port,
`sio/coolname`, and a Dart one do), so we port the parts we need.

## Decisions

1. **A name per address.** The same address always has the same name, in every
   copy of ynotbit, in every version: the name is computed from the address, never
   stored or chosen.
2. **Three words.** 3-word names are about 20–25 characters and there are 277
   million of them after the exclusions below, so two of one person's contacts
   are unlikely to share a name.
3. **coolname's vocabulary and rules, pinned.** The word lists are copied from
   upstream commit `7f895ee` (2026-04-23) and never updated: any change to the
   words, their order or the hashing renames every address.
4. **Unflattering words are excluded.** A name is attached to a person, so words
   that read as a judgement of their body, identity or character are left out
   (list at the end). A name with an excluded word is never given: it is picked
   again, as one breaking coolname's rules is. So a word excluded later renames
   only the addresses whose names contain it, not every address.
5. **A name is a hint, not an identity.** Anyone can generate addresses until one
   gets a name they want, so the UI must never let a derived name pass for a
   contact's name or stand in for the address. (UI work comes after this.)

## Design

### Files

- `third_party/coolname/` — upstream's `LICENSE`, `data/config.json` and
  `data/*.txt` as of `7f895ee`, unchanged; `excluded.txt`, our list; a `README.md`
  naming the upstream commit and what we changed (nothing in the data, the
  exclusions apart).
- `src/coolname.{h,cpp}` — the generator.
- `tests/coolname_tests.cpp` — its tests, a new `coolname` ctest.

The data is embedded with `qt_add_resources` (as `:/coolname/...`) and parsed once,
on first use.

### The generator

A port of coolname's default generator, enough to reproduce it exactly:

- **Lists**, as in `config.json`: word lists and phrase lists (`*.txt`), constants
  (`of`, `from`), nested lists (one after another) and cartesian lists (one item
  from each). The `.txt` format: `#` starts a comment, `name = value` lines before
  the words set `max_length` and `number_of_words`, every other line is a word or,
  with `number_of_words = 2`, a two-word phrase.
- **Indexing**, as upstream: a nested list holds its lists longest first (stable,
  so ties keep config order) and duplicates are kept (`dragon` is both an animal
  and a legendary one); a cartesian list is a mixed-radix number over its lists in
  config order. `name(words, i)` is therefore exactly upstream's
  `_lists[words][i]`, which the tests check.
- **Rules**, as upstream: a name is rejected if a word repeats, if two words share
  their first 4 letters, or if the slug is longer than 50 characters.

```cpp
namespace bm {
class CoolName {
  public:
    // coolname's default vocabulary, less these words and the phrases using them.
    explicit CoolName(const QSet<QString> &excluded = {});
    // How many names there are of this many words (2, 3 or 4), rules aside.
    quint64 combinations(int words) const;
    // The index-th of them (index < combinations(words)), as its words.
    QStringList name(int words, quint64 index) const;
    // Upstream's three rules.
    bool acceptable(const QStringList &name) const;
};
// The words ynotbit leaves out of names (third_party/coolname/excluded.txt).
QSet<QString> excludedWords();
// An address's name: three words, our exclusions applied.
QStringList addressName(const QString &address);
}
```

### Names for addresses

`addressName(address)` hashes `"ynotbit-name:" + address + ":" + k` with SHA-256,
for k = 0, 1, 2, …; the first 8 bytes, as a big-endian number modulo upstream's
whole 3-word count (365,733,117), are an index, and the first index whose name
keeps the rules and has no excluded word is the address's name. The address is trimmed and a leading `BM-`, in any case,
is dropped, so `BM-2cX…`, `bm-2cX…` and `2cX…` share a name; the rest is used as
written (base58 is case-sensitive). An empty address has no name. The modulo bias
is about 1.5 × 10⁻¹¹, nothing.

### Tests

Reference values come from running upstream at the pinned commit.

- **Upstream parity, nothing excluded:** `combinations()` is 370,170 / 365,733,117
  / 83,564,101,798 for 2 / 3 / 4 words; `name(3, i)` equals upstream's for a spread
  of indices (first, last, every pattern's boundary, random ones).
- **With our exclusions:** 306,528 / 277,275,309 / 59,033,406,074, matching
  upstream's `filter_config` with the same list; sampled names match too.
- **Addresses:** fixed address → name pairs (so an accidental change fails the
  build's tests); over 10,000 addresses every name passes the rules, contains no
  excluded word, and the names fall into the patterns exactly as a reference
  implementation's do (3,249 with "of", 262 with "from"); excluding more words
  renames only the names that have them.

## Later

- Where names appear (senders not in contacts, chan members, subscriptions) and how
  they are marked as derived, not chosen.
- Random names (upstream's `generate()`), if something needs them: `name()` with a
  random index and the same rules.

## Exclusions (136 words, for review)

Looks and body: bald, bulky, chubby, curvy, fat, hairy, lumpy, meaty, skinny,
thick, shapeless, girlish, macho, sexy, hot, juicy, married, desirable, pleasurable.

Identity, belief, origin: aboriginal, native, gay, straight, vegan, devout,
orthodox, heretic, liberal, illegal, pygmy, atheism, feminism, piety, holiness,
conversion.

Insults and bad character: annoying, arrogant, belligerent, berserk, cocky, crazy,
daft, daffy, devious, evasive, fanatic, greedy, grumpy, hysterical, loutish,
manipulative, naughty, notorious, outrageous, pompous, sarcastic, sloppy, sly,
snobbish, uppish, uptight, vengeful, demonic, primitive, ludicrous, stereotyped,
tacky, imperious, aggressive, dangerous, unnatural, kickass, messy, weird.

Animals used as insults: pig, hog, swine, warthog, cow (and its breeds' cows),
donkey, mule, rat, weasel, skunk, snake, viper, leech, worm, earthworm, slug,
vulture, hyena, jackal, mongrel.

Primates (long used as racist slurs): ape, baboon, bonobo, capuchin, chimpanzee,
galago, gibbon, gorilla, lemur, loris, macaque, mandrill, marmoset, monkey,
orangutan, tamarin, tarsier, uakari.

Fool slang: booby, boobook, turkey, dodo, loon, coot, cuckoo.

Words that drop a whole phrase: feral (six "feral …" animals), naked ("naked
mole"), killer ("killer whale"), blob ("blob fish"), foreign ("foreign country").

Of-nouns, sexual or hostile: penetration, domination, ecstasy, fertility, anger,
fury, attack, criticism, opposition, superiority.

From-places: hell, uranus.

**Kept, though borderline** (move any to the list above): beaver, chicken, goose,
mosquito, termite, locust, hungry, loud, noisy, bizarre, strange, peculiar,
witchcraft, sorcery, "the government", "gila monster", "vampire bat",
"vampire squid".
