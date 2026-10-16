#include <bmc/bytes.h>
#include <bmc/random.h>
#include <sodium.h>

namespace bmc {
void initCrypto() {
    static const bool started = sodium_init() >= 0;
    if (!started)
        throw Error(Code::Internal, "Cannot initialize cryptography");
}

void randomBytes(void *out, size_t size) {
    initCrypto();
    randombytes_buf(out, size);
}

std::string uuid() {
    uint8_t b[16];
    randomBytes(b, sizeof b);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    const std::string digits = hex(ByteView(b, sizeof b));
    return digits.substr(0, 8) + "-" + digits.substr(8, 4) + "-" + digits.substr(12, 4) + "-" +
           digits.substr(16, 4) + "-" + digits.substr(20, 12);
}
} // namespace bmc
