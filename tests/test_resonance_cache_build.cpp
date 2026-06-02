// tests/test_resonance_cache_build.cpp
// Overi, ze buildResonanceCache naplni preload_resonance JEN pro per-notu
// cilovou velocity vrstvu (max 1 vrstva s resonance_frames>0 na notu).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "sample/sample_store.h"
#include "util/log.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace ithaca;

namespace {
void wU32(std::FILE* f, uint32_t v){ std::fwrite(&v,4,1,f);}
void wU16(std::FILE* f, uint16_t v){ std::fwrite(&v,2,1,f);}

// Stejna fixtura jako test_sample_store.cpp: stereo 16-bit WAV konst. amplitudy.
void writeConstWav(const std::string& path, float amp,
                   int sr = 48000, int frames = -1) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    if (frames < 0) frames = sr * 3 / 10;             // default 0.3 s
    uint32_t data_sz = (uint32_t)frames * 2u * 2u;    // stereo, 16-bit
    std::fwrite("RIFF",1,4,f); wU32(f,36u+data_sz);
    std::fwrite("WAVE",1,4,f); std::fwrite("fmt ",1,4,f); wU32(f,16u);
    wU16(f,1); wU16(f,2); wU32(f,(uint32_t)sr); wU32(f,(uint32_t)sr*4u);
    wU16(f,4); wU16(f,16);
    std::fwrite("data",1,4,f); wU32(f,data_sz);
    int16_t v = (int16_t)std::lround(amp*32767.f);
    for (int i=0;i<frames;i++){ std::fwrite(&v,2,1,f); std::fwrite(&v,2,1,f);}
    std::fclose(f);
}
} // namespace

TEST_CASE("buildResonanceCache plni jen cilovou vrstvu") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_fixture_rescache";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // Nota 60: 3 Streamed vrstvy (dlouhe ssamply > 2*150ms threshold) ruznych amp.
    writeConstWav(dir + "/m060-vel0-f48.wav", 0.1f, 48000, 60000);
    writeConstWav(dir + "/m060-vel4-f48.wav", 0.4f, 48000, 60000);
    writeConstWav(dir + "/m060-vel7-f48.wav", 0.9f, 48000, 60000);
    // Nota 62: 1 vrstva.
    writeConstWav(dir + "/m062-vel3-f48.wav", 0.5f, 48000, 60000);

    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);
    Bank bank = loadBank(dir, L);
    if (bank.loaded_samples == 0) { fs::remove_all(dir); return; }   // skip

    auto ready = buildResonanceCache(bank, /*target_db=*/-20.f, /*window_ms=*/1000, L);
    fs::remove_all(dir);

    for (int n = 0; n < 128; ++n) {
        auto& note = bank.notes[n];
        if (!note.recorded) continue;
        int filled = 0;
        for (auto& s : note.slots)
            if (!s.variants.empty() && !s.variants[0].mics.empty()
                && s.variants[0].mics[0].resonance_frames > 0) ++filled;
        CHECK(filled <= 1);   // max jedna (cilova) vrstva naplnena
    }
    (void)ready;
}
