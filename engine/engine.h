#pragma once
// engine/engine.h
// ---------------
// Fasada celeho prehravaciho enginu (headless). Drzi banku, voice pool a
// lock-free MIDI frontu. noteOn/noteOff jsou thread-safe (jen vlozi do fronty);
// processBlock bezi na audio threadu — vyprazdni frontu a renderuje. Master
// gain post-mix. Streaming/DSP/rezonance jsou dalsi faze.

#include "sample/sample_types.h"
#include "stream/stream_engine.h"
#include "voice/voice_pool.h"
#include "voice/patch_manager.h"
#include "midi/midi_queue.h"

#include <atomic>
#include <memory>
#include <string>

namespace ithaca {

struct EngineConfig {
    int   sample_rate    = 48000;
    int   block_size     = 256;
    int   max_voices     = 128;
    float master_gain    = 1.0f;     // linearni
    float release_ms     = 200.f;
    float keyboard_spread = 0.6f;
    int   midi_from      = 0;        // rozsah nacitane banky (rychle testy)
    int   midi_to        = 127;
    int   preload_ms     = 150;      // preload velikost hlavy samplu v ms; ovlivnuje RAM i streaming bezpecnost
    int   resonance_window_ms = 500;  // delka preload_resonance regionu (Streamed mic)
    // -- Faze 4 streaming --
    int   stream_threads        = 1;      // pocet worker threads (zatim 1)
    int   ring_capacity_frames  = 8192;   // ring per Voice (~170 ms @ 48k)
    int   num_rings             = 32;     // velikost ring poolu
};

class Engine {
public:
    Engine();
    ~Engine();

    bool init(const EngineConfig& cfg);
    // Nacti legacy banku do RAM (respektuje cfg.midi_from/to). Vrati false kdyz nic.
    bool loadBank(const std::string& dir);

    // -- Thread-safe MIDI vstup (volat z MIDI/GUI threadu) --
    void noteOn(int midi, int velocity);
    void noteOff(int midi);
    void allNotesOff();

    // -- Audio thread -- renderuj n_samples do interleaved-by-caller L/R bufferu.
    // Caller buffery nuluje. Drainuje MIDI frontu pred renderem.
    void processBlock(float* out_l, float* out_r, int n_samples) noexcept;

    int  sampleRate() const { return cfg_.sample_rate; }
    int  blockSize()  const { return cfg_.block_size; }
    int  activeVoices() const { return pool_ ? pool_->activeCount() : 0; }
    void setMasterGain(float g) { master_gain_.store(g, std::memory_order_relaxed); }

    // Faze 4 (uzivatelska volba): za behu zmen block size (jako icr/icr2 select).
    // Volajici, ktery drzi AudioDevice mimo Engine, musi nasledne audio device
    // restartovat — Engine sam audio device nedrzi, jen aktualizuje cfg + refill
    // threshold. Vraci nove platny block size (clamped do rozumnych mezi).
    int  setBlockSize(int new_block_size) noexcept;

    // Pristup ke streaming enginu (potreba pro inspect / diag / GUI).
    StreamEngine* streamEngine() { return stream_.get(); }

private:
    // Prepocita StreamEngine refill threshold dle aktualniho block_size.
    void recomputeRefillThreshold() noexcept;

    EngineConfig                  cfg_;
    Bank                          bank_;
    std::unique_ptr<VoicePool>    pool_;
    RoundRobinState               rr_;
    MidiQueue                     midi_q_;
    std::unique_ptr<StreamEngine> stream_;
    std::atomic<float>            master_gain_{1.0f};
    bool                          initialized_ = false;
};

} // namespace ithaca
