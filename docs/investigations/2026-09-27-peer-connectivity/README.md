# Peer connectivity investigation — 27 September 2026

## Conclusion

The main bottleneck is **finding and scheduling usable peers**, not a general
Bitmessage handshake incompatibility. The unchanged production binary connected
to five measured-good public endpoints within the first five-second sample and
held all five through 180 seconds while downloading objects. Meanwhile, the
existing desktop relay spent 35 of 37 samples at zero peers with all eight
outbound slots occupied by SYN_SENT sockets.

Do not raise the connected-peer target: it is already eight. Separate pending
attempts from established peers, enforce a short establishment deadline, learn
from connection outcomes, and retain useful peer history across restarts.

Implementation is specified in
[the implementation plan](../../superpowers/plans/2026-09-27-peer-connectivity.md).
This investigation changes only diagnostics and documentation.

## Environment and limits

- Source: `eb5324b`, clean tracked working tree at investigation start.
- Existing binary: `build/ynotbit.app/Contents/MacOS/ynotbit`, built
  2026-09-26 21:13:43 local time, on this macOS host.
- Running desktop backend: `--node` (vendored notbit), outgoing only (`-i`),
  no proxy in its process arguments. The desktop was left running unchanged.
- Observations: approximately 07:08–07:17 UTC. Cache snapshot had 327 stream-one
  public endpoint records. DNS returned seven endpoints, six already in the cache.
  The reusable control command was additionally checked at 07:21 UTC.
- Public survey: 328 distinct endpoints, concurrency 12, five-second TCP and
  handshake deadlines, ten-second post-handshake observation, 125.7 seconds total.
  No identities, keys, message decryption, or local message publication.
- Control relay used a temporary empty store and five explicit endpoints. It
  downloaded and relayed normal public objects. Its temporary store was removed.
- PyBitmessage comparison is source-based, pinned to official repository commit
  `dcbcc4a2fd74a9c7119fa48c5a54457a6ef8887a`; PyBitmessage itself was not run.
- Public success is time-dependent. Concurrent nodes behind the same public IP
  can influence admission. A five-second timeout means “not successful within
  five seconds,” not “permanently dead.” Three minutes proves short-term
  retention, not an overnight stability guarantee.

## Measurements

| Experiment | Measured result | What it establishes |
|---|---|---|
| Running desktop, 37 samples over 181.4 s | 35 samples: 0 peers and 8 SYN_SENT; final 2 samples: 1 peer | Pending TCP attempts exhaust the connection budget in the actual app |
| DNS + cached endpoint survey | 14/328 TCP successes; 5/328 complete handshakes; 261 TCP timeouts, 50 refusals, 3 other network errors | Uniform selection operates on a very low-yield pool |
| Reachable but rejected peers | 9/14 TCP successes returned fatal `error`: 8 “Too many connections from your IP”, 1 “Server full” | TCP connectivity is not a usable peer; rejection handling matters |
| Unchanged relay with the five successful endpoints | 5 peers by 5.011 s; 5 in every sample through 180.263 s | Existing wire implementation can meet the requested count quickly when selection succeeds |
| Loopback: 8 initial attempts, 7 silent, 1 handshaken, 5 healthy untried candidates | 1 peer for the 45 s observation; 6 by 48.22 s after freeing stalled sockets at about 46 s | Scheduler starvation reproduced independently of internet conditions |
| Same loopback, seven fatal error packets instead of silence | Stays at 1 until remote sockets are closed; 6 by 23.16 s | Fatal error packets are ignored, leaving unusable connections allocated |
| Five identical healthy loopback cache entries, timestamp age 0 versus 7,500 s | Fresh: 5 connections within 5 s; aged: 0 attempts, 0 peers | Two-hour cache filtering discards usable restart candidates |
| Qt backend, same stalled-peer probe | Reproduced SIGSEGV, return code -11, around 33 s | Timeout cleanup has a separate crash path |
| Later 10-second check of the reusable control command | All five endpoints accepted TCP; only three completed handshakes during this short run | Admission and setup success vary; an old successful list cannot guarantee five peers on every run |

The five successful public handshakes took 0.351–3.062 seconds (four
PyBitmessage 0.6.3.2 nodes, one MiNode 0.3.3). Successful peers advertised 806
distinct public stream-one endpoints; 140 were between two and three hours old
at survey completion. The notbit admission cutoff is only two hours. Those 140
were not individually proven reachable; they quantify discarded discovery,
not 140 lost live connections.

