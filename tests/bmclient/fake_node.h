// A fake node and its relay: a directory with an objects.sqlite, written the way the node writes
// it, and the relay's work -- an object one client spooled is offered to the other side's store,
// and the receipt written, exactly as tests/delivery_tests.cpp's transfer() did with Qt.
#pragma once
#include <bmc/bytes.h>
#include <bmc/path.h>
#include <bmc/pow.h>
#include <bmc/protocol.h>
#include <bmc/test.h>
#include <bmc/wire.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ntb-object-db.h>
#include <string>

namespace fake {
// Writes an object the way the node does: through its own store.
inline void cacheObject(const std::string &root, bmc::ByteView payload) {
    std::filesystem::create_directories(bmc::path(root));
    auto *db = ntb_object_db_open(root.c_str());
    CHECK(db);
    const auto hash = bmc::unhex(bmc::Protocol::inventoryHash(payload));
    const bool saved =
        ntb_object_db_save(db, hash.data(), payload.data(), payload.size(), 1'700'000'000);
    ntb_object_db_close(db);
    CHECK(saved);
}

// Offers every spooled object of `from` to `to`, writes the receipt the relay writes, and removes
// the spool file. Returns how many objects travelled.
inline int deliver(const std::string &from, const std::string &to, int64_t now) {
    const auto spool = bmc::path(from + "/publish");
    if (!std::filesystem::is_directory(spool))
        return 0;
    int count = 0;
    std::error_code ignored;
    for (const auto &entry : std::filesystem::directory_iterator(spool)) {
        if (entry.path().extension() != ".object")
            continue;
        const auto object = bmc::test::readFile(entry.path().string());
        CHECK(bmc::ProofOfWork::valid(object, now));
        cacheObject(to, object);
        cacheObject(from, object);
        std::filesystem::create_directories(bmc::path(from + "/receipts"));
        const auto receipt = from + "/receipts/" + entry.path().stem().string() + ".json";
        const auto text = std::string("{\"state\":\"offered\",\"hash\":\"") +
                          bmc::Protocol::inventoryHash(object) + "\"}";
        std::ofstream out(receipt, std::ios::binary);
        out.write(text.data(), std::streamsize(text.size()));
        out.close();
        std::filesystem::remove(entry.path(), ignored);
        ++count;
    }
    return count;
}
} // namespace fake
