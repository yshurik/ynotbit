#include <bmc/hash.h>
#include <openssl/evp.h>

namespace bmc {
namespace {
const EVP_MD *algorithm(Digest d) {
    switch (d) {
    case Digest::Sha1:
        return EVP_sha1();
    case Digest::Sha256:
        return EVP_sha256();
    case Digest::Sha512:
        return EVP_sha512();
    }
    return nullptr;
}
} // namespace

struct Hasher::Impl {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    ~Impl() {
        EVP_MD_CTX_free(ctx);
    }
};

Hasher::Hasher(Digest d) : p_(new Impl) {
    if (!p_->ctx || EVP_DigestInit_ex(p_->ctx, algorithm(d), nullptr) != 1)
        throw Error(Code::Internal, "Digest failed");
}
Hasher::~Hasher() = default;

void Hasher::add(ByteView data) {
    if (EVP_DigestUpdate(p_->ctx, data.data(), data.size()) != 1)
        throw Error(Code::Internal, "Digest failed");
}

Bytes Hasher::result() {
    Bytes out(EVP_MAX_MD_SIZE);
    unsigned size = 0;
    if (EVP_DigestFinal_ex(p_->ctx, out.data(), &size) != 1)
        throw Error(Code::Internal, "Digest failed");
    out.resize(size);
    return out;
}

Bytes hash(Digest d, ByteView data) {
    Hasher h(d);
    h.add(data);
    return h.result();
}
} // namespace bmc
