#pragma once
// engine/util/ithaca_crypto.h
// Symetricka krypto pro pakovanou banku v2 — postavena VYHRADNE nad nasi
// Sha256 (zadna externi zavislost). HMAC-SHA256 (RFC 2104), KDF a CTR
// keystream. Python zrcadlo: tools/ithaca_crypto.py (stejne vektory v testech).

#include <array>
#include <cstddef>
#include <cstdint>

namespace ithaca {

// HMAC-SHA256 (RFC 2104). key_len libovolne; pro nase 32B klice trivialni.
std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t key_len,
                                   const void* msg, size_t msg_len);

// Odvozeni klice: HMAC(secret32, domain || msg). domain je C-string (bez \0).
std::array<uint8_t, 32> deriveKey(const uint8_t secret[32], const char* domain,
                                  const uint8_t* msg, size_t msg_len);

// CTR keystream XOR in-place. p0 = blob-relativni pozice prvniho bajtu v `buf`.
// keystream_block(i) = HMAC(key, nonce32 || u64_le(i)); blok i = p/32.
// Dvoji volani se stejnymi parametry vrati original (involuce).
void keystreamXor(const uint8_t key[32], const uint8_t nonce[32],
                  uint64_t p0, uint8_t* buf, size_t n);

} // namespace ithaca
