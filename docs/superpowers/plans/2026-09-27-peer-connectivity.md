# Fast, sustained peer connectivity implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. This document is a plan, not authorization to change production networking in the investigation turn.

**Goal:** Reach at least five usable peers quickly and replenish lost connections; retain an established-outbound target of eight.

**Architecture:** Separate the count and lifetime of in-progress connections from
established peers. Use bounded parallel discovery, monotonic setup deadlines,
outcome-aware retries, and durable successful-peer history. Keep the notbit and Qt
engines independent while enforcing equivalent externally tested behavior.

**Tech Stack:** C notbit engine, Qt 6/C++20 portable relay, CMake/CTest, Python 3 loopback peers.

**Spec:** [Measured investigation and evidence](../../investigations/2026-09-27-peer-connectivity/README.md).

## Global constraints

- Preserve the eight-established-outbound target; inbound peers cannot satisfy it.
- Support direct connections and existing SOCKS5 routing. Proxy mode must not
  leak destination lookups or dial direct peers as a fallback.
- Preserve `-e`, `-B`, `-b`, `-L`, offline mode, temporary test stores, and existing
  address admission rules. Do not enable incoming listeners as a workaround.
- No keys, identities, or mailbox content enter the relay. Do not log object
  contents. Routine status exposes aggregate counts, not the full peer graph.
- No hard-coded replacement public IP list. Measured good endpoints are controls,
  not shipped seeds.
- No public-network tests in CTest. Public timing is an explicit benchmark.
- Limits and timeouts below are initial design values, to be accepted or revised
  using the specified measurements rather than assumptions.

## Acceptance criteria

1. Controlled clean start with eight healthy candidates: at least five established
   peers within five seconds; never more than eight established outbound peers.
2. Seven stalled setups and five additional healthy candidates cannot hold the
   client at one peer; at least five established within 25 seconds.
3. Fatal protocol rejection frees its slot within one maintenance tick.
4. A warm cache of verified peers survives a restart after 125 minutes and after
   24 hours. Advertisement freshness remains separately enforced.
5. With available replacement peers, dropping three of eight established peers
   recovers to eight within 30 seconds. No repeated dialing of one failed endpoint
   before its backoff deadline, and no duplicate simultaneous attempts.
6. Public benchmark: warm start to five within 30 seconds, cold bootstrap to five
   within 180 seconds, five or more for at least 95% of samples after warm-up over
   30 minutes. These are release objectives conditional on measured network
   availability, not a guarantee a client can manufacture reachable peers.
   Record shortfalls and admission errors rather than silently weakening thresholds.

## Review focus

1. Socket callbacks remove entries synchronously during traversal: test multi-peer
   expiration and error bursts; no crash, double-free or stale counter.
2. Proxy stalls and DNS failures: preserve routing and apply finite setup budgets;
   no direct-network escape during recovery.
3. Restart and clock jumps: persist wall-clock history, use monotonic runtime
   deadlines, clamp imported future times and never underflow age arithmetic.
4. Poisoned or duplicate gossip: bounded address memory, stream-one filtering,
   address validation and randomized exploration; third-party timestamps cannot
   create “verified success.”
5. Quiet established peers and simultaneous handshake completions: retain peers
   that answer ping, cap established count correctly and avoid freeing the current
   callback object from an unsafe traversal.

## Task 1: Establishment deadlines and separate dialing capacity

**Files:**
- Modify `third_party/notbit/src/ntb-network.c`, `ntb-network.h`, `ntb-daemon.c`.
- Modify `src/relay_api.h`, `src/relay_bridge.cpp` for aggregate diagnostics.
- Create `tests/peer_connectivity_test.py`; modify `CMakeLists.txt`.
- Use `scripts/peer_probe.py` as the existing independent black-box reproducer.

**Interfaces:**
- Add `void ntb_network_tick(struct ntb_network *nw);`, called by the daemon
  before `ynotbit_relay_tick()` and `ntb_main_context_poll()`. Existing poll
  wake-up is already capped at 1,000 ms; do not use the minute-bucket timer for
  a 20-second deadline.
- Add aggregate snapshot API `void ntb_network_get_peer_stats(struct ntb_network *,
  struct ntb_network_peer_stats *);` with `established_outgoing`, `pending_outgoing`,
  `established_incoming`, `known_addresses`, `eligible_addresses`, and cumulative
  `attempts`, `setup_timeouts`, `rejections`. Mirror the declaration at the C/C++
  bridge boundary. Preserve existing `peers` and `pending` JSON meanings.
