// tests/test_ithaca_bank.cpp
// openIthacaBank: validace hlavicky, hash indexu, rozsahu zaznamu.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "ithaca_test_blob.h"
#include "sample/ithaca_bank.h"

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
