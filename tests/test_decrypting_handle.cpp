// tests/test_decrypting_handle.cpp
// DecryptingFileHandle: bajty v rozsahu blobu se desifruji keystreamem,
// bajty mimo blob (hlavicka/index) projdou beze zmeny (pass-through).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "io/file_handle.h"
#include "util/ithaca_crypto.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ithaca;

namespace {
struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { if (!path.empty()) std::remove(path.c_str()); }
};
} // namespace

TEST_CASE("DecryptingFileHandle desifruje blob, mimo blob pass-through") {
    const uint64_t blob_off = 100;
    const uint64_t blob_size = 300;
    uint8_t key[32];   std::memset(key, 0x11, 32);
    uint8_t nonce[32]; std::memset(nonce, 0x22, 32);

    std::vector<uint8_t> plain(400);
    for (size_t i = 0; i < plain.size(); ++i) plain[i] = (uint8_t)(i & 0xFF);
    std::vector<uint8_t> disk = plain;
    keystreamXor(key, nonce, /*p0=*/0, disk.data() + blob_off, (size_t)blob_size);

    std::string p = "/tmp/ithaca_dec_handle.bin";
    TempFile guard{p};
    { std::FILE* f = std::fopen(p.c_str(), "wb");
      REQUIRE(f); std::fwrite(disk.data(), 1, disk.size(), f); std::fclose(f); }

    auto raw = openFileHandle(p);
    REQUIRE(raw != nullptr);
    auto dec = makeDecryptingFileHandle(raw, key, nonce, blob_off, blob_size);
    REQUIRE(dec != nullptr);

    // Cteni hlavicky (mimo blob) → plaintext.
    uint8_t hb[16];
    REQUIRE(dec->readAt(0, hb, 16));
    for (int i = 0; i < 16; ++i) CHECK(hb[i] == (uint8_t)(i & 0xFF));

    // Cteni vyrezu blobu [150,150+80) → desifrovano == puvodni plain.
    uint8_t bb[80];
    REQUIRE(dec->readAt(150, bb, 80));
    for (int i = 0; i < 80; ++i) CHECK(bb[i] == plain[150 + i]);

    // Cteni pres hranici blobu [90,90+30) → cast plaintext, cast desifrovana.
    uint8_t cb[30];
    REQUIRE(dec->readAt(90, cb, 30));
    for (int i = 0; i < 30; ++i) CHECK(cb[i] == plain[90 + i]);
}
