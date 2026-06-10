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
#include "midi/note_hold.h"
#include "pedal/pedal_state.h"
#include "resonance/resonance_engine.h"
#include "dsp/dsp_chain.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
    int   resonance_window_ms = 12000; // RAM cache rezonance: okno [ms] cilove vrstvy
    // RAM budget pro nacteni banky [MB]. 0 = AUTO (~60% fyzicke RAM dle sysinfo)
    // → ochrana pred OOM na embedded (RPi5/4GB). >0 = tvrdy strop. Pri prekroceni
    // se nacitani PRERUSI (neuplna banka + error log), misto pádu na bad_alloc.
    int   cache_budget_mb     = 0;     // 0 = auto
    // -- Faze 4 streaming --
    // Pocet worker threadu paralelne pres stream queue. Vice workeru = vetsi
    // propustnost disku I/O = mensi sance underrunu pri akordu + rezonanci
    // v jednom audio bloku. 0 = AUTO dle hardware_concurrency (viz Engine::init):
    // ~poloVina jader na main streaming, ~ctvrtina na rezonanci — nechava rezervu
    // pro audio + GUI thread (napr. 4-jadre RPi5 → 2 main + 1 reso).
    int   stream_threads        = 0;   // 0 = auto
    int   ring_capacity_frames  = 8192;   // ring per Voice (~170 ms @ 48k)
    // MAIN ring pool (>= max_voices). Oddeleny od rezonancniho poolu.
    int   num_rings             = 256;    // MAIN ring pool (>= max_voices)
    // -- Oddeleny streaming pro rezonanci (izolace od hlavnich hlasu) --
    int   resonance_num_rings      = 48;  // RESONANCE ring pool (>= max_resonance_voices)
    int   resonance_stream_threads = 0;   // 0 = auto (viz stream_threads / Engine::init)
    // -- Faze 5 sympaticka rezonance --
    bool  resonance_enabled     = true;
    float resonance_gain_db     = -12.f;  // dB; expose pres CLI/GUI
    float resonance_layer_db    = -30.f;  // dB cil pro vyber velocity vrstvy
    int   max_resonance_voices  = 32;     // hard cap pro rezonancni pool
    float excite_decay_ms       = 5000.f; // tau prirozeneho decay last_excite
    // RT priorita audio vlakna (SCHED_FIFO/time-constraint/MMCSS) — nastavi se
    // pri prvnim processBlock NA VOLAJICIM VLAKNE. Default OFF: zapinaji jen
    // realne audio aplikace (GUI, CLI --play). Jinak by testy/offline render
    // dostaly SCHED_FIFO na main threadu (vyhladoveni systemu, RLIMIT_RTTIME).
    bool  rt_priority           = false;
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
    //   4) handshake waitForAudioQuiesce(epoch+2): in-flight processBlock
    //      dobehl a dalsi blok uz videl flag (timeout 500 ms kryje stojici audio),
    //   5) join recache threadu, hard-stop hlasu, loadBank(path) (disk I/O je
    //      teď bezpecne, audio mlci),
    //   6) bank_loading_=false → audio thread se obnovi.
    // Volat POUZE z non-RT threadu (GUI/main); blokuje ~55 ms + 1-2 audio bloky.
    bool reloadBank(const std::string& dir);

    // -- Thread-safe MIDI vstup (volat z MIDI/GUI threadu) --
    // `channel` = MIDI kanal 0..15 (default 0 pro non-MIDI callery: CLI ton,
    // offline batch render, testy). Cross-channel hold: viz NoteHoldTracker.
    void noteOn(int midi, int velocity, int channel = 0);
    void noteOff(int midi, int channel = 0);
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
    // Detekovany format nactene banky (fixed-velocity / dynamic-velocity / …).
    // Unknown dokud neni nactena zadna banka. Pro GUI TYPE badge.
    BankFormat bankType() const noexcept { return bank_.format; }
    // Realna fakta o nactene bance (pro GUI). 0 dokud neni banka.
    int loadedSamples() const noexcept { return bank_.loaded_samples; }
    int recordedNotes() const noexcept {
        int n = 0;
        for (int i = 0; i < 128; ++i) if (bank_.notes[i].recorded) ++n;
        return n;
    }
    int  activeVoices() const { return pool_ ? pool_->activeCount() : 0; }
    void setMasterGain(float g) { master_gain_.store(g, std::memory_order_relaxed); }

    // Faze 4 (uzivatelska volba): za behu zmen block size (jako icr/icr2 select).
    // Volajici, ktery drzi AudioDevice mimo Engine, musi nasledne audio device
    // restartovat — Engine sam audio device nedrzi, jen aktualizuje cfg + refill
    // threshold. Vraci nove platny block size (clamped do rozumnych mezi).
    int  setBlockSize(int new_block_size) noexcept;

    // Pristup ke streaming enginu (potreba pro inspect / diag / GUI).
    StreamEngine* streamEngine() { return stream_main_.get(); }   // back-compat (main)

    // -- Diagnostika (GUI/monitor; thread-safe atomic loads) --
    // Pocet aktivnich rezonancnich hlasu (sympaticka rezonance, faze 5).
    int     resonanceVoices() const noexcept;
    // Pocet aktualne pouzitych streaming ringu (in_use_ flag).
    int     numRingsUsed()    const noexcept;
    int  mainRingsUsed()      const noexcept { return stream_main_ ? stream_main_->numRingsUsed() : 0; }
    int  mainRingsTotal()     const noexcept { return stream_main_ ? stream_main_->numRings() : 0; }
    int  resonanceRingsUsed() const noexcept { return stream_resonance_ ? stream_resonance_->numRingsUsed() : 0; }
    int  resonanceRingsTotal()const noexcept { return stream_resonance_ ? stream_resonance_->numRings() : 0; }
    bool mainStreamUnderrunRecent(float ms)      const noexcept { return stream_main_ && stream_main_->underrunRecent(ms); }
    bool resonanceStreamUnderrunRecent(float ms) const noexcept { return stream_resonance_ && stream_resonance_->underrunRecent(ms); }
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
    void setResonanceGainDb(float db) noexcept;
    void setResonanceLayerDb(float db) noexcept;
    void setResonanceEnabled(bool on) noexcept;
    // Runtime prestavba rezonancni cache pro novy layer target (GUI slider).
    // Fadene aktivni rezonance + ready=false (nove streamuji), pak na pozadi
    // znovu nacte cilove vrstvy a ready=true. Volat z GUI threadu (debounced).
    // Opakovane volani behem rebuilu se coalescuje (bg vlakno prebuduje znovu
    // na nejnovejsi cil).
    void rebuildResonanceCache(float target_db) noexcept;
    // True dokud bezi background rebuild rezonancni cache (GUI indikace).
    bool recacheInProgress() const noexcept;
    // Min/max peak RMS [dB] napric nactenou bankou (pro dynamicky rozsah GUI
    // slideru "Resonance Layer"). Bez banky default -60 / 0.
    float bankPeakRmsMinDb() const noexcept { return bank_peak_rms_min_db_; }
    float bankPeakRmsMaxDb() const noexcept { return bank_peak_rms_max_db_; }
    void setExciteDecayMs(float ms) noexcept;      // wrap resonance_->setExciteDecayTimeMs
    void setMaxResonanceVoices(int n) noexcept { if (resonance_) resonance_->setMaxVoices(n); }
    int  maxResonanceVoices()    const noexcept { return resonance_ ? resonance_->maxVoices() : 0; }

    // -- Master peak meter (GUI; atomic) --
    // Vraci aktualni peak |out| po master_gain, s decay ~100ms mezi bloky.
    // Cteni je lock-free, GUI muze vzorkovat libovolne casto.
    float masterPeakL() const noexcept { return master_peak_l_.load(std::memory_order_relaxed); }
    float masterPeakR() const noexcept { return master_peak_r_.load(std::memory_order_relaxed); }

    // Pocitadlo zapocatych audio bloku (diagnostika + reload/recache handshake,
    // viz waitForAudioQuiesce). Atomic, thread-safe cteni odkudkoli.
    uint64_t blockEpoch() const noexcept {
        return block_epoch_.load(std::memory_order_seq_cst);
    }

    // -- DSP load meter (GUI; atomic) --
    // Peak-hold zatizeni audio threadu = cas renderu / perioda bloku. 1.0 = na
    // hranici deadline, > 1.0 = blok se nestihl (dropout riziko). Decay ~0.5 s.
    float dspLoadPeak() const noexcept { return dsp_load_peak_.load(std::memory_order_relaxed); }
    // True kdyz overload (load >= 1.0) nastal pred mene nez `ms` ms. Vzor
    // noteOnRecent — GUI cervena dlazba.
    bool  overloadRecent(float ms) const noexcept;

    // -- DSP chain (GUI ovlada stage; audio thread vola process v processBlock) --
    dsp::DspChain& dspChain() noexcept { return dsp_; }

