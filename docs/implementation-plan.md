# Desktop Implementation Plan

Goal: implement the approved encrypted document lifecycle and connect it to a
keyless notbit relay with a usable Qt desktop front end.

Architecture: isolate network processing in a child process that never receives
vault credentials. The desktop owns secure storage and scans persisted network
objects using notbit protocol/ECC routines. Crash-safe SQL transactions protect
mailbox checkpoint advancement.

Tech stack: C++20, Qt 6.8, libsodium 1.0.20, SQLCipher 4.6.1, OpenSSL, vendored notbit.
Spec: design.md.

- [x] Core tests: locked access, wrong password, altered ciphertext, reopening,
  independent mailbox keys, interrupted transactions and duplicate object replay.
- [x] Vault: bounded versioned binary container, Argon2id, XChaCha20-Poly1305,
  guarded secret memory, atomic writes and exclusive file creation.
- [x] Mailbox: SQLCipher keyed before schema access, encrypted transactions,
  messages/drafts/checkpoints, SQLite backup to a separate encrypted database.
- [x] Protocol: reuse notbit address and ECIES parsing; validate signatures and
  destination binding before persisting incoming messages; deterministic chan fixture.
- [x] Relay: build notbit in a keyless mode, retain expired disk objects for local
  scans without advertising them; manage process with Qt, bounded cache cleanup.
- [x] Session: unlock/open, scan batches, deduplication, import identities, lock.
- [x] GUI: document dialogs, password controls, identities/chans, mailbox list,
  reading pane, drafts and visible network/scan/lock state.
- [x] Package and verify: real core integration tests and Qt offscreen launch,
  macOS deployment, build instructions and explicit remaining release gaps.

Acceptance examples:
`create vault -> create mailbox -> lock -> open with wrong password` must fail.
`receive object while locked -> unlock -> scan twice` must persist one message.
Changing any byte in vault ciphertext must prevent unlocking.
Copying the mailbox and vault to a new directory must preserve access.
A network child must never receive a password, mailbox key, or identity key.


## Remaining product work

This plan covers the implemented first development build, not the complete
product. Sending/PoW/pubkey exchange/acknowledgments, broadcasts, Windows relay,
Linux AppImage validation, release signing and security review remain. See README.
