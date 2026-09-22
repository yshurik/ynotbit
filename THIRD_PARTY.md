# Third-party components

- notbit: https://github.com/bpeel/notbit, commit
  `7f50ab3dcc3344c7fc9c4740cd50924a9d128c6f`. License is preserved in
  `third_party/notbit/COPYING` and the source headers. Modifications disable
  keyring/IPC creation in the desktop relay, disable Maildir creation, retain
  expired local objects without reintroducing them into network inventory, bound
  EC coordinate parsing, compare ECIES MACs in constant time, clear selected
  cryptographic work buffers, and port `ntb-netaddress.c`/`.h` to Winsock2 and
  guard the POSIX-only thread/fd helpers in `ntb-util.c`/`.h` for the Windows
  build (both unused by the notbit_engine relay, which stays Unix-only).
  `ntb-network.c` also drops its hard-coded 2014 default-node list (verified
  dead, 0/10 reachable by direct TCP connect, and actively slowed discovery
  of real peers since random candidate selection kept picking them over live
  DNS-bootstrap addresses) and now sends `getaddr` to a peer once connected
  instead of only waiting on unsolicited `addr` gossip, closing most of the
  connected-peer-count gap observed against PyBitmessage.
- The user's Docker integration https://github.com/yshurik/docker-bitmessage and
  its `yshurik/notbit` smtp branch were inspected as prior art. The new desktop
  does not embed SMTP or Dovecot and does not copy their container-specific patch.
- Qt 6.8.3: https://www.qt.io, dynamically linked desktop libraries and QML
  modules. Qt's license terms apply to those components; obtain the corresponding
  source from https://download.qt.io/archive/qt/6.8/6.8.3/single/ . The source
  project and build instructions allow rebuilding/relinking this application.
- libsodium 1.0.20: https://github.com/jedisct1/libsodium, ISC license, commit
  `9511c982fb1d046470a8b42aa36556cdb7da15de`.
- SQLCipher 4.6.1: https://github.com/sqlcipher/sqlcipher, BSD-style license,
  commit `c5bd336ece77922433aaf6d6fe8cf203b0c299d5`.
- OpenSSL 3.6.1: https://www.openssl.org, Apache License 2.0.

- zlib 1.3.2: https://zlib.net, zlib license (bundled transitive dependency).

Dependency license texts are included under `licenses/`.

The ynotbit 0.2 relay also adds a ciphertext-only publication bridge, a bounded
one-second event-loop wakeup, connected-peer status, and support for either
version/verack handshake order. The expanded Qt/OpenSSL wire implementation is
original code. Its independent fixture generator follows the documented wire
layout and PyBitmessage v0.6 worker behavior; it does not vendor PyBitmessage code.