Socket samples tracked several uninterrupted SYN_SENT intervals of approximately
75.5 seconds. Some intervals are censored by sampling boundaries. There is no
application TCP deadline; the OS determines when these attempts fail.

A supplemental bounded survey of 120 DNS/newly-advertised endpoints returned
four TCP successes and one handshake. It overlapped the five-peer control and
is not an independent admission-rate estimate; it is not used to rank fixes.

## Root causes and confidence

### 1. Pending attempts consume the eight-peer target — confirmed, primary

`third_party/notbit/src/ntb-network.c:477` increments `n_outgoing_peers` as soon
as the asynchronous connection object exists. Both `connect_queue_cb()` and
`maybe_queue_connect()` stop at eight regardless of handshake state. In contrast,
`ntb_network_connected_peers()` correctly counts completed handshakes for the UI.
The UI is reporting the real shortage, not undercounting eight healthy peers.

`third_party/notbit/src/ntb-connection.c:769` uses the same ten-minute inactivity
limit for incomplete and established connections, checked by a three-minute
timer. Silent handshakes can occupy slots for roughly 10–13 minutes; incoming
bytes can prolong this further. TCP attempts ordinarily fail earlier through
the OS. The measured local starvation and real SYN_SENT samples demonstrate
both the accounting problem and why faster replenishment is necessary.

### 2. Random selection has no memory of success or failure — confirmed mechanism

`connect_queue_cb()` chooses uniformly among eligible addresses.
`ntb_network_addr` stores only `last_connect_time`, not last handshake success,
failure count, or differentiated backoff. `can_connect_to_addr()` applies a flat
60-second retry interval measured from attempt start. A TCP timeout longer than
60 seconds therefore permits immediate reuse of the same failed candidate.

At the measured 5/328 handshake yield, the simple independent-draw expectation
for an initial batch of eight is only 0.122 successes. This is an illustration,
not a time-to-connect model: real draws, discovery, failures and retries are
correlated. The existing peer count of eight cannot overcome this candidate mix
without faster exploration and successful-peer preference.

### 3. Useful history expires after two hours — confirmed by executable probe

`add_addr()` at `ntb-network.c:663` rejects timestamps aged at least two hours.
`store_for_each_addr_cb()` feeds persisted entries through this same function.
`save_addr_list_cb()` also filters by advertisement age, and `gc_addrs()` deletes
old unconnected discovered addresses. Gossip eligibility, dial eligibility,
and persistence retention are incorrectly coupled to one constant.

The cache stores remote advertisement time, not locally verified success.
An active long-lived connection does not periodically refresh durable success
history. Restarting after a break can lose useful candidates and return to DNS.
The fresh/7,500-second cache probe changes only timestamps and changes the
result from five connections to zero.

### 4. Explicit protocol rejections are silently ignored — confirmed

`ntb-connection.c:425` has no `error` handler; unknown commands return success.
Nine live responses supplied rejection reasons, but the production engine cannot
report these or immediately free the peer on a fatal rejection. This is especially
costly with its long establishment timeout. Do not interpret the remote text as
proof another local app is running: server policy, saturation and shared-IP
conditions can produce similar responses.

### 5. Qt timeout cleanup crashes — reproduced; exact ownership fix needs validation

The actual `--qt-node` binary exited with SIGSEGV in a second run at about 33 s.
The first run's macOS crash report identified `bm::(anonymous namespace)::Relay::tick()`
as the top frame. `src/portable_relay.cpp:241` iterates `peers_`; `abort()` emits
signals whose handlers at lines 205–206 remove entries from that same list.
Iterator invalidation is the leading mechanism consistent with the trigger and
stack, rather than a separately instrumented memory-sanitizer proof.

The Qt backend already has a 30-second setup timeout, but shares an eight-entry
budget and dials only one peer per second. It persists no outcome information.
It does not proactively ping idle peers. Treat Qt parity as a separate task,
with the crash fixed before relying on timeout-based recovery.

### Retention follow-up, not a proven cause of this live shortage

notbit's idle timer sends unsolicited `pong`, rather than `ping`. A quiet peer
that waits for ping and ignores unsolicited pong can stay connected and healthy
yet send no data before notbit's ten-minute read timeout. PyBitmessage normally
sends its own ping, so this does not establish that PyBitmessage peers are being
dropped in practice. Add a quiet-peer ping/pong regression and a 30-minute public
soak before claiming long-term retention is solved.

## Comparison with PyBitmessage

