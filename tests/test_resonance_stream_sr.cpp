// tests/test_resonance_stream_sr.cpp
// -----------------------------------
// Regresni test na BUG A (pos_inc_ ignorovan v ring-read) — SPECIFICKY pro
// STREAMOVANY ResonanceVoice (sympaticka rezonance). Hlavni Voice ma uz svuj
// dedikovany test (test_sample_rate_and_reload.cpp); tady overujeme, ze stejny
// fix funguje i v `else if (ring_)` vetvi resonance_voice.cpp::process().
//
// Princip: zahrajeme notu 72 (C5) → harmonicky vybudi rezonanci na note 60
// (C4, oktava nize). harmonicProximity(60, 72): diff=12 → octaves=1, semis=0 →
// kIntervalWeight[0]=1.0 * kOctaveDecay(0.7) = 0.70 >> kResonanceHarmonicMin(0.05).
// Sustain pedal drzime dole (cc64=127) → vsechny struny undamped → eligibilni.
//
// Obe noty jsou STEREO ramp WAV @ header SR 96000, 96000 frames (L=R=i/96000).
// preload_ms=50 @96k → head 4800 frames; sample 96000 > 2*head → Streamed.
// resonance_window_ms=100 @96k → preload_resonance ~9600 frames; zbytek (~84000)
// tece pres ring → trefime PRESNE ring-read vetev.
//
// pos_inc_ = 96000/48000 = 2.0.
//  - FIX (konzumuj ring az do floor(position_)): rezonance projde cely soubor
//    1x. Output frames @48k ≈ (96000 - res_start)/2.0 ≈ 46800 ≈ ~183 bloku/256.
//  - BROKEN (1 popFrame/output): ring se drenuje 1 frame/vystup, ale position_
//    roste 2x → ring se vyprazdni az po ~84000 vystupech → ~347 bloku, hraje
//    ~2x dele. Prah 280 bloku obe varianty cleanly oddeli.
//
// Excite-decay je vyrazene (excite_decay_ms ~1e9) aby duration urcoval VYHRADNE
// ring-read, ne exponencialni decay obalky (jinak by byl diskriminator slaby).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine.h"
#include "io/wav_writer.h"

#include <algorithm>
#include <chrono>
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
        path = fs::temp_directory_path() / ("ithaca_resstream_" + tag);
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

// Zapise STEREO ramp WAV: frame i → L=R=i/frames, header SR = file_sr.
void writeRamp(const std::string& path, int frames, int file_sr) {
    std::vector<float> samples((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        float v = (float)i / (float)frames;
        samples[(size_t)i * 2]     = v;
        samples[(size_t)i * 2 + 1] = v;
    }
    REQUIRE(writeWavStereo16(path, samples, file_sr));
}
} // namespace

TEST_CASE("Streamed resonance @96kHz traverses file once (ring honors pos_inc)") {
    constexpr int file_sr = 96000;
    constexpr int frames  = 96000;
    constexpr int played  = 72;   // C5
    constexpr int reso    = 60;   // C4 — oktava nize, harmonicProximity=0.70

    TempDir tmp{"sr96"};
    writeRamp((tmp.path / ("m0" + std::to_string(reso)  + "-vel1-f96.wav")).string(), frames, file_sr);
    writeRamp((tmp.path / ("m0" + std::to_string(played) + "-vel1-f96.wav")).string(), frames, file_sr);

    Engine eng;
    EngineConfig cfg;
    cfg.sample_rate         = 48000;
    cfg.block_size          = 256;
    cfg.midi_from           = 59;
    cfg.midi_to             = 73;
    cfg.preload_ms          = 50;    // head 4800 @96k → Streamed (96000 > 9600)
    cfg.resonance_window_ms = 100;   // preload_resonance ~9600 → zbytek streamuje
    cfg.resonance_strength  = 0.5f;  // default; excite = 1.0*0.70*0.5 = 0.35
    cfg.excite_decay_ms     = 1.0e9f; // prakticky vypnuty decay → duration = ring-read
    REQUIRE(eng.init(cfg));
    REQUIRE(eng.loadBank(tmp.path.string()));

    // Sustain pedal dolu → vsechny struny undamped (jinak dampingFor(N)=0 →
    // isUndamped(N) false → rezonance se nikdy nespusti).
    eng.sustainPedal(127);
    eng.noteOn(played, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    constexpr int block       = 256;
    constexpr int max_blocks  = 1500;
    std::vector<float> L(block), R(block);

    bool reso_started   = false;
    bool reso_inactive  = false;
    int  active_blocks  = 0;   // bloky, behem nichz znela ALESPON jedna rezonance

    for (int b = 0; b < max_blocks; ++b) {
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        eng.processBlock(L.data(), R.data(), block);

        if (b == 0) {
            // Po prvnim bloku se MIDI fronta vyprazdnila → onPlayedNoteOn spustil
            // rezonanci na note 60. Pokud 0, je spatny note-pair/eligibilita —
            // NEoslabovat prah, opravit notu.
            REQUIRE(eng.resonanceVoices() >= 1);
        }
        if (eng.resonanceVoices() >= 1) { reso_started = true; active_blocks++; }
        if (reso_started && eng.resonanceVoices() == 0) { reso_inactive = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Rezonance se v ramci rozpoctu sama deaktivovala (dohrala cely soubor 1x).
    CHECK(reso_started);
    CHECK(reso_inactive);
    // KLICOVY diskriminator BUG A: spravny pos_inc=2.0 projde 96000-frame soubor
    // za ~46800 vystupnich frames @48k = ~183 bloku/256. Rozbita 1:1 ring
    // konzumace dreni ring 1 frame/vystup → ~84000 vystupu → ~347 bloku (~2x
    // dele). Mereno: fix ~183, broken ~347. Prah 280 je cleanly oddeli.
    CHECK(active_blocks < 280);

    eng.allNotesOff();
}
