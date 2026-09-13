#pragma once
#include "cache.h"
#include "storage.h"
namespace bm {
int scanMailbox(Cache &cache, Mailbox &mailbox, const Vault &vault, int limit = 8);
}
