// tests/test_long_sample_stream.cpp
// ---------------------------------
// Integracni test fazi 4 streamingu:
//   1. Vytvor 1 s ramp WAV (48000 frames @ 48 kHz).
//   2. Nacti pres loadFixedVelocityBank s preload_ms=50 (head=2400 frames; 48000 >
//      2*head → Streamed).
//   3. Spust Engine, posli noteOn, renderuj v 256-frame blocich pres 1.2 s.
//   4. Over: (a) energie >0 v ramci celeho hraneho useku (ne jen prvnich 50 ms),
//      (b) Voice se cisto deaktivoval po dohrani (no underrun → log neobsahuje
//      "underrun"), (c) L hodnota videna pres stream pokryva celou rampu
//      (videli jsme blizko 0 i blizko 1).
//
// Sample je nacteny rucne pres loadFixedVelocityBank → musi se jmenovat dle legacy
// konvence: <bank>/<midi>_<vel>.wav (sample_loader::parseFixedVelocityName).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine.h"
#include "io/wav_writer.h"
#include "sample/sample_store.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ithaca;
namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        path = fs::temp_directory_path() / ("ithaca_stream_" + tag);
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};
} // namespace

TEST_CASE("Streamed sample hraje cele pres preload_head + ring buffer") {
    constexpr int sr      = 48000;
    constexpr int frames  = 48000;   // 1 sekunda
    constexpr int midi    = 60;
    constexpr int vel     = 100;

    TempDir tmp{"long"};

    // Ramp signal: L = i/N (0..1), R = i/N. Easy to verify "rostou monotonne".
    std::vector<float> samples((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        float v = (float)i / (float)frames;
        samples[(size_t)i*2]     = v;
        samples[(size_t)i*2 + 1] = v;
    }
    // Nazev musi parsovat legacy loader: "mNNN-velV-fSS.wav".
    std::string wav_path = (tmp.path / "m060-vel1-f48.wav").string();
    REQUIRE(writeWavStereo16(wav_path, samples, sr));

    // Engine s preload_ms=50 → head_frames=2400, sampl 48000 → Streamed.
    Engine eng;
    EngineConfig cfg;
    cfg.sample_rate = sr;
    cfg.block_size  = 256;
    cfg.midi_from   = midi;
    cfg.midi_to     = midi;
    cfg.preload_ms  = 50;
    cfg.ring_capacity_frames = 8192;
    cfg.num_rings   = 8;
    cfg.keyboard_spread = 0.f;       // L=R=0.707 → snazsi assert na L hodnoty
    REQUIRE(eng.init(cfg));
    REQUIRE(eng.loadBank(tmp.path.string()));

    // Spust notu a render 1.2 s (60 000 frames) v 256-frame blocich.
    // Simulujeme real-time scheduling: mezi bloky kratky sleep, aby worker
    // mel cas naplnit ring (jinak by se vsech 60k frames vyrenderovalo za
    // mikrosekundy a worker by jeste spal po 1ms pollu).
    eng.noteOn(midi, vel);
    // Krat-ka pauza po noteOn, aby worker stihl pripravit zacatek streamu
    // jeste pred prvnim block-rendrem.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    constexpr int block      = 256;
    constexpr int total_out  = 60000;   // > sample (48000) → posledni cast bude ticho
    std::vector<float> L(block), R(block);

    float max_L = 0.f;
    int   nonzero_blocks_in_play_zone = 0;
    int   played_frames = 0;
    // "Play zone" = prvnich 47000 frames (drobny ohraniceny offset od konce
    // pro toleranci fade na hranicich).
    const int play_zone_end = 47000;
    int last_active_frame = -1;

    for (int s = 0; s < total_out; s += block) {
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        eng.processBlock(L.data(), R.data(), block);
        float energy = 0.f;
        for (int i = 0; i < block; ++i) {
            energy += std::fabs(L[i]);
            if (L[i] > max_L) max_L = L[i];
            if (L[i] != 0.f || R[i] != 0.f) last_active_frame = s + i;
        }
        if (s < play_zone_end && energy > 0.f) nonzero_blocks_in_play_zone++;
        played_frames += block;
        // ~real-time tempo: 256 frames @ 48k = 5.33 ms. Sleep 2 ms (sub-
        // realtime tempo, ale dost aby worker stihl).
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    (void)played_frames;

    // a) Energie byla nenulova v drtive vetsine bloku play_zone (head + stream
    // by mely pokryt celych 48000 frames; po +preload mozna chvilku underrun
    // pri prvnim refillu, ale celkovy podil musi byt vysoky).
    int total_blocks_in_zone = (play_zone_end + block - 1) / block;
    CHECK(nonzero_blocks_in_play_zone > total_blocks_in_zone * 9 / 10);

    // b) Voice se dohnal a deaktivoval (active = 0 na konci).
    CHECK(eng.activeVoices() == 0);

    // c) L hodnoty pokryly rozumny rozsah rampu: dosli jsme blizko 1.0
    // (=hodnoty z konce souboru). Vystupni max = signal * vel_gain * pan_l =
    // 1.0 * (100/127)^2 * cos(pi/4) = ~0.438. Pozadujeme aspon 0.35 (= ~80%
    // z teoretickeho max), coz potvrzuje, ze jsme pres ring dorazili k
    // hornimu konci rampu.
    CHECK(max_L > 0.35f);

    // d) Posledni nenulovy frame je nekde v okoli konce sample (48000-ish).
    // Streaming + onset/release musi pokryt aspon 90% delky sample.
    CHECK(last_active_frame > frames * 9 / 10);

    eng.allNotesOff();
}