- Store per-peer `setup_started_us` from the monotonic clock. Count established
  state and pending state independently. Pending includes TCP, SOCKS and handshake.

- [ ] Extend the deterministic loopback fixture into a test with 13 listeners:
  the first eight accepted connections stall, one then completes its handshake,
  and each subsequently accepted endpoint immediately completes. Start with all
  13 explicit candidates, no DNS and a temporary directory. Poll `status.json`:

  ```python
  assert max(s['status']['peers'] for s in samples if s['seconds'] <= 25) >= 5
  assert all(s['stats']['established_outgoing'] <= 8 for s in samples)
  assert all(s['stats']['pending_outgoing'] <= 16 for s in samples)
  ```

  The current binary fails the first assertion, as the recorded probe proves.
  Add separate eight-healthy and simultaneous-completion cases. Fixture snapshots
  must combine status and recorded accepted sockets; do not infer TCP success
  from UI peer count.
- [ ] Register `peer-connectivity` in CTest and run it against the existing engine
  to establish failure before changing production code.
- [ ] Implement maintenance on the main loop. Initial constants: target 8,
  maximum pending 16 direct/4 proxy, setup deadline 20 s direct/60 s proxy,
  maximum four new attempts per second with an initial allowance of four.
  Use a monotonic token budget; repeated event-loop passes cannot bypass it.

  ```c
  /* Tick ordering; every traversal that removes peers uses the safe iterator. */
  expire_unestablished_peers(nw, now_us);
  count_peer_states(nw, &established, &pending);
  while (established < 8 && pending < pending_limit && consume_dial_token(nw, now_us)) {
          struct ntb_network_addr *addr = choose_eligible_address(nw, now_us);
          if (!addr) break;
          if (connect_to_addr(nw, addr)) pending++;
  }
  ```

  These static helpers belong in `ntb-network.c`: expiration calls `remove_peer`;
  counting examines `direction` and `state`; the initial chooser reuses the current
  randomized selection without endpoint repetition. Replace the old idle/timer
  connection queue with this single scheduler so two mechanisms cannot dial
  concurrently. Keep failure bookkeeping in one removal path.
- [ ] On handshake completion, if eight outbound peers are already established,
  close the surplus connection without penalizing that endpoint. Set state only
  after the capacity check. Stop new attempts at target; cancel leftover pending
  attempts from a subsequent safe maintenance pass, not inside another peer's
  callback. Inbound accounting remains separate.
- [ ] Add a monotonic fake-clock unit test for setup expiration at 19/20 seconds
  and proxy 59/60 seconds; incoming byte trickles cannot extend the absolute
  establishment deadline. Retain established peers when this deadline passes.
  Test an error burst freeing several peers in one tick and ensure stats remain
  consistent with the peer list.
- [ ] Run `ctest --test-dir build -R 'peer-connectivity|relay|netaddress' --output-on-failure`.
  Commit the deadline/scheduler change with tests and diagnostics together.

## Task 2: Parse fatal rejections and expose actionable failures

**Files:** Modify `third_party/notbit/src/ntb-connection.c`, `ntb-connection.h`,
`ntb-network.c`, and `tests/peer_connectivity_test.py`.

**Interfaces:** Add a rejection event carrying a bounded, sanitized reason,
severity and retry hint. Parse Bitmessage `error` fields: severity varint, ban-time
varint, inventory varstring, error-text varstring. Do not treat the whole payload
as ASCII. Reuse existing protocol readers and cap accepted reason text at 512 bytes.

- [ ] Add fixture packets for fatal, warning and malformed errors:

  ```python
  reason = b'Server full, please try again later.'
  fatal_packet = frame('error', b'\x02\x00\x00' + bytes([len(reason)]) + reason)
  # Fatal peer must disconnect; warning must not remove a healthy peer.
  assert fatal_peer.closed_within(2)
  assert warning_peer.still_connected()
  assert stats['rejections'] == 1
  ```

  Implement the fixture methods with EOF observation and a monotonic deadline;
  no sleeping assumptions. Test truncated varints, overlong reason text and an
  error arriving while SOCKS negotiation is incomplete (must stay in SOCKS parser).
- [ ] Add `error` to the command table. Emit rejection only after bounded parsing;
  severity 2 closes the connection, severity 0/1 logs without disconnecting.
  Return immediately after an event that may remove/free the current connection.
  Keep reason text safe for logs; maintain aggregate counters without exposing
  public endpoint lists in routine status.
- [ ] Apply a bounded retry hint through Task 3's cooldown policy; until then use
  the existing cooldown, never immediately retry inside an error callback.
