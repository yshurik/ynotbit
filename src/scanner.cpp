#include "scanner.h"
namespace bm {
// The direct-message-only scan bmc::scanMailbox does, through the Qt-typed wrappers.
int scanMailbox(Cache &cache, Mailbox &mailbox, const Vault &vault, int limit) {
    return bmc::scanMailbox(cache.core(), mailbox.core(), vault.core(), limit);
}
} // namespace bm
