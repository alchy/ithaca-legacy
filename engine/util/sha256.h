#pragma once
// engine/util/sha256.h
// Minimalni SHA-256 (FIPS 180-4) pro integritu .ithaca banky. Zadne externi
// zavislosti; streaming update/finish (hash indexu se pocita po sekcich).

#include <array>
#include <cstddef>
#include <cstdint>

namespace ithaca {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    std::array<uint8_t, 32> finish();   // po finish() nutny reset()

    static std::array<uint8_t, 32> hash(const void* data, size_t len) {
        Sha256 s; s.update(data, len); return s.finish();
    }

private:
    void processBlock(const uint8_t* p);
    uint32_t h_[8];
    uint64_t total_   = 0;   // bajty celkem
    uint8_t  buf_[64];
    size_t   buf_len_ = 0;
};

} // namespace ithaca
