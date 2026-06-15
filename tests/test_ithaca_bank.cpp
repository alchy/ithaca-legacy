// tests/test_ithaca_bank.cpp
// openIthacaBank: validace hlavicky, hash indexu, rozsahu zaznamu.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "ithaca_test_blob.h"
#include "io/sample_read.h"
#include "sample/ithaca_bank.h"

#include <cstring>
#include <fstream>

using namespace ithaca;
using namespace ithaca_test;

TEST_CASE("openIthacaBank valid blob") {
    BuiltBlob b = buildTestIthaca("open_ok", {
        {60, 2000, 48000, -30.f, 100},
        {60, 2000, 48000, -20.f, 120},   // hlasitejsi vrstva stejne noty
        {72, 1500, 48000, -25.f, 90},
    });
    IthacaBankFile f = openIthacaBank(b.ithaca_path);
    REQUIRE(f.ok);
    CHECK(f.entries.size() == 3u);
    CHECK(f.handle != nullptr);
    CHECK(f.entries[0].midi == 60);
    CHECK(f.entries[1].rms_db == doctest::Approx(-20.f));
    CHECK(f.entries[2].frames == 1500);
    removeBlob(b);
}

TEST_CASE("openIthacaBank odmitne spatny hash indexu") {
    BuiltBlob b = buildTestIthaca("bad_hash", {{60, 2000, 48000, -30.f, 100}},
                                  /*corrupt_index_hash=*/true);
    IthacaBankFile f = openIthacaBank(b.ithaca_path);
    CHECK_FALSE(f.ok);
    CHECK(f.error.find("hash") != std::string::npos);
    removeBlob(b);
}

TEST_CASE("openIthacaBank odmitne nepodporovanou verzi") {
    BuiltBlob b = buildTestIthaca("bad_ver", {{60, 2000, 48000, -30.f, 100}},
                                  false, /*force_version=*/99);
    IthacaBankFile f = openIthacaBank(b.ithaca_path);
    CHECK_FALSE(f.ok);
    removeBlob(b);
}

TEST_CASE("openIthacaBank odmitne nastavene flags (v1 neumi sifru/podpis)") {
    BuiltBlob b = buildTestIthaca("bad_flags", {{60, 2000, 48000, -30.f, 100}},
                                  false, kIthacaVersion, /*force_flags=*/1);
    IthacaBankFile f = openIthacaBank(b.ithaca_path);
    CHECK_FALSE(f.ok);
    removeBlob(b);
}

TEST_CASE("openIthacaBank odmitne neexistujici soubor") {
    IthacaBankFile f = openIthacaBank("/tmp/ithaca_no_such_dir/soundbank.ithaca");
    CHECK_FALSE(f.ok);
}

TEST_CASE("openIthacaBank odmitne zaznam mimo rozsah blobu (crafted, hash sedi)") {
    // Nafouknuty entry_size: hash indexu je platny (poskozeni je pred hashem),
    // ale per-zaznam range check musi chytit OOB → prazdna banka, zadny crash.
    BuiltBlob b = buildTestIthaca("oob_entry", {{60, 2000, 48000, -30.f, 100}},
                                  false, kIthacaVersion, 0,
                                  /*corrupt_entry_range=*/true);
    IthacaBankFile f = openIthacaBank(b.ithaca_path);
    CHECK_FALSE(f.ok);
    CHECK(f.error.find("blob") != std::string::npos);
    removeBlob(b);
}

TEST_CASE("openIthacaBank licensed happy path (spravny secret)") {
    uint8_t secret[32]; std::memset(secret, 0x07, 32);
    const char* lic = "{\"owner\":\"A\"}";
    BuiltBlob b = buildTestIthaca("lic_ok", {{60, 2000, 48000, -30.f, 100}},
                                  false, kIthacaVersion, 0, false, secret, lic);
    IthacaBankFile f = openIthacaBank(b.ithaca_path, secret);
    REQUIRE(f.ok);
    CHECK(f.entries.size() == 1u);
    REQUIRE(f.handle != nullptr);
    // Cteni pres desifrujici handle musi vratit puvodni (plaintext) vzorky:
    // overuje, ze openIthacaBank obalil handle spravnym klicem/nonce/offsetem.
    const IthacaEntry& e = f.entries[0];
    SampleFile sf;
    sf.path = b.ithaca_path; sf.blob = f.handle; sf.valid = true;
    sf.frames = (int)e.frames; sf.sample_rate = (int)e.sample_rate;
    sf.channels = e.channels; sf.sample_format = e.sample_format;
    sf.pcm_offset = e.entry_offset + e.pcm_data_offset;
    WavData w = readSampleRange(sf, 0, 64);
    REQUIRE(w.valid);
    CHECK(w.frames == 64);
    // Bit-exact proti ocekavanym (nesifrovanym) ramp datum z fixture.
    for (size_t i = 0; i < w.samples.size(); ++i)
        REQUIRE(w.samples[i] == b.expected_samples[0][i]);
    removeBlob(b);
}

TEST_CASE("openIthacaBank licensed: spatny secret → LicenseInvalid") {
    uint8_t secret[32]; std::memset(secret, 0x07, 32);
    uint8_t wrong[32];  std::memset(wrong, 0x08, 32);
    const char* lic = "{\"owner\":\"A\"}";
    BuiltBlob b = buildTestIthaca("lic_badsecret", {{60, 2000, 48000, -30.f, 100}},
                                  false, kIthacaVersion, 0, false, secret, lic);
    IthacaBankFile f = openIthacaBank(b.ithaca_path, wrong);
    CHECK_FALSE(f.ok);
    CHECK(f.license_invalid);
    removeBlob(b);
}

TEST_CASE("openIthacaBank licensed: editovana license → LicenseInvalid") {
    uint8_t secret[32]; std::memset(secret, 0x07, 32);
    const char* lic = "{\"owner\":\"A\"}";
    BuiltBlob b = buildTestIthaca("lic_edited", {{60, 2000, 48000, -30.f, 100}},
                                  false, kIthacaVersion, 0, false, secret, lic);
    { std::ofstream lf(b.dir + "/license.ithaca", std::ios::binary);
      lf << "{\"owner\":\"B\"}"; }
    IthacaBankFile f = openIthacaBank(b.ithaca_path, secret);
    CHECK_FALSE(f.ok);
    CHECK(f.license_invalid);
    removeBlob(b);
}

TEST_CASE("openIthacaBank licensed: chybi license → LicenseInvalid") {
    uint8_t secret[32]; std::memset(secret, 0x07, 32);
    const char* lic = "{\"owner\":\"A\"}";
    BuiltBlob b = buildTestIthaca("lic_missing", {{60, 2000, 48000, -30.f, 100}},
                                  false, kIthacaVersion, 0, false, secret, lic);
    std::remove((b.dir + "/license.ithaca").c_str());
    IthacaBankFile f = openIthacaBank(b.ithaca_path, secret);
    CHECK_FALSE(f.ok);
    CHECK(f.license_invalid);
    removeBlob(b);
}
