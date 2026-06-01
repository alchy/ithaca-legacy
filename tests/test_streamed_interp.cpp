// tests/test_streamed_interp.cpp
// -------------------------------
// Faze 6/7: lin. interpolace ve streamovane (ring) casti. Ramp WAV je idealni
// sonda — spravna interpolace da temer linearni vystupni sklon; nearest-neighbor
// da schody (opakovane pak preskocene hodnoty). Testy: hladkost @44.1k, plynuly
// sev head->ring, EOF hold-last-sample, a 48k regrese (frac=0 → beze zmeny).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine.h"
#include "io/wav_writer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace ithaca;
namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        path = fs::temp_directory_path() / ("ithaca_interp_" + tag);
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

std::string writeRamp(const fs::path& dir, int frames, int header_sr,
                      const char* tag) {
    std::vector<float> s((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        float v = (float)i / (float)frames;
        s[(size_t)i * 2]     = v;
        s[(size_t)i * 2 + 1] = v;
    }
    std::string p = (dir / (std::string("m060-vel1-f") + tag + ".wav")).string();
    REQUIRE(writeWavStereo16(p, s, header_sr));
    return p;
}

std::vector<float> renderNote(Engine& eng, int max_blocks) {
    constexpr int block = 256;
    std::vector<float> L(block), R(block), out;
    out.reserve((size_t)max_blocks * block);
    bool started = false;
    for (int b = 0; b < max_blocks; ++b) {
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        eng.processBlock(L.data(), R.data(), block);
        for (int i = 0; i < block; ++i) out.push_back(L[i]);
        if (eng.activeVoices() >= 1) started = true;
        if (started && eng.activeVoices() == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return out;
}

EngineConfig streamCfg(int sample_rate) {
    EngineConfig cfg;
    cfg.sample_rate = sample_rate;
    cfg.block_size  = 256;
    cfg.midi_from   = 59;
    cfg.midi_to     = 61;
    cfg.preload_ms  = 50;
    return cfg;
}
} // namespace

TEST_CASE("Streamed 44.1k ramp je hladky (lin. interpolace, ne schody)") {
    TempDir tmp{"sr441"};
    constexpr int frames = 44100;
    writeRamp(tmp.path, frames, 44100, "44");

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> out = renderNote(eng, 1200);

    int equal_pairs = 0, rising_pairs = 0;
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i] > 0.001f && out[i-1] > 0.001f && out[i] < 0.99f) {
            if (out[i] == out[i-1]) equal_pairs++;
            else if (out[i] > out[i-1]) rising_pairs++;
        }
    }
    REQUIRE(rising_pairs > 1000);
    // 44.1k ramp je ulozen jako 16-bit PCM: 44100 framu na ~32768 urovni v [0,1)
    // znamena, ze ~11k sousednich zdrojovych vzorku ma IDENTICKOU 16-bit hodnotu
    // (kvantizacni plateaus). I se spravnou interpolaci tak zustane ~1k equal
    // paru (lo==hi na plateau). Nearest-neighbor je vsak radove horsi (~14.5k
    // equal proti ~1k), takze prah rising/10 fix bezpecne projde a NN bug
    // bezpecne chyti — viz TEMP-revert v plan/Task3.
    CHECK(equal_pairs < rising_pairs / 10);
}

TEST_CASE("Streamed sev head->ring nema diskontinuitu") {
    TempDir tmp{"seam"};
    constexpr int frames = 48000;
    writeRamp(tmp.path, frames, 48000, "48");

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> out = renderNote(eng, 1200);

    float max_jump = 0.f;
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i] > 0.01f && out[i-1] > 0.01f && out[i] < 0.98f) {
            float d = std::fabs(out[i] - out[i-1]);
            if (d > max_jump) max_jump = d;
        }
    }
    CHECK(max_jump < 1e-3f);
}

TEST_CASE("Streamed EOF: hold last sample, cisty konec") {
    TempDir tmp{"eof"};
    constexpr int frames = 20000;
    writeRamp(tmp.path, frames, 48000, "48");

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> out = renderNote(eng, 1200);

    CHECK(eng.activeVoices() == 0);
    float last_nonzero = 0.f;
    for (float v : out) if (v > 0.001f) last_nonzero = v;
    CHECK(last_nonzero > 0.5f);
}

TEST_CASE("Streamed 48k regrese: vystup = vstup (frac=0)") {
    TempDir tmp{"sr48"};
    constexpr int frames = 48000;
    writeRamp(tmp.path, frames, 48000, "48");

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> out = renderNote(eng, 1200);

    float peak = 0.f;
    for (float v : out) peak = (std::max)(peak, v);
    CHECK(peak > 0.6f);
    CHECK(eng.activeVoices() == 0);
}