private:
    // Prepocita StreamEngine refill threshold dle aktualniho block_size.
    void recomputeRefillThreshold() noexcept;

    // Pocka (z non-RT threadu), az audio thread ZAPOCNE aspon min_epochs
    // novych bloku — tj. pripadny in-flight processBlock dobehl a nasledujici
    // blok uz cetl aktualni atomic flagy (bank_loading_/fade request).
    // Timeout kryje zastavene audio (testy, odpojene zarizeni) — pak je
    // mutace sdileneho stavu trivialne bezpecna.
    void waitForAudioQuiesce(int min_epochs, int timeout_ms) noexcept;

    // Half-pedal continuous release scaling (spec 5.4). Vraci skalovany
    // release_ms podle aktualniho cc64_ (CC0 → ×1, CC127 → ~×20).
    float scaledReleaseMs() const;

    EngineConfig                      cfg_;
    Bank                              bank_;
    float bank_peak_rms_min_db_ = -60.f;
    float bank_peak_rms_max_db_ =   0.f;
    std::unique_ptr<VoicePool>        pool_;
    RoundRobinState                   rr_;
    MidiQueue                         midi_q_;
    NoteHoldTracker                   hold_;   // per-pitch maska kanalu (audio thread)
    std::unique_ptr<StreamEngine>     stream_main_;
    std::unique_ptr<StreamEngine>     stream_resonance_;
    PedalState                        pedal_;
    std::unique_ptr<ResonanceEngine>  resonance_;
    std::atomic<float>                master_gain_{1.0f};
    dsp::DspChain                     dsp_;
    // Master peak meter — psano z audio threadu (processBlock), cteno z GUI.
    std::atomic<float>                master_peak_l_{0.f};
    std::atomic<float>                master_peak_r_{0.f};
    // DSP load meter — psano z audio threadu (processBlock), cteno z GUI.
    std::atomic<float>                dsp_load_peak_{0.f};
    std::atomic<uint64_t>             last_overload_us_{0};
    // Bank reload guard. Pokud true, processBlock vraci ticho a preskoci
    // veskery render / drain — chrani pred race s loadBank na non-RT threadu.
    std::atomic<bool>                 bank_loading_{false};
    // Pocitadlo zapocatych bloku (inkrement na zacatku processBlock).
    std::atomic<uint64_t>             block_epoch_{0};
    // Indikator note-on/off blikani: timestamp (steady_clock micros) posledniho
    // eventu. Psano z MIDI/GUI threadu pri noteOn/noteOff, cteno z GUI.
    std::atomic<uint64_t>             last_note_on_us_{0};
    std::atomic<uint64_t>             last_note_off_us_{0};
    int                  dbg_reso_count_ = -1;  // audio-thread only (DIAG zmeny poctu rezonanci)
    std::thread          recache_thread_;
    // Stav rebuilu pod mutexem (vsechny strany jsou non-RT: GUI + bg thread).
    // Drive dvojice atomiku → lost-update okno (pending nastaveny tesne po
    // exchange workeru se uz nevyzvedl). Review F-nizka / 1.4c.
    mutable std::mutex   recache_mtx_;
    bool                 recache_running_ = false;
    bool                 recache_pending_ = false;
    std::atomic<float>   recache_target_{-30.f};   // zadany cil (cte bg thread; bez torn readu cfg_)
    bool                              initialized_ = false;
};

} // namespace ithaca
