// The fixtures the storage tests read: files written by the previous release's Qt code, kept in
// tests/golden, plus the fixed password and mail/key that made them.
#pragma once
#include <bmc/bytes.h>
#include <bmc/secret.h>
#include <bmc/test.h>
#include <fstream>
#include <string>

namespace fixtures {
inline bmc::Bytes load(const char *name) {
    std::ifstream in(std::string(BMC_GOLDEN_DIR) + "/" + name, std::ios::binary);
    if (!in)
        bmc::test::fail(__FILE__, __LINE__, std::string("cannot read ") + name);
    bmc::Bytes out;
    char c;
    while (in.get(c))
        out.push_back(uint8_t(c));
    return out;
}
inline bmc::Bytes goldenVault1() {
    return load("vault-v1.bmvault");
}
inline bmc::Bytes goldenVault2() {
    return load("vault-v2.bmvault");
}
inline bmc::Bytes goldenVaultOldLabel() {
    return load("vault-v2-old-label.bmvault");
}
inline const char *goldenPassword() {
    return "slice2-golden";
}
inline bmc::Bytes goldenMailbox1() {
    return load("mailbox-v1.bmmail");
}
inline bmc::Bytes goldenMailbox2() {
    return load("mailbox-v2.bmmail");
}
// The fixed 32 bytes 0x00..0x1f the golden mailbox generator keyed both files with.
inline bmc::Secret goldenMailboxKey() {
    bmc::Secret key(32);
    for (int i = 0; i < 32; ++i)
        key.data()[i] = uint8_t(i);
    return key;
}
} // namespace fixtures
