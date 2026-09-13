# Complete Messaging Implementation Plan

Goal: make the existing app usable for sending, receiving and managing direct
messages and chans, and support broadcasts, while preserving existing documents.

Architecture: encrypted mailbox owns durable drafts, public-key cache, outbox,
subscriptions and delivery events. Vault remains the only owner of identity keys.
A cancellable worker calculates proof of work on already encrypted objects.
Only complete network objects cross into the keyless relay; control and receipt
files allow crash-safe publication confirmation. UI exposes actual delivery state.

Spec: existing docs/design.md plus the user's approval to implement the entire
sending flow and logical usability gaps. No additional approval needed for local
implementation, tests and packages. Public-network test messages are not authorized;
use loopback peers and generated test identities.

Global constraints: retain existing vault compatibility, migrate mailbox v1 in a
transaction, keep network running while locked, wipe key access and pause dependent
work on lock, store drafts/outbox/ack tokens only inside SQLCipher, distinguish
publication from acknowledgment and from reading. Never claim a platform package
works without building/testing it.

Tasks:
- [x] Wire codecs: public-only recipient encryption, pubkeys versions2–4,
  authenticated ack frames, broadcast v4/v5, adversarial parsing tests.
- [x] Encrypted persistence: migrate v1, edit/upsert drafts, outbox rows and event
  history, recipient public keys, subscriptions, archive/trash/restore and search.
- [x] Relay publication: bounded file-based ciphertext-only command queue, atomically
  recorded acceptance/rejection, connected-peer status, restart-safe reannouncements.
- [x] Delivery controller: key discovery/request/response, ack POW then message POW,
  durable recovery/retries/cancel, incoming ack responses, publication status,
  broadcast subscriptions and outgoing broadcasts, reply and self-send cases.
- [ ] Desktop flows: edit/send existing draft, sender selector, Outbox/Sent,
  state/error/retry/cancel controls, reply, identity naming, subscriptions, safe
  unsaved compose behavior, mailbox close/switch/create and recent vault restore,
  settings for relay/retention/CPU, visible peer state and failures.
- [ ] End-to-end: generated two-party loopback delivery with unknown public key,
  acknowledgment, locked reception/restart, v1 user draft migration, chan and
  broadcast behavior, desktop interactions, all regression tests.
- [ ] Build/package updated app and source, native portability where supported,
  document remaining external signing/platform constraints accurately.

A task completes only when its externally observable behavior is tested. Review
wire/persistence/security boundaries and the final integrated change; fix important
findings before delivery. Keep progress in work/implementation-v02/ledger.md.

## 2026-09-13 status

Repository published as https://github.com/yshurik/ynotbit; executable/bundle/UI
renamed ynotbit. Desktop sending, draft editing/autosave, reply, folder actions,
subscriptions, peer/proxy/retention settings, and node restart are implemented.
Proof of work currently uses one CPU thread. Nine local suites pass; full
controller delivery and real relay transport are tested separately. Native
Windows relay and Linux AppImage validation remain open, as do CPU parallelism
and a single test combining two controllers with two actual network relays.
CI template is preserved in docs/ci because the GitHub token lacks workflow scope.
