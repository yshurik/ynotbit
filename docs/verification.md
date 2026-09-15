# Verification — ynotbit 0.4 Widgets development

Validated locally on Apple Silicon macOS using Qt 6.8.3, SQLCipher 4.6.1,
libsodium 1.0.20, and OpenSSL 3.6.1. The macOS target is 15.6+ arm64.

The CTest suite covers:

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
- **gui-smoke:** Qt Widgets application startup in offline mode.
- **desktop:** an encrypted 1,500-message mailbox (10 KB per body), scrolling to
  distant rows, a three-page cache bound, folder search, full-body selection,
  visual Markdown rendering, blocked local image resources, composing through
  the persistent outbox, system/light/dark palette selection, plaintext view
  clearing on lock, styled unlock rejection/retry, recent-mailbox restoration,
  and saving an active editor before locking.
- **qt-relay / qt-relay-verack-first:** the portable Qt relay's loopback handshake
  and publication path on platforms without the Unix notbit engine.

The expanded relay test exposed an upstream ordering bug: version-before-verack
never entered CONNECTED. Both handshake orders now have passing regression tests.

Tests never send messages to the public Bitmessage network. These checks are not
an independent security audit or public-network certification.

Run the complete suite with `ctest --test-dir build --output-on-failure` after
configuring Qt and the encrypted-storage dependencies.

## Widgets 0.4.0 performance check

A native Cocoa run of `build/desktop_tests` on 14 September 2026, using the
synthetic mailbox above and an empty offline network cache, measured:

- Vault unlock plus mailbox open: 640 ms (includes password derivation).
- Physical footprint after opening the mailbox: 56.5 MiB.
- 60 scroll positions across 1,500 rows: 384 ms total.
- 50 selections rendering 10 KB bodies: 921 ms total (including UI event processing).
- Physical footprint after scrolling and selection: 71.2 MiB.
- Another 500 selections: footprint remained between 71.19 and 71.27 MiB.

Footprint uses macOS `TASK_VM_INFO.phys_footprint`. These are workload-specific
measurements of the desktop test process, including fixture-creation allocations,
not a guarantee for arbitrary messages or proof of absence of leaks. Tests use no
user documents. The list caches no more than 300 truncated summaries, and only
the selected message body is retained by the reader. The packaged application
has no Qt Quick/QML engine or runtime dependency.

Full-text search and delivery inspection still execute on the main thread.
Unfiltered list access uses a folder/time index; deep OFFSET seeks and searches
can still get slower with very large mailboxes. Further background storage work
would be needed to guarantee bounded latency for those operations.

The final local suite passed 11/11 (38.24 seconds). The extracted, ad-hoc-signed
Apple Silicon bundle passed signature/dependency checks and a native offline
launch; vmmap reported a 52.4 MiB idle physical footprint with no mailbox open.
The ZIP is 13,292,671 bytes. No Quick or QML libraries are bundled.

## 0.4.1 channel navigation and address typography

The desktop regression now includes two separate channel recipients and a joined
channel with no messages. It verifies recipient-filtered counts and rows, search
isolation, clearing the old reader, selection retention across folder navigation,
and the sender/recipient prefilled by Write to channel. Existing 1,500-row paging
and lock/composer checks remain in place.

Address typography checks compare actual glyph advances for narrow and wide
characters. Native macOS passed; the offscreen platform exposed a proportional
FixedFont fallback, now handled by selecting an installed fixed-pitch font.
BM-addresses in document text are styled by a highlighter without changing the
stored Markdown. Other core, lifecycle, delivery, and relay tests passed during
this change; the desktop and startup gates were rerun after the font correction.