- [ ] Run rejection, handshake-order and malformed-packet tests, then commit.

## Task 3: Durable outcome-aware peer selection

**Files:** Modify `third_party/notbit/src/ntb-network.c`, `ntb-store.c`,
`ntb-store.h`; extend `tests/peer_connectivity_test.py`. Add focused C policy
tests and their CMake target if needed to inject a clock/RNG cleanly.

**Data:** Extend address state with `last_success_wall`, `last_attempt_wall`,
`consecutive_failures`, and monotonic `next_attempt_us`. Persist local history
in a versioned sidecar `peer-state-v1.txt` using the store's asynchronous atomic
write pattern; preserve legacy `addr-list.txt` compatibility. One record contains
endpoint, stream, services, advertisement time, last local success, last attempt,
and failures. Missing/corrupt sidecar falls back to the legacy list and DNS.

- [ ] Pin these policy cases with a fake clock and seeded RNG before implementing:

  ```text
  local success 24h ago + remote advertisement 24h ago -> dialable, not advertised
  remote advertisement 150m ago + no local success -> dialable, advertised (3h window)
  remote advertisement 48h ago + no local success -> discarded (24h unverified retention)
  local success 29d ago + no fresher advertisement -> discarded
  failure at t -> ineligible before next_attempt; never parallel-dial same endpoint
  future timestamp / negative imported integer / duplicate endpoint -> clamped or rejected
  malformed sidecar -> legacy cache still loaded; no loss of bootstrap path
  ```

- [ ] Separate constants: advertisement window 3h, unverified history 24h,
  locally verified success retention 28d. A successful handshake updates local
  success; refresh it while that peer supplies valid protocol traffic. An address
  received through gossip never counts as a local success. Save successful history
  promptly and flush on orderly shutdown; do not wait ten minutes to save the
  first working peers.
- [ ] Select three out of four attempts from eligible locally successful peers
  when available; use the fourth for randomized discovery. If one pool is empty,
  use the other. Randomize within the pool and retain address/network-group
  diversity; never use a stable ordered prefix for every retry. Cap known
  addresses at 4,096, evict expired/failed unverified entries first, and validate
  stream, address and port on every load/admission path.
- [ ] On failure, compute exponential cooldown with bounded jitter: base 60 s,
  double per consecutive failure, cap at 3,600 s, measured from **failure time**.
  Honor bounded remote retry hints up to 3,600 s. A normal disconnect after a
  useful established session does not erase success. User disconnect, surplus
  cancellation and shutdown are neutral outcomes. A completed handshake resets
  consecutive setup failures.
- [ ] Ensure exponential arithmetic cannot overflow. Persist wall-clock metadata,
  reconstruct cooldown against current wall time at startup, then use monotonic
  runtime deadlines. Test wall-clock movement during a running attempt.
- [ ] Extend `peer_cache_probe.py` with verified-sidecar fixtures; run fresh,
  125-minute and 24-hour restart scenarios. Check old gossip is not advertised
  even while a verified endpoint remains eligible to dial. Test persistence after
  an orderly restart and after a crash following an atomic successful write.
- [ ] Run policy, connectivity and existing relay tests; commit independently.

## Task 4: Bootstrap recovery and honest discovery tests

**Files:** Modify `third_party/notbit/src/ntb-dns-bootstrap.c`, its header,
`ntb-network.c`, `tests/peer_connectivity_test.py`, and comments in
`tests/relay_test.py`/`THIRD_PARTY.md` that overclaim `getaddr` interoperability.

**Interfaces:** Bootstrap resolution returns candidate batches asynchronously to
the main loop. Use the project's existing worker/wakeup pattern; no detached
thread may outlive network teardown. One lookup in flight at a time.

- [ ] Test temporary DNS failure followed by recovery, empty successful response,
  cancellation at shutdown, and a proxied node with DNS disabled. Assert no
  direct DNS queries in the proxy-disabled-DNS case. Use an injected resolver;
  CTest must never resolve public seed names.
- [ ] Resolve at startup without blocking the peer/event loop. While below five
  peers and without usable candidates, retry seeds after 60 s, then 120/240/480 s,
  capped at 900 s with jitter. Reset after useful new candidates. Keep one
  bootstrap retry schedule; do not restart all healthy peers to rebootstrap.
- [ ] Test the actual discovery path: an established fixture sends unsolicited
  `addr`; the relay admits and dials eligible new peers. Also test the three-hour
  boundary and private-address filtering. Keep `getaddr` only as a harmless
  optional extension; do not claim its emission proves PyBitmessage support.
