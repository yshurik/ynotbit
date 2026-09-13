# Portable notbit desktop — approved design

C++/Qt Quick desktop application, libsodium Argon2id authenticated vault,
SQLCipher mailbox documents, and a notbit-derived network node.

Vault files contain identity signing/encryption secrets and independent random
mailbox keys. No plaintext keys.dat or maildir is created. A locked vault has
no retained application decryption keys. The relay continues receiving,
validating and caching objects without access to the vault. Unlock scans cached
objects, commits decrypted messages with a per-mailbox checkpoint, and deduplicates
by object inventory hash. Adding identities resets the scan checkpoint. Local
retention is independent of protocol relay expiry and bounded by disk usage.

Mailbox files contain private message text, identity labels, drafts and scan
state. A wrong vault must never create or overwrite a mailbox. Password changes
atomically replace the encrypted vault. Backups copy a closed mailbox and vault.

UI uses a conversation list and a reading pane, explicit lock and node states,
file-oriented create/open flows, and truthful delivery state. Chans are shared-key
identities; subscription broadcasts are a separate protocol feature.

Delivery targets: macOS app, Linux AppImage, standalone Windows exe. Portability
must be demonstrated by builds, not inferred from Qt support. Protocol changes
require compatibility fixtures and peer checks before release claims.
