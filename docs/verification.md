# Verification — ynotbit 0.2 development

Validated locally on Apple Silicon macOS using Qt 6.8.3, SQLCipher 4.6.1,
libsodium 1.0.20, and OpenSSL 3.6.1. The macOS target is 15.6+ arm64.

Nine CTest suites cover:

- **core:** vault password/tamper rejection, password rotation and size rollback,
  SQLCipher wrong-key rejection, backup, atomic message/checkpoint failure,
  identity/chan compatibility, and signed ECIES interoperability.
- **lifecycle:** locked caching, later inspection of retained expired objects,
  identity/checkpoint invalidation, cache relocation, replay, pruning, malformed ECC.
- **outbox:** transactional v1 mailbox migration, editing existing drafts, durable
  delivery/ack state, job persistence, cancellation/retry, trash and subscriptions.
- **wire:** public-recipient encryption, v2/v3/v4 pubkeys, v4/v5 broadcasts,
  acknowledgments, independent Python fixtures, notbit interoperability,
  destination binding, truncation, corruption, canonical varints, object limits.
- **delivery:** two encrypted mailboxes, unknown recipient lookup, authenticated
  cached public key, real proof of work, stop/lock/reopen recovery, ciphertext
  handoff, decryption, acknowledgment, reply, deduplication, retry/cancel, broadcast.
  This controller test models relay receipts; the following tests exercise the
  actual network relay separately.
- **relay / relay-verack-first:** actual loopback TCP handshakes in both valid
  message orders, minimum-difficulty proof of work, inventory/getdata/object,
  disk persistence, local ciphertext publication, peer fetch, rejected malformed
  jobs, peer status, and absence of keys.dat/Maildir.
- **gui-smoke:** real QML application startup in offline mode.
- **desktop:** native file/password dialogs, identity creation, real draft editor
  and Send control, editing without duplication, cancellation, immediate lock
  flushing unsaved text, wrong-password rejection, and document restoration.

The expanded relay test exposed an upstream ordering bug: version-before-verack
never entered CONNECTED. Both handshake orders now have passing regression tests.

Tests never send messages to the public Bitmessage network. These checks are not
an independent security audit, public-network certification, or evidence of
working Windows/Linux release packages. See README for remaining platform work.

Release configuration: all nine suites passed. After the final nonce restart
change, the affected delivery and desktop suites passed again. The locked-receipt
and disconnected-peer waiting-state tests also passed. All ten independent
wire fixtures were regenerated and matched the checked-in byte strings exactly.