- [ ] Run bootstrap and relay tests, then commit. Public seed rotation is a
  benchmark observation, never an excuse to ship a newly hard-coded IP list.

## Task 5: Qt timeout crash and equivalent connection policy

**Files:** Modify `src/portable_relay.cpp`; reuse policy fixtures in
`tests/peer_connectivity_test.py` with `--qt-node`; modify `CMakeLists.txt`.

**Interfaces:** Extend Qt `Peer` with direction and monotonic establishment time;
provide the same aggregate status fields and policy values as Tasks 1–3. Use a
versioned Qt peer-state sidecar while accepting existing `qt-peers.json` entries
as unverified. The C and Qt stores need not share serialization.

- [ ] First register the existing timeout reproducer as a Qt test that fails on
  process death or a stale status timestamp. Run it against the current binary
  to confirm SIGSEGV at the timeout boundary.
- [ ] Make traversal safe under synchronous socket callbacks. Minimal pattern:

  ```cpp
  const auto snapshot = peers_;
  for (const auto &p : snapshot) {
      if (!p->socket || !peers_.contains(p)) continue;
      if (setupExpired(p)) {
          p->socket->abort();
          continue;
      }
      // Process only still-live peers; callbacks may remove other entries.
  }
  ```

  `setupExpired` uses monotonic elapsed time and Task 1's deadline. Audit every
  `peers_` traversal that can emit socket callbacks, not only the timeout branch.
  Deduplicate disconnected/error cleanup and keep snapshot-owned peer lifetimes
  valid. Validate with the reproducer and an address-sanitized build.
- [ ] Implement separate pending/established/inbound counts, bounded burst dialing,
  failure backoff and verified history. Replace iteration from the beginning of
  `QHash` on each tick with policy selection. Fatal error packets free slots.
- [ ] Run the same startup, starvation, rejection, churn, restart and proxy
  fixtures for both backends. Test multiple simultaneous expirations and errors.
  Commit crash repair separately from the broader Qt policy change if that makes
  review and rollback clearer.

## Task 6: Retention validation and public acceptance measurement

**Files:** Modify `third_party/notbit/src/ntb-connection.c`, `src/portable_relay.cpp`
for active idle liveness; extend `tests/peer_connectivity_test.py`; update the
investigation evidence with post-change benchmark results.

- [ ] Add a fake peer that stays quiet and replies only to `ping`. With a fake
  monotonic clock, advance through multiple keepalive periods and assert it stays
  connected. A peer that neither sends traffic nor answers ping must eventually
  disconnect and be replaced. No accelerated constants in release builds.
- [ ] Send `ping` after five minutes without useful traffic, accept valid `pong`
  as read activity, retain a ten-minute established read timeout. Avoid a ping
  every tick while awaiting a response. Apply this consistently to both backends;
  no keepalive writes before handshake completion.
- [ ] Run the focused suite and then one full `ctest --test-dir build
  --output-on-failure`. Use the platform CI matrix before release.
- [ ] Run public measurements **serially** against separate temporary stores:
  current baseline, modified cold start (DNS only), modified warm restart, and
  modified warm cache after a simulated 24-hour gap. Do not run PyBitmessage and
  ynotbit concurrently as the primary comparison: admission may be per public IP.
  A PyBitmessage comparison, if available, uses the same host/proxy and observation
  duration in a separate run.
- [ ] Record time to 1/3/5/8 established peers, attempts and failure categories,
  time spent below five, drop reasons, downloaded-object progress and maximum
  pending/total sockets. Run a 30-minute soak and a controlled three-peer drop.
  Compare with the acceptance criteria at the top; report unavailable peers and
  rejected admissions explicitly.
- [ ] Retain raw timestamped results and a compact before/after table. A temporary
  explicit-good-peer control is diagnostic evidence, not passing automatic
  bootstrap acceptance. Commit benchmark documentation only after recording it.

## Execution and rollback

Implement Tasks 1–3 first: they address the measured notbit bottleneck directly.
Task 4 repairs cold-start recovery; Task 5 is required before Windows claims;
Task 6 validates retention. Native execution in the current task is suitable:
the policy and lifecycle changes share interfaces and benefit from one coherent
implementation pass followed by review.

Keep commits separated at the task boundaries. Preserve old cache files, use a
sidecar for new metadata, and tolerate missing/newer sidecar versions so rollback
to the old binary remains possible. Do not change the user's active relay or
replace its executable until isolated regression and public acceptance results
are reviewable.
