#pragma once
#include <cstddef>
#include <string>

namespace bmc {
// Starts libsodium; throws Error(Internal) if it cannot. Every user of it calls this first.
void initCrypto();
void randomBytes(void *out, size_t size);
// A random (version 4) UUID, lower case, 8-4-4-4-12.
std::string uuid();
} // namespace bmc