The official implementation separates established and pending counts, normally
allows up to 64 pending attempts (four through a proxy), and closes incomplete
connections after approximately 20 seconds without transmission progress.
See pinned [connectionpool.py](https://github.com/Bitmessage/PyBitmessage/blob/dcbcc4a2fd74a9c7119fa48c5a54457a6ef8887a/src/network/connectionpool.py)
and [helper_startup.py](https://github.com/Bitmessage/PyBitmessage/blob/dcbcc4a2fd74a9c7119fa48c5a54457a6ef8887a/src/helper_startup.py).

It ranks peers by past outcomes and keeps history substantially longer than the
advertisement window. See [connectionchooser.py](https://github.com/Bitmessage/PyBitmessage/blob/dcbcc4a2fd74a9c7119fa48c5a54457a6ef8887a/src/network/connectionchooser.py)
and [knownnodes.py](https://github.com/Bitmessage/PyBitmessage/blob/dcbcc4a2fd74a9c7119fa48c5a54457a6ef8887a/src/network/knownnodes.py).

The previous fix, `8a30a32`, described active `getaddr` requests as a PyBitmessage
discovery mechanism. The checked reference has no `bm_command_getaddr` handler;
it sends `addr` automatically after establishment. Its specialized bootstrap
connections close after receiving addresses. Thus the existing test proving
ynotbit **sends** `getaddr` does not prove improved interoperability or discovery.
See [tcp.py](https://github.com/Bitmessage/PyBitmessage/blob/dcbcc4a2fd74a9c7119fa48c5a54457a6ef8887a/src/network/tcp.py)
and [bmproto.py](https://github.com/Bitmessage/PyBitmessage/blob/dcbcc4a2fd74a9c7119fa48c5a54457a6ef8887a/src/network/bmproto.py).

## Repeat the measurements

Run from the repository root; the binary path below is this macOS build.
The loopback probes do not contact the public network or change the desktop's
store. They report observed behavior rather than asserting that the current
broken behavior should remain.

```sh
python3 scripts/peer_probe.py loopback --binary "$PWD/build/ynotbit.app/Contents/MacOS/ynotbit" --output /tmp/peer-starvation.json
python3 scripts/peer_probe.py loopback --binary "$PWD/build/ynotbit.app/Contents/MacOS/ynotbit" --reject --duration 20 --output /tmp/peer-rejection.json
python3 scripts/peer_cache_probe.py --binary "$PWD/build/ynotbit.app/Contents/MacOS/ynotbit" --output /tmp/peer-cache.json
python3 scripts/peer_probe.py loopback --binary "$PWD/build/ynotbit.app/Contents/MacOS/ynotbit" --backend=--qt-node --output /tmp/peer-qt.json
```

The public probe sends only version/verack/getaddr and pong replies; it neither
requests message objects nor advertises invented endpoints. Public endpoints
are taken from DNS and an optional snapshot of the user's cache, with at most
400 endpoint attempts and 12 concurrent sockets by default.

```sh
cp "$HOME/Library/Application Support/NotbitDesktop/Notbit Desktop/node/addr-list.txt" /tmp/peer-addresses.txt
python3 scripts/peer_probe.py public --addresses /tmp/peer-addresses.txt --output /tmp/peer-survey.json
python3 scripts/peer_probe.py control --binary "$PWD/build/ynotbit.app/Contents/MacOS/ynotbit" --survey /tmp/peer-survey.json --duration 180 --output /tmp/peer-control.json
```

`control` launches the real relay against successful surveyed endpoints in a
temporary empty directory. Unlike the lightweight public survey, it receives
normal public objects. Run it after the survey completes. It does not establish
that a peer from an old survey is still available, nor that the automatic
scheduler has been fixed.

## Evidence files

- `running-app.json`: 37 status/socket-count samples; local addresses omitted.
- `public-survey.json`: all 328 endpoint results; gossip bodies reduced to counts.
  In this first survey, normal completion of the ten-second hold is recorded as
  `TimeoutError`; use handshake/elapsed fields to distinguish it from setup
  failure. The reusable probe now labels it `hold_complete`.
- `public-control.json`: all 37 control samples and relay log.
- `public-control-repeat.json`: later ten-second command check, reaching three
  handshakes; reasons for the two incomplete setups were not established.
- `loopback-starvation.json`, `loopback-rejection.json`: observations and logs.
- `cache-age.json`: the timestamp-only A/B experiment.
- `qt-timeout-crash.json`: repeat-run samples and process return code -11.

An initial discarded local test tried advertising 127.0.0.1 peers through `addr`;
notbit rejects loopback gossip even with `-L`. The retained starvation probe uses
13 explicit loopback endpoints, so this address filter cannot explain its result.
