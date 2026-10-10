# New vault dialog — design (2026-10-10)

## Why

Creating a vault today asks for a file name, then for a password on the lock
screen, and makes an empty vault: "Vault created. Add an identity and create a
mailbox." The first address comes later, from Identity → Create identity…, with
a label typed into a bare input box. A new user meets three dialogs before
having an address, and none of them says what the address will be.

## Decisions

1. **One dialog makes the vault and its first identity.** File → Create vault…
   and the lock screen's "Create a new vault…" open it; no file dialog comes
   first. Create makes the vault with the identity in it, as its default.
2. **The address is shown before it is made.** The dialog generates the keys
   when it opens and shows the address; what Create saves is that address.
3. **Two ways to make keys:** random, from the system's entropy, or from a
   secret phrase, the way PyBitmessage derives a deterministic address (the same
   phrase gives the same address there, and to `scripts/keys-from-phrase.py`).
4. **Shorter addresses, as `tools/bm-shortaddr` (big1) finds them.** "Make it
   shorter" searches in the background, on every core but one, for keys whose
   RIPE hash starts with more zero bytes; each shorter address found replaces
   the one shown, and Stop keeps the best.
5. **The nickname is the address's name** (`bm::addressName`, in title case:
   "Mysterious Poised Shellfish"), and the vault file is named after the
   nickname in `~/Documents` ("mysterious-poised-shellfish.bmvault").
6. **The password is set in the same dialog.** Unlocking stays on the lock
   screen.
7. **No mailbox is created.** After Create the window shows "No mailbox open",
   as today.

## The dialog

A form, labels left, in two sections: **Identity** (nickname, address, keys)
and **Vault** (file, password). The dialog is as big as its rows: it grows as
the phrase and notes appear and shrinks back, and nothing in it is squeezed, in
any language. Every field reaches its right edge (also on macOS, where forms
keep fields at their size hint), and the file field is wide enough for the
whole default path of a long nickname. Rows stay put while the phrase is typed.

- **Nickname** — editable; starts as the address's name. Words are capitalised
  except "of", "from" and "the" after the first: "Noble Llama of Discourse".
  While the user has not edited it, it follows the address.
- **Address** — the BM-address in the fixed-width font, its identicon, a copy
  button and, for random keys, an "Another random address" button.
  Under it, "Find a shorter address" starts the search, beside the note
  "Optional. Shorter addresses are easier to read and type."; while it runs the
  button reads "Stop search" (one width for both) and the note says how fast it
  searches and how long the next shorter address takes on average. Changing the
  keys' source or the phrase stops the search.
- **Keys** — two choices:
  - *Random, from the system's entropy* (default).
  - *From a secret phrase* — a hidden field with an eye inside to show it, and
    the note "The same phrase always gives the same address. Anyone who knows
    or guesses it has your keys: use a long random phrase." The address follows
    the phrase as it is typed (key pair search restarted 300 ms after the last
    change); an empty phrase has no address. An address other than the
    phrase's first gets a line on how to rebuild it: "Key pair 27122 of this
    phrase: note the number with the phrase to rebuild this address." (what
    `keys-from-phrase.py --pair 27122` takes).
- **Vault file** — a path field and a "…" button opening the system save dialog
  (`.bmvault` filter). It starts as `~/Documents/<slug>.bmvault`, `-2`, `-3` …
  appended while that file exists, and follows the nickname until the user
  edits the field or picks a file. The slug is the nickname in lower case,
  letters and digits kept, every other run of characters one `-`; "vault" if
  nothing is left. A path that exists is refused under the field: existing
  vaults are never overwritten.
- **Vault password**, **Repeat password** — hidden fields, with the note "You
  need it to open the vault. A forgotten password cannot be recovered." A
  mismatch is said once the repeat is as long as the password.
- **Cancel**, **Create vault** — Create is enabled when there is an address, the
  passwords match and are not empty, and the file does not exist. While the
  search runs, Create uses the best address so far (and stops the search).

Closing the dialog stops the search and clears the phrase and password fields.

## Keys

`src/key_search.{h,cpp}`, beside `Protocol` (which keeps `identity()` and
`channel()`):

- `Identity phraseIdentity(const QString &phrase, const QString &label, quint64 pair)`
  — the keys of key pair `pair`: signing key = SHA-512(phrase + varint(2·pair))[:32],
  encryption key = SHA-512(phrase + varint(2·pair + 1))[:32] (the derivation
  `Protocol::channel` already uses), version 4, stream 1, not a chan.
- `std::optional<quint64> firstPair(const QString &phrase, int zeroBytes, quint64 limit)`
  — the first key pair whose RIPE hash starts with at least `zeroBytes` zero
  bytes. `firstPair(phrase, 1)` is the phrase's standard address, PyBitmessage's
  first deterministic address; `firstPair(phrase, 2)` is its "shorter" one.
- `class KeySearch : public QObject` — the background search, on every core but
  one, with libsecp256k1 (already linked for ECDH):
  - `start(const QString &phrase, int fromZeroBytes)` searches key pairs 0, 1, 2 …;
    `start(int fromZeroBytes)` searches random keys (32 bytes each from
    libsodium's `randombytes_buf`).
  - `found()` is emitted for each address shorter than the last one found, and
    `take()` hands over its `KeyFind {Identity identity; int zeroBytes; qint64
    pair}` (pair −1 when random): an `Identity`'s keys are a move-only `Secret`,
    so they never travel through a signal. For a phrase, a find is reported only
    once every lower pair has been searched, so it is the *first* pair with that
    many zero bytes, whatever thread found it: the result is the same on every
    machine, and matches `firstPair()`.
  - `progress(double pairsPerSecond)` once a second; `stop()`; the destructor
    stops and joins.

## Saving

`Session::createVault(const QString &path, QString password, QString repeated,
const Identity &identity)` — the checks `submitVaultPassword` makes for a new
vault (non-empty, matching, file does not exist, lock acquired), then
`Vault::create`, then `Vault::addIdentity(const Identity &)` (new overload: adds
the given keys instead of making random ones). It returns false and sets
`error()` on failure; the dialog stays open and shows it.

The inline create mode of the lock screen (the repeat field, the "Create vault"
button text, `showVaultPasswordFor`'s `create` flag), `Session::beginVaultCreate`
and the unused `Session::createVault()` go: nothing else creates vaults.

## Tests

- **Keys** (core tests): `phraseIdentity`/`firstPair` for "general" give
  `[chan] general`'s `BM-2cW67GEKkHGonXKZLCzouLLxnLym3azS8r` (the chan flag
  aside); for "ynotbit test phrase", `firstPair(…, 1)` = 60 →
  `BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN` and `firstPair(…, 2)` = 27122 →
  `BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6` (both from `keys-from-phrase.py`).
  `KeySearch` on that phrase reports 2 zero bytes at pair 27122 with any number
  of threads, and stops when told; a random search finds 2 zero bytes and its
  keys rebuild their address.
- **Dialog** (desktop tests): the nickname is the address's name in title case;
  the file is `~/Documents/<slug>.bmvault`, numbered past an existing one; the
  nickname drives the file until the file is edited; "Another" changes address,
  nickname and file; a phrase gives its address; an existing path, empty or
  mismatched passwords disable Create; Stop keeps the best address; both entry
  points open the dialog.
- **Create**: the vault file exists, the session is unlocked, and its only
  identity is the shown address with the nickname, as default.

## Later

- Creating the first mailbox with the vault.
- Importing a `bm-shortaddr` find (phrase + key pair) as an identity.
- The same dialog for Identity → Create identity….
