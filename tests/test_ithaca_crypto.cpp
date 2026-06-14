// tests/test_ithaca_crypto.cpp
// HMAC-SHA256 proti RFC 4231 + keystream determinismus. Parita s pythonem
// (tools/ithaca_crypto.py) je zajistena shodnymi vektory v obou jazycich.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "util/ithaca_crypto.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ithaca;

namespace {
std::string hex(const std::array<uint8_t, 32>& d) {
    char o[65];
    for (int i = 0; i < 32; ++i) std::snprintf(o + i * 2, 3, "%02x", d[i]);
    return std::string(o, 64);
}
} // namespace

TEST_CASE("hmacSha256 RFC 4231 test case 2") {
    // key="Jefe", data="what do ya want for nothing?"
    auto m = hmacSha256((const uint8_t*)"Jefe", 4,
                        "what do ya want for nothing?", 28);
    CHECK(hex(m) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("deriveKey = HMAC(secret, domain || msg)") {
    uint8_t secret[32]; std::memset(secret, 0x42, 32);
    std::string m = std::string("enc") + "LIC";
    auto ref = hmacSha256(secret, 32, m.data(), m.size());
    auto got = deriveKey(secret, "enc", (const uint8_t*)"LIC", 3);
    CHECK(hex(got) == hex(ref));
}

TEST_CASE("keystreamXor je involuce (XOR dvakrat = original) a offset-aware") {
    uint8_t key[32]; std::memset(key, 0x11, 32);
    uint8_t nonce[32]; std::memset(nonce, 0x22, 32);
    std::vector<uint8_t> data(200);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)(i * 7 + 3);
    std::vector<uint8_t> orig = data;
    keystreamXor(key, nonce, /*p0=*/0, data.data(), data.size());
    CHECK(data != orig);
    keystreamXor(key, nonce, /*p0=*/0, data.data(), data.size());
    CHECK(data == orig);

    std::vector<uint8_t> full = orig;
    keystreamXor(key, nonce, 0, full.data(), full.size());
    std::vector<uint8_t> part(orig.begin() + 50, orig.begin() + 110);
    keystreamXor(key, nonce, /*p0=*/50, part.data(), part.size());
    for (size_t i = 0; i < part.size(); ++i)
        CHECK(part[i] == full[50 + i]);
}
