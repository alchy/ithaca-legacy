// tests/test_render_regression.cpp
// Bit-exact regrese pro refactor StreamedSampleReader (spec 2026-06-10 cast B):
// hash vystupu deterministicke sceny se NESMI zmenit. Pokryva RAM cesty
// (FullyLoaded hlasy, damp/retrigger/steal, pedal, rezonance cache-mode);
// streamovany ring kryji behavioralni testy (test_streamed_interp,
// test_stream_engine, test_long_sample_stream).
//
// POZN.: fixture je vazana na toolchain (poradi FP operaci) — pri zmene
// platformy/kompilatoru hashe preregeneruj: vynuluj kExpected, spust test,
// vypsane hodnoty vloz zpet.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine.h"
#include "io/wav_writer.h"
#include "util/log.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace ithaca;
namespace fs = std::filesystem;

namespace {
uint64_t fnv1a(const float* L, const float* R, int n, uint64_t h) {
    auto mix = [&](float v) {
        uint32_t b; std::memcpy(&b, &v, 4);
        h ^= b; h *= 1099511628211ull;
    };
    for (int i = 0; i < n; ++i) { mix(L[i]); mix(R[i]); }
    return h;
}
void writeRampWav(const std::string& p, int frames, float amp) {
    std::vector<float> s((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        const float v = amp * (float)i / (float)frames;
        s[(size_t)i * 2] = v; s[(size_t)i * 2 + 1] = v;
    }
    REQUIRE(writeWavStereo16(p, s, 48000));
}
} // namespace

TEST_CASE("render regression: hash deterministicke sceny (bit-exact guard)") {
    // 0 = placeholder → test vypise skutecne hodnoty; po zafixovani PASS.
    static const uint64_t kExpected[8] = {
        4545704654523935335ull,   // [0] onset + ton
        1048932552887497177ull,   // [1] pedal + rezonance
        17441043737466344350ull,  // [2] retrigger (damp)
        9766627733645862841ull,   // [3] steal (pool 2)
        6963393830382077346ull,   // [4] release + pedal up
        9143863059838577852ull,   // [5] novy ton
        17648933142900229859ull,  // [6] panic release
        16816433311640641763ull,  // [7] doznivani
    };

    const std::string dir = "/tmp/ithaca_regr_bank";
    fs::remove_all(dir); fs::create_directories(dir);
    // 12000 frames @48k < 2*preload(150ms=7200) → FullyLoaded (deterministicke).
    for (int n : {48, 60, 64, 67, 72}) {
        char name[40];
        std::snprintf(name, sizeof(name), "/m%03d-vel0-f48.wav", n);
        writeRampWav(dir + name, 12000, 0.2f + 0.1f * (float)((n - 48) % 5));
    }
    ithaca::log::Logger::default_().setOutputMode(false, false);

    Engine e;
    EngineConfig cfg;
    cfg.sample_rate = 48000; cfg.block_size = 128;
    cfg.max_voices  = 2;                  // vynuti steal
    cfg.midi_from = 40; cfg.midi_to = 80;
    REQUIRE(e.init(cfg));
    REQUIRE(e.loadBank(dir));
    fs::remove_all(dir);

    std::vector<float> L(128), R(128);
    uint64_t h = 1469598103934665603ull;
    uint64_t got[8]; int gi = 0;
    auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            std::fill(L.begin(), L.end(), 0.f);
            std::fill(R.begin(), R.end(), 0.f);
            e.processBlock(L.data(), R.data(), 128);
            h = fnv1a(L.data(), R.data(), 128, h);
        }
        got[gi++] = h;
    };
    e.noteOn(60, 100);                          run(8);   // [0] onset + ton
    e.sustainPedal(127); e.noteOn(48, 90);      run(8);   // [1] pedal + rezonance
    e.noteOn(60, 80);                           run(4);   // [2] retrigger (damp)
    e.noteOn(64, 70); e.noteOn(67, 60);         run(8);   // [3] pool 2 → steal
    e.noteOff(60); e.sustainPedal(0);           run(8);   // [4] release + pedal up
    e.noteOn(72, 110);                          run(4);   // [5] novy ton
    e.allNotesOff();                            run(8);   // [6] panic release
    run(8);                                                // [7] doznivani

    bool placeholder = true;
    for (auto v : kExpected) if (v != 0) placeholder = false;
    for (int i = 0; i < 8; ++i) {
        if (placeholder)
            MESSAGE("kExpected[" << i << "] = " << got[i] << "ull");
        else
            CHECK(got[i] == kExpected[i]);
    }
    CHECK_FALSE(placeholder);   // donuti zafixovat hodnoty
}
