// tests/test_packed_bank_load.cpp
// loadBank nad pakovanou bankou: kostra z indexu, baked RMS/attack
// autoritativni, poradi slotu = poradi indexu (predrazeno), preload heads
// ctene z blobu, mode FullyLoaded/Streamed dle runtime preload_ms.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "ithaca_test_blob.h"
#include "io/wav_writer.h"   // writeWavStereo16 (adresarova fixture)
#include "sample/sample_store.h"

using namespace ithaca;
using namespace ithaca_test;

TEST_CASE("loadBank packed: sloty, baked hodnoty, preload z blobu") {
    // 2 vrstvy noty 60 (tissi pred hlasitejsi — jak je radi bake) + nota 72.
    // 4096 frames @48k: pri preload_ms=150 je preload_frames=7200 →
    // frames <= 2*7200 → FullyLoaded (cely sampl v RAM).
    BuiltBlob b = buildTestIthaca("load_basic", {
        {60, 4096, 48000, -30.f, 100},
        {60, 4096, 48000, -20.f, 120},
        {72, 4096, 48000, -25.f, 90},
    });
    auto& L = log::Logger::default_();
    Bank bank = loadBank(b.dir, L);
    CHECK(bank.format == BankFormat::PackedIthaca);
    CHECK(bank.loaded_samples == 3);
    REQUIRE(bank.notes[60].recorded);
    REQUIRE(bank.notes[60].slots.size() == 2u);
    REQUIRE(bank.notes[72].slots.size() == 1u);
    // Baked RMS autoritativni (zadne mereni!) a poradi dle indexu.
    CHECK(bank.notes[60].slots[0].rms_db == doctest::Approx(-30.f));
    CHECK(bank.notes[60].slots[1].rms_db == doctest::Approx(-20.f));
    CHECK(bank.notes[60].slots[0].variants[0].attack_end_frame == 100);
    // Preload data bit-exact z blobu.
    const MicLayer& m = bank.notes[60].slots[0].variants[0].mics[0];
    CHECK(m.mode == MicLayerMode::FullyLoaded);
    REQUIRE(m.head_frames == 4096);
    for (size_t i = 0; i < m.preload_head.size(); ++i)
        REQUIRE(m.preload_head[i] == b.expected_samples[0][i]);
    // Lokator pro streaming pripraveny.
    CHECK(m.file.blob != nullptr);
    CHECK(m.file.sample_rate == 48000);
    removeBlob(b);
}

TEST_CASE("loadBank packed: dlouhy sampl je Streamed s baked resonance startem") {
    // 48000 frames @48k: preload_frames=7200, 48000 > 14400 → Streamed.
    BuiltBlob b = buildTestIthaca("load_stream", {{60, 48000, 48000, -25.f, 500}});
    auto& L = log::Logger::default_();
    Bank bank = loadBank(b.dir, L);
    REQUIRE(bank.notes[60].slots.size() == 1u);
    const MicLayer& m = bank.notes[60].slots[0].variants[0].mics[0];
    CHECK(m.mode == MicLayerMode::Streamed);
    CHECK(m.head_frames == 7200);
    CHECK(m.resonance_start_frame == 500);   // = baked attack_end
    removeBlob(b);
}

TEST_CASE("loadBank packed: midi filtr") {
    BuiltBlob b = buildTestIthaca("load_filter", {
        {60, 2048, 48000, -30.f, 10}, {72, 2048, 48000, -25.f, 10}});
    auto& L = log::Logger::default_();
    Bank bank = loadBank(b.dir, L, 0, /*midi_from=*/70, /*midi_to=*/80);
    CHECK_FALSE(bank.notes[60].recorded);
    CHECK(bank.notes[72].recorded);
    CHECK(bank.loaded_samples == 1);
    removeBlob(b);
}

TEST_CASE("loadBank packed: poskozeny soubor → prazdna banka, nepada") {
    BuiltBlob b = buildTestIthaca("load_corrupt", {{60, 2048, 48000, -30.f, 10}},
                                  /*corrupt_index_hash=*/true);
    auto& L = log::Logger::default_();
    Bank bank = loadBank(b.dir, L);
    CHECK(bank.format == BankFormat::PackedIthaca);
    CHECK(bank.loaded_samples == 0);
    removeBlob(b);
}

TEST_CASE("loadBank packed == loadBank adresar (bit-exact preload data)") {
    // Stejne 2 vrstvy noty 60 jako dynamic-velocity adresar vs. packed blob.
    // Poradi slotu v adresarove bance urcuje MERENA RMS (DSP detail — zde
    // nepredikujeme); sloty parujeme podle delky (4096 vs 2048 je
    // jednoznacne) a porovnavame preload data bit-exact.
    namespace fs = std::filesystem;
    BuiltBlob b = buildTestIthaca("load_equiv", {
        {60, 4096, 48000, -30.f, 0},
        {60, 2048, 48000, -20.f, 0},
    });
    std::string ddir = "/tmp/ithaca_equiv_dyn";
    fs::remove_all(ddir);
    fs::create_directories(ddir + "/m060");
    writeWavStereo16(ddir + "/m060/a.wav", rampSamples(4096), 48000);
    writeWavStereo16(ddir + "/m060/b.wav", rampSamples(2048), 48000);
    auto& L = log::Logger::default_();
    Bank pb = loadBank(b.dir, L);
    Bank db = loadBank(ddir, L);
    REQUIRE(pb.notes[60].slots.size() == 2u);
    REQUIRE(db.notes[60].slots.size() == 2u);
    for (int want = 0; want < 2; ++want) {
        const int frames = (want == 0) ? 4096 : 2048;
        const MicLayer* pm = nullptr; const MicLayer* dm = nullptr;
        for (const auto& s : pb.notes[60].slots)
            if (s.variants[0].mics[0].file.frames == frames)
                pm = &s.variants[0].mics[0];
        for (const auto& s : db.notes[60].slots)
            if (s.variants[0].mics[0].file.frames == frames)
                dm = &s.variants[0].mics[0];
        REQUIRE(pm != nullptr); REQUIRE(dm != nullptr);
        REQUIRE(pm->head_frames == dm->head_frames);
        REQUIRE(pm->preload_head.size() == dm->preload_head.size());
        for (size_t i = 0; i < pm->preload_head.size(); ++i)
            REQUIRE(pm->preload_head[i] == dm->preload_head[i]);   // bit-exact
    }
    fs::remove_all(ddir);
    removeBlob(b);
}
