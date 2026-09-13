# ynotbit — why not bit?

A compact desktop Bitmessage client based on [notbit](https://github.com/bpeel/notbit),
by [yshurik](https://github.com/yshurik). Development version: **0.2.0-dev**.

The sending workflow is under active implementation. The current GUI still only
saves drafts; the encrypted outbox and expanded wire codecs are foundational work,
not a claim of working end-to-end delivery.

A native Qt Quick desktop client foundation based on notbit. This first build
implements encrypted vault/mailbox documents, a keyless background relay, and
local incoming-message inspection. It is not yet a complete messaging client.

## Implemented

- Create/open portable `.bmvault` files. Argon2id (64 MiB, 3 passes) derives a
  wrapping key; XChaCha20-Poly1305 protects identities and random mailbox keys.
- Create/open `.bmmail` SQLCipher databases after unlocking their vault. Messages,
  subjects, addresses, drafts and scan state stay in the encrypted database.
- Password rotation, combined vault/mailbox backup, random identities, compatible
  deterministic chans (versions 3 and 4), and notbit/PyBitmessage `keys.dat` import.
- A separate process runs a notbit relay without constructing a keyring or IPC
  mail interface. No vault password, identity key or mailbox key goes to it.
- Lock closes the mailbox and releases guarded key allocations while the relay
  continues running. Unlock scans cached objects in bounded batches, verifies
  ECIES authentication, the sender signature and recipient binding, and stores
  matching direct/chan messages. Imported identities invalidate scan checkpoints.
- Per-cache checkpoints support moving mailbox files between machines. Replays
  deduplicate by inventory hash. Network expiry is separate from local retention.
- Local retention currently defaults to 512 MiB of indexed objects and 90 days.
  Pruning is visible in the status area. These limits are constants in this build.
- Qt desktop UI with a locked landing screen, mailbox reader, identities/chans,
  encrypted drafts, node status and cache activity.

## Not implemented yet

- Sending, outgoing proof-of-work scheduling, recipient public-key retrieval and
  publication, acknowledgments, retries, and a persistent outbox. The compose UI
  explicitly saves drafts only. New identities do not yet publish their public
  keys, so arbitrary peers cannot reliably start correspondence with them.
- Broadcast subscriptions and broadcast decoding. Chans are shared-key identities,
  a distinct feature; their members cannot be individually authenticated when
  sending with the shared chan identity.
- Native Windows relay support and single-EXE packaging. The current relay uses
  Unix APIs; a Windows GUI-only compile is not a functioning Windows client.
- Linux AppImage production/validation, signed/notarized macOS distribution,
  exhaustive interoperability testing, search, and attachment support.
- A security audit. Lock zeros guarded application key allocations and closes
  data access, but Qt/OS copies of displayed plaintext are not guaranteed erased
  from every memory buffer, swap file, crash dump or screenshot.

## Use the macOS build

The supplied `ynotbit.app` is an Apple Silicon development build requiring
macOS 15.6 or newer. Its runtime frameworks and libcrypto are bundled. It is
ad-hoc signed, not Developer ID signed or notarized.

1. Open the app and create a vault with a strong password.
2. Use **Identity** to create an identity, import an existing `keys.dat`, or join
   a chan. For existing chans, enter their expected address to verify the phrase.
3. Create or open a mailbox document in Documents or another writable folder.
4. Lock the vault. The node stays running; unlocking resumes mailbox inspection.
5. Use **File → Back up mailbox and vault** to preserve both documents.

The original `keys.dat` is not deleted or modified during import. Store that
plaintext source appropriately. A mailbox alone cannot recover its encryption
key. Password changes do not revoke old copies of the vault or old backups.

The node lives in Qt's platform application-data directory, under `node`.
`--data-dir /absolute/path` overrides its location. `--portable` uses
`./notbit-data/node` relative to the launch working directory. User documents
remain at the paths chosen in the file dialogs. The relay ends when the app exits.
`--offline` prevents network startup and is useful for inspecting local data.

## Build from source (macOS / Linux)

Requirements: CMake 3.22+, a C++20 compiler, Qt 6.8+ Core/Gui/Quick/QuickControls2/
Widgets, OpenSSL, libsodium, SQLCipher, pkg-config. Autoconf, Automake, make and Tcl
are needed to build dependencies. Python 3 runs the loopback integration test.

Dependencies can be installed by your package manager or built into a local
prefix with `scripts/build-dependencies.sh WORK_DIRECTORY INSTALL_PREFIX`.
The script checks the exact source commit IDs before building.

```sh
export PKG_CONFIG_PATH=/absolute/deps/lib/pkgconfig
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/absolute/Qt/6.8.3/macos \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

On Linux, point `CMAKE_PREFIX_PATH` at your Qt installation (usually `gcc_64` for
Qt's binary SDK). Linux builds and packaging have not been verified here.

To deploy on macOS, copy the built `.app`, then run Qt's `macdeployqt` with
`-qmldir=/absolute/path/to/ui`. Deployment must be retested from a relocated
bundle. Tests operate on temporary files and loopback peers; they do not send
messages to the public Bitmessage network.

## Code map

- `src/storage.*`: guarded secrets, authenticated vault, SQLCipher mailbox.
- `src/protocol.*`: notbit codec adapter, identities/chans, verified message decode.
- `src/cache.*`, `src/scanner.*`: node object index, retention and mailbox scans.
- `src/session.*`, `ui/Main.qml`: desktop state and interface.
- `third_party/notbit`: attributed upstream C sources with focused relay changes.
- `tests`: persistence, tampering, lock lifecycle, real loopback relay, GUI startup.

See `docs/design.md`, `docs/implementation-plan.md` and `THIRD_PARTY.md`.
