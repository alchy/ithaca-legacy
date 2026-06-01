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
#include "pedal/pedal_state.h"
#include "resonance/resonance_engine.h"

#include <atomic>
#include <memory>
#include <string>

namespace ithaca {

struct EngineConfig {
    int   sample_rate    = 48000;
    int   block_size     = 256;
    int   max_voices     = 256;     // hlavni voice pool (max kMaxPoolSize)
    float master_gain    = 1.0f;     // linearni
    float release_ms     = 200.f;
    float keyboard_spread = 0.6f;
    int   midi_from      = 0;        // rozsah nacitane banky (rychle testy)
    int   midi_to        = 127;
    int   preload_ms     = 150;      // preload velikost hlavy samplu v ms; ovlivnuje RAM i streaming bezpecnost
    int   resonance_window_ms = 500;  // delka preload_resonance regionu (Streamed mic)
    // -- Faze 4 streaming --
    // Pocet worker threadu paralelne pres stream queue. Vice workeru = vetsi
    // propustnost disku I/O = mensi sance underrunu pri akordu + rezonanci
    // v jednom audio bloku. Default 4. Na vice-jadernych systemech zvazit 6-8.
    int   stream_threads        = 4;
    int   ring_capacity_frames  = 8192;   // ring per Voice (~170 ms @ 48k)
    // Ring pool je SDILENY mezi hlavnimi (max_voices) a rezonancnimi
    // (max_resonance_voices) hlasy. Default pokryva plnou polyfonii, aby
    // pri pedalu + rezonanci nedoslo k `acquireRing → nullptr` (hlas by pak
    // hral jen preload_head ~150 ms a utichl). Engine::init si pohlida, ze
    // num_rings >= max_voices + max_resonance_voices.
    // Pamet: num_rings × ring_capacity_frames × 2 (stereo) × 4 (float) bytes.
    // Default 288 × 8192 × 8 = ~18 MB.
    int   num_rings             = 288;
    // -- Faze 5 sympaticka rezonance --
    float resonance_strength    = 0.5f;   // 0..1, expose pres CLI
    int   max_resonance_voices  = 32;     // hard cap pro rezonancni pool
    float excite_decay_ms       = 5000.f; // tau prirozeneho decay last_excite
};

class Engine {
public:
    Engine();
    ~Engine();

    bool init(const EngineConfig& cfg);
    // Nacti legacy banku do RAM (respektuje cfg.midi_from/to). Vrati false kdyz nic.
    bool loadBank(const std::string& dir);
    // Thread-safe reload banky z GUI/CLI threadu, pres "graceful pause":
    //   1) push AllNotesOff do MIDI fronty (audio drain ji zpracuje pristi blok),
    //   2) pockej ~50 ms aby release dobehl,
    //   3) zapni bank_loading_ flag → audio thread zacne vracet ticho a preskoci
    //      drain MIDI / voice render (viz processBlock top),
    //   4) pockej ~10 ms aby pripadny in-flight processBlock dobehl,
    //   5) loadBank(path) (disk I/O je teď bezpecne, audio mlci),
    //   6) bank_loading_=false → audio thread se obnovi.
    // Volat POUZE z non-RT threadu (GUI/main); blokuje ~60 ms.
    bool reloadBank(const std::string& dir);

    // -- Thread-safe MIDI vstup (volat z MIDI/GUI threadu) --
    void noteOn(int midi, int velocity);
    void noteOff(int midi);
    void allNotesOff();
    // Sustain pedal CC64 — spojita hodnota 0..127. Promita se do PedalState
    // a per-blok do rezonanci (viz spec 5.4 + 5.5). Thread-safe (jen vlozi
    // udalost do MidiQueue).
    void sustainPedal(uint8_t cc);

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

    // -- Diagnostika (GUI/monitor; thread-safe atomic loads) --
    // Pocet aktivnich rezonancnich hlasu (sympaticka rezonance, faze 5).
    int     resonanceVoices() const noexcept;
    // Pocet aktualne pouzitych streaming ringu (in_use_ flag).
    int     numRingsUsed()    const noexcept;
    // Aktualni hodnota sustain pedalu (CC64, 0..127).
    uint8_t pedalCC()         const noexcept;
    // Blikani indikatoru: true kdyz posledni note-on / note-off event nastal
    // pred mene nez `ms` ms (GUI lampy NOTE/OFF). Cas drzi engine (steady_clock).
    bool    noteOnRecent(float ms)  const noexcept;
    bool    noteOffRecent(float ms) const noexcept;
    // Maska aktivnich main voice midi cisel; vyplni 128 bool out.
    // out[i] = true kdyz aspon jeden hlas s midi=i je active() (vc. releasing).
    void    activeMidiNotes(bool out[128]) const noexcept;
    // Maska sympaticky rezonujicich strun; out[i] = true kdyz nota i prave
    // rezonuje (sekundarni hlas). Pro odliseni na klaviature od primarnich.
    void    resonatingMidiNotes(bool out[128]) const noexcept;
    // Max currentLevel pres vsechny voicy te midi (pro keyboard viz alpha).
    // midi mimo rozsah <0,127> → 0.f. Nezohlednuje releasing samostatne.
    float   currentGainFor(int midi) const noexcept;

    // -- Runtime parametry (GUI; atomic / single-thread-safe) --
    void setReleaseMs(float ms) noexcept;
    void setResonanceStrength(float s) noexcept;   // wrap resonance_->setStrength
    void setExciteDecayMs(float ms) noexcept;      // wrap resonance_->setExciteDecayTimeMs

    // -- Master peak meter (GUI; atomic) --
    // Vraci aktualni peak |out| po master_gain, s decay ~100ms mezi bloky.
    // Cteni je lock-free, GUI muze vzorkovat libovolne casto.
    float masterPeakL() const noexcept { return master_peak_l_.load(std::memory_order_relaxed); }
    float masterPeakR() const noexcept { return master_peak_r_.load(std::memory_order_relaxed); }

private:
    // Prepocita StreamEngine refill threshold dle aktualniho block_size.
    void recomputeRefillThreshold() noexcept;

    // Half-pedal continuous release scaling (spec 5.4). Vraci skalovany
    // release_ms podle aktualniho cc64_ (CC0 → ×1, CC127 → ~×20).
    float scaledReleaseMs() const;

    EngineConfig                      cfg_;
    Bank                              bank_;
    std::unique_ptr<VoicePool>        pool_;
    RoundRobinState                   rr_;
    MidiQueue                         midi_q_;
    std::unique_ptr<StreamEngine>     stream_;
    PedalState                        pedal_;
    std::unique_ptr<ResonanceEngine>  resonance_;
    std::atomic<float>                master_gain_{1.0f};
    // Master peak meter — psano z audio threadu (processBlock), cteno z GUI.
    std::atomic<float>                master_peak_l_{0.f};
    std::atomic<float>                master_peak_r_{0.f};
    // Bank reload guard. Pokud true, processBlock vraci ticho a preskoci
    // veskery render / drain — chrani pred race s loadBank na non-RT threadu.
    std::atomic<bool>                 bank_loading_{false};
    // Indikator note-on/off blikani: timestamp (steady_clock micros) posledniho
    // eventu. Psano z MIDI/GUI threadu pri noteOn/noteOff, cteno z GUI.
    std::atomic<uint64_t>             last_note_on_us_{0};
    std::atomic<uint64_t>             last_note_off_us_{0};
    bool                              initialized_ = false;
};

} // namespace ithaca
