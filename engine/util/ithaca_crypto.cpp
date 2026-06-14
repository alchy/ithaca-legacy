// engine/util/ithaca_crypto.cpp — viz ithaca_crypto.h.
#include "util/ithaca_crypto.h"

#include "util/sha256.h"

#include <cstring>
#include <vector>

namespace ithaca {

namespace {
constexpr size_t kBlock = 64;   // SHA-256 block size pro HMAC padding
} // namespace

std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t key_len,
                                   const void* msg, size_t msg_len) {
    uint8_t k[kBlock];
    std::memset(k, 0, kBlock);
    if (key_len > kBlock) {
        auto kh = Sha256::hash(key, key_len);
        std::memcpy(k, kh.data(), 32);
    } else {
        std::memcpy(k, key, key_len);
    }
    uint8_t ipad[kBlock], opad[kBlock];
    for (size_t i = 0; i < kBlock; ++i) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    Sha256 inner;
    inner.update(ipad, kBlock);
    inner.update(msg, msg_len);
    auto ih = inner.finish();
    Sha256 outer;
    outer.update(opad, kBlock);
    outer.update(ih.data(), ih.size());
    return outer.finish();
}

std::array<uint8_t, 32> deriveKey(const uint8_t secret[32], const char* domain,
                                  const uint8_t* msg, size_t msg_len) {
    const size_t dlen = std::strlen(domain);
    std::vector<uint8_t> m(dlen + msg_len);
    std::memcpy(m.data(), domain, dlen);
    if (msg_len) std::memcpy(m.data() + dlen, msg, msg_len);
    return hmacSha256(secret, 32, m.data(), m.size());
}

void keystreamXor(const uint8_t key[32], const uint8_t nonce[32],
                  uint64_t p0, uint8_t* buf, size_t n) {
    if (n == 0) return;
    uint64_t p = p0;
    size_t done = 0;
    while (done < n) {
        const uint64_t block_i = p / 32;
        uint8_t m[40];
        std::memcpy(m, nonce, 32);
        for (int b = 0; b < 8; ++b) m[32 + b] = (uint8_t)(block_i >> (8 * b));
        auto ks = hmacSha256(key, 32, m, sizeof(m));
        size_t off_in_block = (size_t)(p % 32);
        while (off_in_block < 32 && done < n) {
            buf[done] = (uint8_t)(buf[done] ^ ks[off_in_block]);
            ++off_in_block; ++done; ++p;
        }
    }
}

} // namespace ithaca
