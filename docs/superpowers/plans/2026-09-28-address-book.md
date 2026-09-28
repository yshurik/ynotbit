# Address book — research and plan (2026-09-28)

## Why

ynotbit shows every correspondent as a raw 36-character `BM-…` address. There
is no way to name a person, no way to pick a recipient except pasting an
address, and the list column never says who a letter is from. That is the
biggest day-to-day usability gap left.

## Research

**PyBitmessage (the reference client).** An `addressbook(label, address)` table
in `messages.dat`. A dedicated Address Book tab with add / rename / delete,
"Send message to this address", "Copy address", "Subscribe". The inbox context
menu has "Add sender to address book". The To field accepts `Label <BM-…>` and
completes against the book. Everywhere an address is shown, it is replaced by
the best known name: own identity label, then address-book label, then
subscription label, then the raw address. Nothing is added automatically.

**Mail clients generally (Thunderbird, Apple Mail, Delta Chat).** The common
conveniences: one click to save the sender of the letter you are reading;
autocomplete in the To field by name *or* address; names instead of addresses
in the list; the address still visible (and copyable) in the reading pane so a
name never hides who a letter really came from.

**This codebase.**
- The vault holds only keys; the design doc puts labels and private state in
  the mailbox (SQLCipher). Subscriptions already live there
  (`subscriptions(address, label)`), with the same shape an address book needs.
  → Contacts go in the mailbox, encrypted at rest, travelling with the mailbox.
- `Session::messagePage` builds the list rows; `LetterDelegate` paints them from
  model roles. The reader shows `From` / `To` as monospace address labels.
- `Wire::validAddress` validates addresses.
- Pages are selected by row in a hidden `folders_` list driving an icon rail;
  Identities is the last row (8). Appending Contacts as row 9 shifts nothing.

## Design

**Security posture.** A contact name is a local, private label. It is never
sent anywhere and never trusted from the network: names come only from what
the user typed. The reader always keeps the full address next to a name, so a
contact name cannot be used to disguise a different sender.

**Name resolution** (`Session::nameFor(address)`), first match wins:
own identity / chan label → contact → subscription → none.

**Storage.** `contacts(address TEXT PRIMARY KEY, label TEXT NOT NULL,
added INTEGER NOT NULL)` in the mailbox, created by the existing migration.
`Mailbox::contacts()`, `saveContact(address, label)` (insert or rename),
`removeContact(address)`.

**Session API.** `contacts()`, `addContact(address, label)` → error text or
empty (validates the address, refuses own identities, empty name falls back to
the address), `renameContact`, `removeContact`, `nameFor`. Changes emit
`changed()` so every view refreshes.

**UI.**
1. *Contacts page* — a new rail icon (after Identities). Header "Contacts",
   filter box, "Add contact…". Cards like the identity cards: identicon, name,
   address with copy, and Write / Rename / Delete. An empty state explains the
   two ways to add someone.
2. *Add contact dialog* — Name + Address; the address is pre-filled from the
   clipboard when it holds a valid address; live validation; OK only when valid;
   an address already in the book shows its current name and saving renames it.
3. *Reader* — `From` / `To` show `Name` then the address. An unknown, foreign
   address gets a small "Add to contacts" button that opens the dialog with the
   address filled in. Same in the pop-out window.
4. *List* — the meta line reads `Name · date` when the correspondent is known.
5. *Composer* — the To field completes on name or address (the popup shows
   "Name — BM-…", choosing inserts the address), a contacts button next to To
   lists them all, and a line under To shows who the typed address is.

**Out of scope for now.** Import/export, groups, notes, avatars, blacklist /
whitelist, `bitmessage:` URIs.

## Tests

- Storage: add, rename by re-saving, remove, persistence across close/open,
  ordering by name.
- Session: validation (bad address, own identity), empty-name fallback, name
  precedence identity > contact > subscription.
- Widgets: Contacts page lists cards and filters; Write opens the composer to
  that address; the reader's add button shows only for unknown foreign
  addresses and disappears once added; names appear in the reader and in the
  list role; the composer completer offers contacts and the To hint names them.
