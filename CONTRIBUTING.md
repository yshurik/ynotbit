# Contributing to ynotbit

Build instructions are in README.md. Run `ctest --test-dir build --output-on-failure`
before submitting changes. Use temporary vaults and loopback peers in tests;
never commit real vaults, mailboxes, keys.dat, passwords, or personal messages.

The relay must remain keyless. Persist message bodies, identity associations,
acknowledgment tokens and delivery state only inside the encrypted mailbox.
Changes to document formats need migration and reopen tests that preserve old data.
Protocol changes need malformed-input tests and interoperability evidence.

Development is ongoing. Check the README and implementation plans before assuming
that a feature is available end to end. Open an issue describing observable behavior,
platform, and version; omit secrets and private message contents from diagnostics.
