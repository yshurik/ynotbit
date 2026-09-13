# Verification — development build 0.1

Verified on Apple Silicon macOS with Qt 6.8.3, SQLCipher 4.6.1, libsodium 1.0.20,
and OpenSSL 3.6.1. Build target is macOS 15.6+ arm64.

Release configuration: all five CTest suites passed, 0 failures.

1. **core**: wrong password and ciphertext tampering rejected; password rotation;
   oversized vault mutation rollback; mailbox encryption and wrong-key rejection;
   transaction failure rolls back message and checkpoint together; duplicate
   suppression; backup restoration; known version-3 general chan address; signed
   ECIES encode/decode and tampering rejection.
2. **lifecycle**: objects cached with the vault locked; unlock processes retained
   expired objects; replay deduplicates; identities added with a mailbox closed
   trigger rescanning; moving to another cache resets the checkpoint; pruning
   retains monotonically increasing sequence IDs; malformed EC coordinates rejected.
3. **relay**: real local TCP peer handshake, valid network proof of work, inventory
   request and object persistence with no vault/keyring. No public-network messages.
4. **gui-smoke**: the real Qt/QML desktop starts and exits in offline mode.
5. **desktop**: actual Qt file/password dialogs, identity creation, draft saving,
   clearing views on lock, wrong-password rejection and document restoration.

A separate source review found two issues (oversized vault writes and identity
checkpoint invalidation); both were fixed and covered by failing-then-passing
regression tests.

The packaged app was copied outside its build directory and passed offline
startup plus the loopback relay test. 87 Mach-O files were inspected; no absolute
non-system library dependencies remained. The app passed ad-hoc code-signature
verification in its packaging staging directory. It is not notarized.

Qt's offscreen test plugin emits expected platform/font diagnostic messages.

This is validation of an initial development build, not a cryptographic audit,
public-network interoperability certification, or validation of Windows/Linux
release artifacts. Sending, acknowledgments and broadcast subscriptions remain
unimplemented; see README for the complete feature boundary.
