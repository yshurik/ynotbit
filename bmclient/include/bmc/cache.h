// The node's object store (objects.sqlite), read in std. Only the node writes it; the desktop app
// and any bot read the same file with their own connection, which is why the file name, the
// version and the layout live in third_party/notbit/src/ntb-object-db.h.
#pragma once
#include <bmc/bytes.h>
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
namespace bmc {
class Mailbox;
class Vault;

struct CachedObject {
    int64_t sequence = 0;
    std::string hash; // lowercase hex, as Protocol::inventoryHash() spells it
    Bytes payload;
};

// Reads the node's object store (objects.sqlite). Only the node writes it.
class Cache {
    sqlite3 *db_ = nullptr;
    std::string root_;
    bool ready();

  public:
    explicit Cache(std::string root);
    ~Cache();
    Cache(const Cache &) = delete;
    Cache &operator=(const Cache &) = delete;
    std::vector<CachedObject> after(int64_t sequence, int limit = 32);
    bool hasAfter(int64_t sequence);
    int64_t firstSequence(); // 0 when nothing is stored
    std::string id();        // empty until the node has created its store
};

// The older, direct-message-only scan of the store. Delivery::scan() is the one the app and the
// bots run; this stays because the Qt suite (tests/lifecycle_tests.cpp) pins it.
int scanMailbox(Cache &cache, Mailbox &mailbox, const Vault &vault, int limit = 8);
} // namespace bmc
