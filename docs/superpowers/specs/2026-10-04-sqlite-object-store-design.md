# SQLite object store — design (2026-10-04)

## Why

The node keeps every network object as its own file in `<node>/objects/`, and the
app finds new ones by walking that folder. On a real node (29,170 objects):

- **Slow discovery.** Nothing tells the app that a file arrived. `Cache::discover()`
  walks the folder in a snapshot, at most 128 entries or 5 ms per 750 ms tick, so a
  new object waits for the rest of the current walk and part of the next. A
  just-sent broadcast took 12–20 minutes to reach Subscriptions. (An interim fix,
  commit `133d85b`, stopped `prune()` from restarting the walk at the size cap;
  the walk itself remains.)
- **Wasted disk.** The median object is 556 bytes and 89% are under 4 KB, but APFS
  allocates 4 KB blocks: 526 MiB of objects occupy 615 MiB (+17%).
- **Slow startup.** The engine opens all 29k files to read each one's 16-byte expiry
  (`ntb-store.c`, `process_file`).
- **Two indexes kept in step by polling:** the node's in-memory inventory and the
  app's `cache.sqlite`.

Raw filesystem load is not the problem; Bitmessage carries a few objects a minute.
The cost is two processes coordinating through a folder.

## Decisions

1. One SQLite file, `<node>/objects.sqlite`, replaces the `objects/` folder and the
   app's `cache.sqlite`.
2. **No migration.** The cache holds only public, re-downloadable network objects;
   letters live in the mailbox (`.bmmail`). On first start the node deletes
   `objects/`, the app deletes `cache.sqlite`, and the node re-syncs the live
   network inventory (about 4 minutes).
3. **The node is the only writer** and owns retention. The app only reads.
4. **SQL lives in C in the engine** (`ntb-object-db.{c,h}`); the app has its own C++
   reader. The schema text, version number and column names live in that one
   C header, which the app also includes.
5. **The Qt relay (`--qt-node`) is removed**, so there is one relay and one store.
6. Default size limit is **2048 MiB**; default age limit stays 90 days.

## Design

### The file

- `PRAGMA journal_mode=WAL` (the node writes while the app reads),
  `synchronous=NORMAL` (safe against process crashes; a power cut may lose the last
  few objects, acceptable for a cache), `auto_vacuum=INCREMENTAL` (set before the
  tables are created, so pruning can return space to the disk).
- Schema, `PRAGMA user_version = 1`:

  ```sql
  CREATE TABLE objects(
    seq      INTEGER PRIMARY KEY AUTOINCREMENT, -- arrival order, never reused
    hash     BLOB NOT NULL UNIQUE,              -- 32-byte inventory hash
    expires  INTEGER NOT NULL,                  -- from the object header
    received INTEGER NOT NULL,                  -- local arrival time
    size     INTEGER NOT NULL,                  -- length of payload
    payload  BLOB NOT NULL);                    -- whole object, as the file held it
  CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL); -- 'cache_id'
  ```

- A `user_version` other than the one the code expects means the node deletes the
  file and creates it afresh.
- When the node creates the file it writes a random `cache_id` to `meta`. The
  mailbox binds to it as it does today; a new file means a new id, and the mailbox
  re-reads from the start.

### Node (engine)

- **`ntb-object-db.{c,h}`** (new): synchronous functions with no threads — open
  (create schema, check version, delete a leftover `objects/` folder), save, load,
  list for startup, prune, stats. Its header is the shared schema header.
- **`ntb-store.c`** calls them. One connection: the startup listing uses it on the
  main thread before the store thread starts; afterwards only the store thread does.
- **Save** (`SAVE_BLOB`): `INSERT OR IGNORE`. Saves queued together commit in one
  transaction.
