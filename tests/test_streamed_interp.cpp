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
#include "stream/stream_engine.h"
#include "util/log.h"
#include "voice/voice_pool.h"

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

TEST_CASE("Streamed clean end loguje END-OF-SAMPLE (Info), ne UNDERRUN (Warning)") {
    // Cisty konec streamovaneho vzorku: ring se vyprazdni az kdyz uz byl cely
    // soubor vyzadan → ma se logovat jako Info "END-OF-SAMPLE", NE jako
    // Warning "UNDERRUN" (to je matouci). Voice loguje pres RT ring (LOG_RT_*),
    // subscriberum se zpravy doruci pri flushRTBuffer() — v testu explicitne.
    TempDir tmp{"cleanend"};
    constexpr int frames = 20000;
    writeRamp(tmp.path, frames, 48000, "48");

    auto& lg = ithaca::log::Logger::default_();
    lg.setOutputMode(false, false);   // ticho behem testu
    lg.clearSubscribers();
    bool saw_end_of_sample_info = false;
    bool saw_underrun_warning   = false;
    lg.addSubscriber([&](const ithaca::log::LogEntry& e) {
        if (e.topic != "voice_end") return;
        if (e.sev == ithaca::log::Severity::Info &&
            e.message.find("END-OF-SAMPLE") != std::string::npos)
            saw_end_of_sample_info = true;
        if (e.sev == ithaca::log::Severity::Warning &&
            e.message.find("UNDERRUN") != std::string::npos)
            saw_underrun_warning = true;
    });

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    (void)renderNote(eng, 1200);
    lg.flushRTBuffer();   // doruc RT zpravy (voice_end) subscriberum

    lg.clearSubscribers();
    lg.setOutputMode(true, false);    // restore

    CHECK(eng.activeVoices() == 0);
    CHECK(saw_end_of_sample_info);
    CHECK_FALSE(saw_underrun_warning);
}

TEST_CASE("underrun fade tvaruje drzeny posledni vzorek, ne tvrdy strih do nuly") {
    // Streamed mic s headem plnym 0.5, workery NEbezi → ring prazdny → underrun
    // hned za headem. Fade musi tvarovat POSLEDNI DRZENY vzorek (~0.5 → 0);
    // drive sL=sR=0 a 5ms rampa nasobila nuly = tvrdy strih/klik.
    StreamEngine se(2, 64, 1);                 // start() nevolame
    SampleAsset a;
    MicLayer m;
    m.file.frames      = 100000;
    m.file.sample_rate = 48000;
    m.file.valid       = true;
    m.mode             = MicLayerMode::Streamed;
    m.head_frames      = 64;
    m.preload_head.assign(64 * 2, 0.5f);
    a.mics.push_back(std::move(m));
    VoicePool pool(1);
    pool.setStreamEngine(&se);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 1.0; vs.vel_gain = 1.0f;
    pool.noteOn(60, vs, 48000.f);
    std::vector<float> L(256, 0.f), R(256, 0.f);
    pool.processBlock(L.data(), R.data(), 256, 48000.f);
    // Za headem (frame >64) underrun: 5ms fade drzeneho 0.5 → nenulove vzorky.
    CHECK(std::fabs(L[100]) > 0.05f);
    // Fade dobehne do nuly (64+240 < 512) → druhy blok konci tichem.
    std::vector<float> L2(256, 0.f), R2(256, 0.f);
    pool.processBlock(L2.data(), R2.data(), 256, 48000.f);
    CHECK(std::fabs(L2[255]) == doctest::Approx(0.f));
}
