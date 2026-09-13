# ynotbit — why not bit?

A compact desktop Bitmessage client based on [notbit](https://github.com/bpeel/notbit),
by [yshurik](https://github.com/yshurik). **Development version: 0.2.0.**

ynotbit keeps identity keys in a password-protected vault and correspondence in a
separate encrypted mailbox document. Its keyless relay continues collecting
network objects while the vault is locked. Unlocking inspects retained objects
and saves matching letters to the mailbox.

![ynotbit desktop with an editable encrypted draft](docs/images/desktop.png)

## Using the app

1. Create or open a `.bmvault` file. Create an identity, import `keys.dat`, or join
   a chan using its shared phrase and expected address.
2. Create or open a `.bmmail` document in Documents or another writable folder.
3. Choose **Write a letter**, select the sender, enter a recipient BM-address,
   and write. Changes automatically save in the encrypted mailbox.
4. Choose **Send letter**. Existing drafts have an **Edit / Send** control.
5. Follow progress in **Outbox**. Key lookup and proof of work can take time;
   recipient acknowledgment is asynchronous and is not a read receipt.
6. Lock the vault when finished. Preparation pauses, the mailbox closes, and
   the relay continues handling already submitted encrypted network objects.

**File → Back up mailbox and vault** saves both documents. Keep both: a mailbox
alone cannot recover its encryption key. Changing a password does not invalidate
old vault copies or backups. Import preserves the original plaintext `keys.dat`.

## Available in this development version

- Portable vault: Argon2id (64 MiB, 3 passes) and XChaCha20-Poly1305; guarded key
  allocations; password changes; identity labels; deterministic v3/v4 chans.
- SQLCipher mailbox: drafts, message bodies, addresses, public keys, delivery
  records, acknowledgment tokens, subscriptions, and checkpoints remain encrypted.
  Existing v1 mailbox documents migrate transactionally without losing drafts.
- Sending with public recipient keys; v2/v3/v4 public-key lookup and responses;
  cancellable background proof of work; persistent outbox and delivery history.
- Direct-message decryption and sender verification; acknowledgments; bounded
  automatic expiry retries; manual retry and cancellation; reply using the
  authenticated sender key. Cancellation cannot recall objects already relayed.
- Broadcast publishing/subscriptions (v4/v5 objects), shared chans, folder search,
  read state, archive, trash/restore, and explicit permanent deletion.
- A separate notbit relay receives no vault or mailbox keys. Its bounded local
  queue accepts network objects, validates proof of work, and records acceptance,
  rejection, and offers to connected peers. Receipt state survives restarts.
- Peer count, offline mode, node restart, additional peer and SOCKS5 proxy settings,
  configurable network-cache retention, recent document paths, and combined backup.

Delivery distinguishes **queued → requesting key → preparing receipt → proof of
work → waiting for peers → awaiting acknowledgment → acknowledged**. Broadcasts
and chan messages can finish as **published**, without a recipient receipt.
An offer to peers is not proof that every peer or the final recipient received it.

## Build and test

Requirements: CMake 3.22+, C++20, Qt 6.8+ (Core, Gui, Quick, QuickControls2,
Widgets, Network, Test), OpenSSL, libsodium, SQLCipher, pkg-config, Ninja.
Python 3 runs the loopback relay tests. Autoconf, Automake, make and Tcl are
needed when building the pinned storage dependencies from source.

```sh
scripts/build-dependencies.sh /absolute/scratch /absolute/deps
export PKG_CONFIG_PATH=/absolute/deps/lib/pkgconfig
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/absolute/Qt/6.8.3/macos \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

On Linux, use your Qt installation's `gcc_64` directory. The executable target
is `ynotbit`; macOS produces `ynotbit.app`. Tests use temporary documents and
loopback peers, never public-network messages. The independent wire fixtures can
be regenerated with `tests/generate_wire_fixtures.py` using Python cryptography
50.0.1; the normal test suite does not need that package.

To package an Apple Silicon build with Qt's runtime libraries:

```sh
scripts/package-macos.sh /absolute/build /absolute/ynotbit-macos-arm64.zip /absolute/Qt/6.8.3/macos
```

## Documents, network data, and portability

Vault and mailbox documents live wherever you choose. The node/cache directory
uses Qt's application-data location. Its internal application identifier remains
`NotbitDesktop/Notbit Desktop` for compatibility with the earlier alpha's cache.
`--data-dir /absolute/path` overrides the node folder. `--portable` uses
`./notbit-data/node` relative to the launch directory. `--offline` starts without
network connections. The relay stops when the application exits.

Default network retention is 512 MiB / 90 days; change it under **Network**.
Local retention can outlive protocol expiry, allowing later unlocked inspection.
Discarding a retained object can prevent later recovery; already saved mailbox
letters are unaffected. Proof of work uses one background CPU thread.

## Current boundaries

This remains development software. Local tests cover document migration,
malformed objects, protocol fixtures, real proof of work, controller delivery,
lock/reopen, desktop actions, and real loopback relay publication. This is not
an independent security audit or a public-network interoperability certification.
Lock releases guarded keys and closes access; Qt/OS copies of displayed plaintext
are not guaranteed erased from all memory, swap, crash dumps or screenshots.

The packaged macOS build targets **Apple Silicon, macOS 15.6+** and bundles its
runtime dependencies. It is ad-hoc signed, not Developer ID signed or notarized.
Native Windows relay support and single-EXE packaging remain unfinished. Linux
AppImage generation is not yet validated. Attachment UI, configurable CPU
parallelism, and large-mailbox paging are not implemented.

A Linux CI workflow template is in `docs/ci/build.yml`. It is not active: the
GitHub login used to create this repository lacks the `workflow` scope required
to publish `.github/workflows` files.

See [verification](docs/verification.md), [architecture](docs/design.md),
[implementation plan](docs/superpowers/plans/2026-09-13-complete-messaging.md),
and [third-party attribution](THIRD_PARTY.md). Original ynotbit code is MIT
licensed; vendored components retain their own licenses.
