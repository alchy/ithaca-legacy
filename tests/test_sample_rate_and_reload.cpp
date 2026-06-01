// tests/test_sample_rate_and_reload.cpp
// -------------------------------------
// Dva regresni testy na potvrzene logicke chyby:
//   BUG A: streamovana (ring) cast Voice/ResonanceVoice ignorovala pos_inc_ —
//          popovala vzdy 1 frame/vystup, ale position_ rostl o pos_inc_. Pro
//          sample s header SR != 48000 (napr. 96000 → pos_inc=2.0) to hralo
//          spatnou vysku a position_ utikal pred realnou konzumaci ringu →
//          predcasny EOF/hard-guard utnul notu.
//   BUG B: Engine::reloadBank uvolnoval starou banku, zatimco doznivajici
//          (release-ramping) hlasy stale ukazovaly raw const MicLayer*/
//          SampleAsset* do uz uvolnene pameti → use-after-free.
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
        path = fs::temp_directory_path() / ("ithaca_srreload_" + tag);
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};
} // namespace

TEST_CASE("Streamed 96kHz sample plays full ramp (ring honors pos_inc)") {
    // STEREO ramp, 96000 frames, header SR = 96000. frame i: L=R=i/96000 (0..~1).
    // preload_ms=50 @96k → head ~4800 frames; sample 96000 > 2*head → Streamed.
    // Spravne pos_inc=96000/48000=2.0 projde CELOU rampou → peak ~ ramp_max*pan
    // (~0.71). Rozbity 1:1 ring dojde jen do ~poloviny rampy (peak ~0.38) a
    // utne brzy. Prah 0.6 je oddeli.
    constexpr int file_sr = 96000;
    constexpr int frames  = 96000;
    constexpr int midi    = 60;

    TempDir tmp{"sr96"};

    std::vector<float> samples((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        float v = (float)i / (float)frames;
        samples[(size_t)i * 2]     = v;
        samples[(size_t)i * 2 + 1] = v;
    }
    std::string wav_path = (tmp.path / "m060-vel1-f96.wav").string();
    REQUIRE(writeWavStereo16(wav_path, samples, file_sr));

    Engine eng;
    EngineConfig cfg;
    cfg.sample_rate = 48000;
    cfg.block_size  = 256;
    cfg.midi_from   = 59;
    cfg.midi_to     = 61;
    cfg.preload_ms  = 50;
    REQUIRE(eng.init(cfg));
    REQUIRE(eng.loadBank(tmp.path.string()));

    eng.noteOn(midi, 127);   // vel 127 → vel_gain 1.0
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    constexpr int block      = 256;
    constexpr int max_blocks  = 800;
    std::vector<float> L(block), R(block);

    float peak = 0.f;
    bool  note_started = false;
    bool  went_inactive = false;
    int   active_blocks = 0;   // pocet renderovacich bloku, behem nichz hlas znel

    for (int b = 0; b < max_blocks; ++b) {
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        eng.processBlock(L.data(), R.data(), block);
        for (int i = 0; i < block; ++i) {
            peak = (std::max)(peak, (std::max)(std::fabs(L[i]), std::fabs(R[i])));
        }
        if (eng.activeVoices() >= 1) { note_started = true; active_blocks++; }
        if (note_started && eng.activeVoices() == 0) { went_inactive = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Hlas se nakonec deaktivoval (dohral cely sample) a dorazil az k vrcholu rampy.
    CHECK(went_inactive);
    CHECK(peak > 0.6f);
    // KLICOVY diskriminator BUG A: spravny pos_inc=2.0 prehraje 96000-frame soubor
    // (header 96 kHz) za ~48000 vystupnich frames @48k = ~188 bloku po 256.
    // Rozbita 1:1 ring konzumace cte jen 1 frame/vystup → position_ utika 2x rychleji
    // nez realna konzumace → hraje ~2x dele (~376 bloku, az hard-guard/underrun utne
    // pri position_ ~2x total). Mereno: fix ~188, broken ~376. Prah 280 je oddeli.
    CHECK(active_blocks < 280);

    eng.allNotesOff();
}

TEST_CASE("reloadBank hard-stops active voices (no dangling bank pointers)") {
    // SHORT stereo WAV, 1000 frames, header SR=48000, konstantni amplituda 0.5.
    // Pred fixem reloadBank nechaval release-ramping hlasy active() (ukazujici
    // do uvolnene bank pameti). Po fixu jsou tvrde zastaveny.
    constexpr int file_sr = 48000;
    constexpr int frames  = 1000;
    constexpr int midi    = 60;

    TempDir tmp{"reload"};

    std::vector<float> samples((size_t)frames * 2, 0.5f);
    std::string wav_path = (tmp.path / "m060-vel1-f48.wav").string();
    REQUIRE(writeWavStereo16(wav_path, samples, file_sr));

    Engine eng;
    EngineConfig cfg;
    cfg.sample_rate = 48000;
    cfg.block_size  = 256;
    cfg.midi_from   = 59;
    cfg.midi_to     = 61;
    REQUIRE(eng.init(cfg));
    REQUIRE(eng.loadBank(tmp.path.string()));

    eng.noteOn(midi, 100);

    // Render jeden blok → MIDI fronta se vyprazdni a hlas se spusti.
    std::vector<float> L(256), R(256);
    std::fill(L.begin(), L.end(), 0.f);
    std::fill(R.begin(), R.end(), 0.f);
    eng.processBlock(L.data(), R.data(), 256);
    REQUIRE(eng.activeVoices() >= 1);   // precondition

    eng.reloadBank(tmp.path.string());

    CHECK(eng.activeVoices() == 0);
    CHECK(eng.resonanceVoices() == 0);
}
