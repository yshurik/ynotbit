# Third-party components

- notbit: https://github.com/bpeel/notbit, commit
  `7f50ab3dcc3344c7fc9c4740cd50924a9d128c6f`. License is preserved in
  `third_party/notbit/COPYING` and the source headers. Modifications disable
  keyring/IPC creation in the desktop relay, disable Maildir creation, retain
  expired local objects without reintroducing them into network inventory, bound
  EC coordinate parsing, compare ECIES MACs in constant time, and clear selected
  cryptographic work buffers.
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