- **Load** (`LOAD_BLOB`, for a peer's `getdata`): `SELECT payload WHERE hash=?`.
  A pruned object returns no blob — the path a missing file takes today.
- **Startup listing:** `SELECT hash, expires WHERE expires > now - NTB_PROTO_EXTRA_AGE`
  fills the in-memory inventory; no per-object file opens.
- **Retention:** new options `-R <MiB>` (default 2048) and `-A <days>` (default 90).
  The store thread prunes every 60 s and whenever a save takes the total over the
  limit: delete lowest `seq` first until the total is ≤ 95% of the limit and no row
  is older than the age limit, then `PRAGMA incremental_vacuum`. The total size is
  kept in memory (summed once at open, adjusted per insert and delete). A pruned
  object stays in the in-memory inventory until it expires; a `getdata` for it gets
  nothing, as when the app deletes a file today.
- **Stats:** `ntb_store_object_stats(count, bytes)`, declared in `relay_api.h`;
  `relay_bridge.cpp` adds both to `status.json`.
- **Removed:** temp-file writing, renames, leftover `.tmp` cleanup, the startup
  directory scan, and `dump-store.c` (already excluded from the build).
- **Build:** `notbit_engine` links the SQLite library already in the build
  (`libsqlcipher`; with no key set it writes a plain SQLite file).

### App

- **`Cache` becomes a reader** of `objects.sqlite`. It opens the file read-write but
  only ever reads (a WAL reader needs write access to the `-shm` file), with a short
  `busy_timeout`, and without `SQLITE_OPEN_CREATE`: only the node creates the file.
  If the file does not exist yet, it returns nothing and tries again on the next
  tick. It never holds a transaction open across ticks, so the node's
  WAL checkpoints are not blocked.
- **`after(seq, limit)`** returns `{seq, hash, payload}`. `Delivery::scan`,
  `catchUpBroadcasts` and `scanMailbox` use `payload` instead of opening files.
  Objects over 256 KiB are ignored, as today.
- **Cache id** is read at open and checked each tick; if it changed (the node
  recreated the file), the app reopens and re-binds the mailbox.
- **`Session::tick`** no longer calls `discover()` or `prune()`; a new object is read
  on the next tick (≤ 0.75 s).
- **Status bar** object count and size come from `status.json`, not a query: a
  `SUM(size)` over rows that also hold the payload would read most of the file.
  When the node is not running (offline), the values it last wrote are shown.
- **Retention settings:** the default `retentionMB` becomes 2048 (range 64–32768
  unchanged); saving new values restarts the node with the new `-R`/`-A`
  (`restartNode()`); `startNode()` always passes them.
- **"Older letters may no longer be recoverable"** shows only when pruning removed
  objects this mailbox had not read: the lowest `seq` in the file is above the
  mailbox checkpoint + 1.
- **Upgrade:** the app deletes `<node>/cache.sqlite` at start.

### Removed

- `src/portable_relay.{cpp,h}`, the `--qt-node` branch in `main.cpp`, its
  `CMakeLists.txt` sources, the tests `qt-relay`, `qt-relay-verack-first` and
  `qt-peer-connectivity`, and the `--backend` choice in `scripts/peer_probe.py`.
  Investigation notes that mention it stay as history.
- `Cache::discover()`, `Cache::prune()`, and their tests in `lifecycle_tests.cpp`.

### Unchanged

`publish/` and `receipts/` (the handoff of the user's own outgoing objects),
`status.json` (apart from the two new fields), `addr-list.txt`,
`peer-state-v1.txt`, the mailbox.

## Testing

- **`tests/object_db_tests.c`** (new, built like `peer_policy_tests`): save then load
  returns the same bytes; a duplicate save is ignored; the startup listing omits
  long-expired objects; over the size limit, prune frees to ≤ 95%; prune by age;
  `seq` is never reused after deletes; vacuum shrinks the file; a version mismatch
  recreates the file; a leftover `objects/` folder is removed.
- **App tests:** `lifecycle_tests` and `delivery_tests` write fixture objects with
  `ntb_object_db_save`, the node's real writer, so each app test also checks the
  reader against the writer. New checks: a changed cache id reopens and re-binds;
  the unrecoverable-letters warning appears only for objects pruned before the
  mailbox read them; objects over 256 KiB are ignored; a new object is read on the
  next tick.
- **`tests/relay_test.py`** waits for the object's row in `objects.sqlite` instead
  of a file.

## Acceptance

1. When the app is not catching up a backlog, a new object is read within one tick
   (≤ 0.75 s). Tested.
2. After a full re-sync on a real node, `objects.sqlite` is at most 1.05× the
   object data size (today 1.17×). Measured.
3. The node lists 30k objects at startup in under 1 s. Measured.
4. All tests pass on macOS, and the Windows (`win-notbit.yml`) and Linux AppImage
   CI jobs pass.

## Risks

- **WAL growth** if a reader holds a transaction open: the app finalizes every
  statement within a tick.
- **First start after upgrade** shows only live network objects; retained history
  older than that is gone (accepted: decision 2).
- **Windows build** must find SQLite for the engine target through vcpkg, as the app
  already does. Verified by the Windows CI job.

## Out of scope

Moving `publish/` and `receipts/` into the database; encrypting `objects.sqlite`;
the node accepting objects larger than 256 KiB (a real node holds four of 0.3–3.5
MB) — a separate network-limits fix.
