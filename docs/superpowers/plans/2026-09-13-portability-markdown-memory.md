# Portable desktop, Markdown, and memory implementation

The user has authorized implementation on macOS, Windows, and Linux, system-aware
dark/light themes, Markdown as the message format, a visual Markdown editor, and
investigation/fixes for suspected excessive memory use. The supplied channel
screenshot adds bounded title/list layout and readable message typography.

## Design

Keep the Qt desktop and existing encrypted document/protocol formats. Render and
edit Markdown through QTextDocument; do not add a browser engine. Themes follow
the system by default and can be overridden. Incoming text cannot load remote or
local images without an explicit future attachment feature. Preserve received
message content while bounding its display.

Keep the existing Unix notbit relay on macOS/Linux. Add a native Qt Network relay
for Windows, testable on every host with `--qt-node`, sharing the existing verified
wire codec, proof-of-work validation, cache layout, and ciphertext publication
queue. Use bounded peer buffers, request queues, and disk-backed inventory. Build
and test each operating system in its own native CI runner; never equate a GUI-only
Windows compilation with a functioning messaging client.

Investigate GUI memory using process summaries and generated test mailboxes; do
not inspect private mailbox contents or dump an unlocked user's process memory.
Fix demonstrated mechanisms, distinguish leaks from transient allocation peaks,
and test idle/update behavior with a populated synthetic mailbox.

## Deliverables and checks

- [ ] Stable mailbox model notifications and bounded background discovery;
  regression test showing no full-mailbox reload on unchanged timer ticks.
- [ ] System/Light/Dark appearance class, visual Markdown editor and safe reader;
  formatting/roundtrip/undo/theme tests, screenshot inspection in both themes.
- [ ] Constrained list previews and reader headings for oversized/multiline chan
  subjects, with full original text still accessible.
- [ ] Native Qt relay: valid handshakes in both orders, bounded framing, object
  requests/persistence/publication, disconnected waiting state, rejection,
  reconnect, SOCKS5, and startup inventory recovery. Run existing loopback tests
  against both relay implementations plus malformed/oversized-input tests.
- [ ] Windows codec/build compatibility; macOS .app, Linux AppImage, and Windows
  portable bundle with a single executable distribution wrapper where feasible.
- [ ] Native OS build/test workflow and reproducible packaging scripts. Workflow
  activation needs GitHub `workflow` authorization, absent from the current token.
- [ ] Run focused tests, complete regression suite, memory measurements, inspect
  packaged launch/signature/runtime dependencies, push source and publish only
  artifacts actually produced and verified for their stated platforms.

Existing vault/mailbox compatibility, locked keyless reception, and queued-send
recovery remain mandatory. No test messages go to the public Bitmessage network.
