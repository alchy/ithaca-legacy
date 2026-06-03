// engine/engine.cpp — viz engine.h.
#include "engine.h"

#include "sample/sample_store.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace ithaca {

Engine::Engine() {}
Engine::~Engine() {
    if (recache_thread_.joinable()) recache_thread_.join();
    if (stream_main_)      stream_main_->stop();
    if (stream_resonance_) stream_resonance_->stop();
}

bool Engine::init(const EngineConfig& cfg) {
    cfg_ = cfg;
    pool_   = std::make_unique<VoicePool>(cfg.max_voices);
    // Oddelene streaming pooly: hlavni hlasy vs rezonance (izolace ringu + workeru,
    // aby rezonancni burst pod pedalem nevyhladovel hlavni hlasy). Kazdy pool ma
    // vlastni ringy + workery + frontu.
    const int main_rings = (cfg.num_rings >= cfg.max_voices) ? cfg.num_rings : cfg.max_voices;
    const int res_rings  = (cfg.resonance_num_rings >= cfg.max_resonance_voices)
                         ? cfg.resonance_num_rings : cfg.max_resonance_voices;
    cfg_.num_rings = main_rings;
    cfg_.resonance_num_rings = res_rings;

    stream_main_ = std::make_unique<StreamEngine>(main_rings, cfg.ring_capacity_frames,
                                                  cfg.stream_threads);
    stream_resonance_ = std::make_unique<StreamEngine>(res_rings, cfg.ring_capacity_frames,
                                                       cfg.resonance_stream_threads);
    pool_->setStreamEngine(stream_main_.get());
    recomputeRefillThreshold();
    stream_main_->start();
    stream_resonance_->start();

    resonance_ = std::make_unique<ResonanceEngine>(cfg.max_resonance_voices);
    resonance_->setGainDb(cfg.resonance_gain_db);
    resonance_->setLayerTargetDb(cfg.resonance_layer_db);
    resonance_->setEnabled(cfg.resonance_enabled);
    resonance_->setStreamEngine(stream_resonance_.get());
    resonance_->setExciteDecayTimeMs(cfg.excite_decay_ms, cfg.block_size,
                                     (float)cfg.sample_rate);
    master_gain_.store(cfg.master_gain, std::memory_order_relaxed);
    dsp_.prepare((float)cfg.sample_rate, cfg.block_size);
    initialized_ = true;
    return true;
}

bool Engine::loadBank(const std::string& dir) {
    auto& L = log::Logger::default_();
    // ::ithaca::loadBank (volna funkce) — kvalifikovat, jinak by se nasel
    // clen Engine::loadBank (skryva volnou funkci ve scope metody).
    bank_ = ithaca::loadBank(dir, L, /*cache_budget_mb=*/0,
                           cfg_.midi_from, cfg_.midi_to,
                           cfg_.preload_ms, cfg_.resonance_window_ms);
    if (bank_.loaded_samples <= 0) return false;
    // Cache min/max peak RMS napric vsemi velocity sloty (pro GUI slider rozsah).
    {
        float mn = 1e30f, mx = -1e30f;
        for (int n = 0; n < 128; ++n)
            for (const auto& s : bank_.notes[n].slots) {
                if (s.rms_db < mn) mn = s.rms_db;
                if (s.rms_db > mx) mx = s.rms_db;
            }
        if (mn <= mx) { bank_peak_rms_min_db_ = mn; bank_peak_rms_max_db_ = mx; }
    }
    // Postav RAM cache rezonance pro per-notu cilovou vrstvu + zapis ready flagy.
    {
        auto ready = ithaca::buildResonanceCache(bank_, cfg_.resonance_layer_db,
                                                 cfg_.resonance_window_ms, L);
        if (resonance_) resonance_->setCacheReady(ready);
    }
    return true;
}

bool Engine::reloadBank(const std::string& dir) {
    // Graceful reload — viz engine.h dokumentace. Volat jen z non-RT threadu.
    // 1) Drain: posli AllNotesOff do MIDI fronty. Audio thread ji vyzvedne
    //    pristi blok a spusti release ramp na vsech aktivnich voicech.
    allNotesOff();
    // 2) Pockej ~50 ms aby release dobehl. Pri release_ms ~200 ms je voice
    //    porad v "releasing" stavu, ale uroven uz hodne klesla; pripadne
    //    cvaknuti pri prepnuti banky je akceptovatelne (uzivatelska akce).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // 3) Zapni bank_loading_ flag. processBlock to uvidi a vrati ticho;
    //    pripadny prave bezici processBlock dobehne s puvodnim bank_.
    bank_loading_.store(true, std::memory_order_release);
    // 4) Pockej ~10 ms aby in-flight processBlock dobehl. Audio bloky maji
    //    typicky pod 6 ms (256 vz / 48k), 10 ms je bezpecna rezerva.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // Join pripadny bezici recache thread — pristupuje k bank_/cfg_, musi
    // dobehnout DRIV nez loadBank prepise banku (jinak race / use-after-free).
    if (recache_thread_.joinable()) recache_thread_.join();
    recache_running_.store(false, std::memory_order_release);
    // 5) TVRDE zastav vsechny hlasy DRIV nez uvolnime bank_ — jinak by Voice/
    //    ResonanceVoice drzely const MicLayer*/SampleAsset* do uvolnene pameti
    //    stare banky (release ~200 ms >> 60 ms pauza) => use-after-free.
    if (pool_)      pool_->reset();
    if (resonance_) resonance_->reset();
    // 6) Slow disk I/O — bank_ se prepise, ale audio thread to nesahne.
    const bool ok = loadBank(dir);
    // 7) Pust audio thread zpet.
    bank_loading_.store(false, std::memory_order_release);
    return ok;
}

namespace {
// Monotonni cas v mikrosekundach (pro note-on/off blikani indikatoru).
uint64_t nowMicros() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}
} // namespace

void Engine::noteOn(int midi, int velocity, int channel) {
    if (velocity <= 0) { noteOff(midi, channel); return; }
    last_note_on_us_.store(nowMicros(), std::memory_order_relaxed);
    if (!midi_q_.push({MidiEvent::NoteOn, (uint8_t)midi, (uint8_t)velocity,
                       (uint8_t)channel}))
        log::Logger::default_().log("midi", log::Severity::Warning,
            "MIDI fronta plna — NoteOn midi=%d ZAHOZEN", midi);
}
void Engine::noteOff(int midi, int channel) {
    last_note_off_us_.store(nowMicros(), std::memory_order_relaxed);
    if (!midi_q_.push({MidiEvent::NoteOff, (uint8_t)midi, 0, (uint8_t)channel}))
        log::Logger::default_().log("midi", log::Severity::Warning,
            "MIDI fronta plna — NoteOff midi=%d ZAHOZEN", midi);
}

bool Engine::noteOnRecent(float ms) const noexcept {
    const uint64_t t = last_note_on_us_.load(std::memory_order_relaxed);
    if (t == 0) return false;
    return (nowMicros() - t) < (uint64_t)(ms * 1000.f);
}
bool Engine::noteOffRecent(float ms) const noexcept {
    const uint64_t t = last_note_off_us_.load(std::memory_order_relaxed);
    if (t == 0) return false;
    return (nowMicros() - t) < (uint64_t)(ms * 1000.f);
}
bool Engine::overloadRecent(float ms) const noexcept {
    const uint64_t t = last_overload_us_.load(std::memory_order_relaxed);
    if (t == 0) return false;
    return (nowMicros() - t) < (uint64_t)(ms * 1000.f);
}
void Engine::allNotesOff() {
    midi_q_.push({MidiEvent::AllNotesOff, 0, 0});
}
void Engine::sustainPedal(uint8_t cc) {
    midi_q_.push({MidiEvent::Sustain, cc, 0});
}

void Engine::processBlock(float* out_l, float* out_r, int n_samples) noexcept {
    if (!initialized_ || !pool_) return;
    // Bank reload v progressu? Vrat ticho — nesahej na bank_ (race s reloadBank
    // na non-RT threadu) a preskoc drain MIDI / voice render. Resetneme i peak
    // metr aby GUI videlo, ze nic neteche. Caller buffery nemusi mit vynulovane,
    // proto je explicitne zerujeme (kontrakt: vystup po processBlock je validni).
    if (bank_loading_.load(std::memory_order_acquire)) {
        std::memset(out_l, 0, sizeof(float) * (size_t)n_samples);
        std::memset(out_r, 0, sizeof(float) * (size_t)n_samples);
        master_peak_l_.store(0.f, std::memory_order_relaxed);
        master_peak_r_.store(0.f, std::memory_order_relaxed);
        return;
    }
    const uint64_t block_t0 = nowMicros();
    const float sr = (float)cfg_.sample_rate;

    // 1. Vyprazdni MIDI frontu (audio thread) → akce do voice poolu + rezonance.
    // Drain order per spec 5.5.1: pravidlo B PRED voice noteOn, onPlayedNoteOn
    // PO voice noteOn (aby eligibility videla novy main voice).
    MidiEvent e;
    while (midi_q_.pop(e)) {
        switch (e.type) {
            case MidiEvent::NoteOn: {
                int m = (int)e.data1;
                int v = (int)e.data2;
                int ch = (int)e.channel;
                if (v == 0) {
                    // MIDI konvence: NoteOn s vel=0 = NoteOff. Stejna cesta jako
                    // 0x80 vc. cross-channel hold (viz NoteOff case).
                    if (hold_.noteOff(m, ch)) {
                        pedal_.noteOff(m);
                        pool_->noteOffWithPedal(m, pedal_, scaledReleaseMs(), sr);
                    }
                } else {
                    // Cross-channel hold: tlumitko (pedal_.noteOn) zapnout jen na
                    // PRVNI drzitel vysky m. Voice ale (re)strike vzdy — opakovany
                    // uder / re-artikulace stejne noty ma znit.
                    const bool first = hold_.noteOn(m, ch);
                    if (first) pedal_.noteOn(m);
                    // Pravidlo B PRED voice noteOn: pokud N uz rezonuje,
                    // zafade rezonanci.
                    resonance_->onSelfNoteOn(m, sr);
                    VoiceSpec spec = selectVoice(bank_, m, v, rr_);
                    // DIAG: log kazdy NoteOn na drainu — m, vel, kanal, jestli se
                    // nasel asset (spec.asset==NULL = nota nema namapovany sample
                    // → tise se zahodi), a kolik hlasu je aktivnich (steal).
                    log::Logger::default_().log("midi_on", log::Severity::Info,
                        "noteOn midi=%d vel=%d ch=%d first=%d asset=%s active_voices=%d",
                        m, v, ch, (int)first, spec.asset ? "yes" : "NULL",
                        pool_->activeCount());
                    if (spec.asset)
                        pool_->noteOn(m, spec, sr, cfg_.keyboard_spread, &pedal_);
                    // PO voice noteOn: spusti rezonance harmonicky pribuznych
                    // strun (eligibility uz vidi novy active main voice).
                    resonance_->onPlayedNoteOn(m, v, bank_, *pool_, pedal_, sr);
                }
                break;
            }
            case MidiEvent::NoteOff: {
                int m = (int)e.data1;
                int ch = (int)e.channel;
                float rms = scaledReleaseMs();
                // Cross-channel hold: release teprve kdyz pusti POSLEDNI kanal,
                // ktery vysku m drzel. Drzi-li ji jeste jiny kanal (druha ruka v
                // Synthesia), note-off se ignoruje a nota zni dal.
                const bool last = hold_.noteOff(m, ch);
                const bool sustained = pedal_.isUndamped(m);
                log::Logger::default_().log("midi_off", log::Severity::Info,
                    "noteOff midi=%d ch=%d last=%d release_ms=%.0f cc64=%d sustained=%d",
                    m, ch, (int)last, rms, (int)pedal_.sustainCC(), (int)sustained);
                if (!last) break;   // jiny kanal porad drzi → neni release
                // Pedal nejdriv noteOff (snizi held_), pak voice off s pedalem
                // — VoicePool si overi pedal.isUndamped(m): pokud pedal drzi
                // strunu, sample bude hrat dal jako pending_release; jinak
                // normalni release ramp.
                pedal_.noteOff(m);
                pool_->noteOffWithPedal(m, pedal_, rms, sr);
                break;
            }
            case MidiEvent::Sustain: {
                // CC64 jako spojita hodnota; PedalState prepocita damping_[128].
                // Resonance se prizpusobi PER-BLOK ve resonance_->processBlock().
                const bool was_down = pedal_.isPedalDown();
                log::Logger::default_().log("midi_cc", log::Severity::Info,
                    "Sustain CC64=%d", (int)e.data1);
                pedal_.setSustainCC(e.data1);
                const bool now_down = pedal_.isPedalDown();
                // Pri prechodu DOWN → UP: aplikuj release na vsechny pending
                // hlasy, jejichz nota neni aktualne drzena, a STRIKTNE muteni
                // rezonance na nedrzenych strunach (rychly fadeOut). Drive se
                // rezonance spolehala jen na per-blok target (pomaly 30ms ramp
                // az pristi blok) → nektere hlasy slysitelne dohravaly.
                if (was_down && !now_down) {
                    pool_->releasePendingNotes(pedal_, scaledReleaseMs(), sr);
                    const int faded = resonance_->dampOnPedalUp(pedal_, sr);
                    log::Logger::default_().log("resonance", log::Severity::Info,
                        "pedal UP → mute rezonance: faded=%d aktivnich_zbylo=%d",
                        faded, resonance_->activeCount());
                }
                break;
            }
            case MidiEvent::AllNotesOff: {
                log::Logger::default_().log("midi_off", log::Severity::Info,
                    "AllNotesOff");
                hold_.allNotesOff();
                pedal_.allNotesOff();
                pool_->allNotesOff(cfg_.release_ms, sr);
                break;
            }
        }
    }

    // 2. Render hlasu (caller buffery vynuloval).
    pool_->processBlock(out_l, out_r, n_samples, sr);
    // 2b. Pricti sympatickou rezonanci do L/R (sleduje pedal per-blok).
    if (resonance_) resonance_->processBlock(out_l, out_r, n_samples, pedal_);

    // 3. Master gain post-mix.
    float g = master_gain_.load(std::memory_order_relaxed);
    if (std::fabs(g - 1.f) > 0.001f)
        for (int i = 0; i < n_samples; ++i) { out_l[i] *= g; out_r[i] *= g; }

    // 3b. DSP chain (CONVOLVER -> AGC -> ENHANCER -> Limiter). Disabled stage = no-op.
    dsp_.process(out_l, out_r, n_samples);

    // 4. Master peak meter pro GUI (decay ~100 ms; non-blocking atomic).
    // Pocitame ABS peak per blok a kombinujeme s predchozim peak * decay.
    // Decay vzorec: exp(-n_samples / (tau_s * sample_rate)). Pro tau=0.1 s
    // a 48k/256 ≈ 0.948 per blok → ~150 ms na poklesni z 1.0 na 0.1.
    float peak_l = 0.f, peak_r = 0.f;
    for (int i = 0; i < n_samples; ++i) {
        const float al = std::fabs(out_l[i]);
        const float ar = std::fabs(out_r[i]);
        if (al > peak_l) peak_l = al;
        if (ar > peak_r) peak_r = ar;
    }
    const float decay = std::exp(-(float)n_samples / (0.1f * sr));
    const float cur_l = master_peak_l_.load(std::memory_order_relaxed);
    const float cur_r = master_peak_r_.load(std::memory_order_relaxed);
    const float new_l = (peak_l > cur_l * decay) ? peak_l : cur_l * decay;
    const float new_r = (peak_r > cur_r * decay) ? peak_r : cur_r * decay;
    master_peak_l_.store(new_l, std::memory_order_relaxed);
    master_peak_r_.store(new_r, std::memory_order_relaxed);

    // 5. DSP load meter: cas renderu / perioda bloku. Peak-hold s decay ~0.5 s,
    // aby cislo na liste bylo citelne. Overload (load >= 1.0 = minul deadline)
    // orazitkujeme pro cervene blikani v GUI.
    const uint64_t dt_us     = nowMicros() - block_t0;
    const uint64_t period_us = cfg_.sample_rate > 0
        ? (uint64_t)n_samples * 1000000ull / (uint64_t)cfg_.sample_rate : 0;
    const float    load      = period_us > 0 ? (float)dt_us / (float)period_us : 0.f;
    if (load >= 1.0f)
        last_overload_us_.store(nowMicros(), std::memory_order_relaxed);
    const float load_decay = std::exp(-(float)n_samples / (0.5f * sr));
    const float cur_load   = dsp_load_peak_.load(std::memory_order_relaxed);
    dsp_load_peak_.store((load > cur_load * load_decay) ? load : cur_load * load_decay,
                         std::memory_order_relaxed);
}

int Engine::setBlockSize(int new_block_size) noexcept {
    // Clamp do rozumnych mezi. POZN.: caller (CLI/GUI) je zodpovedny za
    // restart audio device s novym block_size — Engine sam audio device
    // nedrzi (drzi ho ithaca-cli / pluginovy host).
    if (new_block_size < 32)    new_block_size = 32;
    if (new_block_size > 8192)  new_block_size = 8192;
    cfg_.block_size = new_block_size;
    recomputeRefillThreshold();
    // Faze 5: decay_per_block konstanta zavisi na block_size — drz ji v sync.
    if (resonance_)
        resonance_->setExciteDecayTimeMs(cfg_.excite_decay_ms, cfg_.block_size,
                                         (float)cfg_.sample_rate);
    dsp_.prepare((float)cfg_.sample_rate, cfg_.block_size);
    return new_block_size;
}

void Engine::recomputeRefillThreshold() noexcept {
    auto setFor = [this](StreamEngine* se) {
        if (!se) return;
        const int cap = se->capacityFrames();
        int thr = (std::max)(cap / 2, cfg_.block_size * 4);
        if (thr > cap - 64) thr = cap - 64;
        if (thr < 0) thr = 0;
        se->setRefillThresholdFrames(thr);
    };
    setFor(stream_main_.get());
    setFor(stream_resonance_.get());
}

// ----- Diagnostika (GUI/monitor) -----------------------------------------
// Vsechny gettery jsou thread-safe pro cteni z GUI threadu, pokud podkladove
// stavy ctou atomic loads nebo jsou "ramcove" konzistentni (snapshot bez
// locku, hodnota muze byt o jeden mimo). Engine sam zadny lock nedrzi.

int Engine::resonanceVoices() const noexcept {
    // ResonanceEngine::activeCount uz dela atomic load pres pool resonance
    // voicu (faze 5). Pri Engine pred init je resonance_ nullptr.
    return resonance_ ? resonance_->activeCount() : 0;
}

int Engine::numRingsUsed() const noexcept {
    int n = 0;
    if (stream_main_)      n += stream_main_->numRingsUsed();
    if (stream_resonance_) n += stream_resonance_->numRingsUsed();
    return n;
}

uint8_t Engine::pedalCC() const noexcept {
    // PedalState::sustainCC vraci aktualni CC64 (8-bit getter, neatomic).
    // Cteni 1 bytu je trivialne atomic na vsech rozumnych platformach;
    // GUI to ctе ramcove, tolerujeme prechodne hodnoty pri zmene.
    return pedal_.sustainCC();
}

void Engine::activeMidiNotes(bool out[128]) const noexcept {
    // Iteruj pres voice pool, pro kazdy active() zaznamenej jeho midi.
    // POZN.: Voice::active() neni atomic, ale jednoduchy bool — race-free
    // pri readeru, ktery toleruje "lehky lag". Pri pool_=nullptr (engine
    // pred init) vratime same false.
    std::memset(out, 0, 128 * sizeof(bool));
    if (!pool_) return;
    for (const auto& v : pool_->voicesView()) {
        if (v.active() && v.midi() >= 0 && v.midi() < 128) {
            out[v.midi()] = true;
        }
    }
}

void Engine::resonatingMidiNotes(bool out[128]) const noexcept {
    std::memset(out, 0, 128 * sizeof(bool));
    if (!resonance_) return;
    for (int n = 0; n < 128; ++n)
        if (resonance_->isResonating(n)) out[n] = true;
}

float Engine::currentGainFor(int midi) const noexcept {
    // Max currentLevel pres vsechny voicy hrajici notu `midi`. Pouzije se
    // pro alfu klavesy v GUI (1.0 = svetla, 0.0 = zhasla). Voice
    // currentLevel je tez "lehky lag" snapshot (ne-atomic float).
    if (!pool_ || midi < 0 || midi >= 128) return 0.f;
    float g = 0.f;
    for (const auto& v : pool_->voicesView()) {
        if (v.active() && v.midi() == midi) {
            const float l = v.currentLevel();
            if (l > g) g = l;
        }
    }
    return g;
}

// ----- Runtime settery (GUI) ---------------------------------------------
// Voláno z GUI threadu; audio thread cte cfg_.release_ms ve scaledReleaseMs().
// cfg_.release_ms je obycejny float (NE atomic). Zápis z GUI a soubezne cteni
// z audio threadu zde NENI race-critical: float zápis je atomic na x86/arm
// (4-byte aligned mov), a pripadny "rozjety" cyklus pred/po zmene daje hrac
// stejne neslysi — release scaling je hodne hladky. Pridani atomic by bylo
// over-engineering pro hodnotu, ktera se meni v GUI tempo (jednotky Hz).

void Engine::setReleaseMs(float ms) noexcept {
    if (ms < 1.f) ms = 1.f;
    if (ms > 60000.f) ms = 60000.f;
    cfg_.release_ms = ms;
}

void Engine::setResonanceGainDb(float db) noexcept {
    if (resonance_) resonance_->setGainDb(db);
}
void Engine::setResonanceLayerDb(float db) noexcept {
    if (resonance_) resonance_->setLayerTargetDb(db);
}
void Engine::setResonanceEnabled(bool on) noexcept {
    if (resonance_) resonance_->setEnabled(on);
}

void Engine::rebuildResonanceCache(float target_db) noexcept {
    if (!resonance_) return;
    cfg_.resonance_layer_db = target_db;            // GUI-thread zapis (pamet aktualniho cile)
    recache_target_.store(target_db, std::memory_order_release);  // cil pro bg thread (bez torn read)
    resonance_->setLayerTargetDb(target_db);        // nove spawny vyberou novou vrstvu hned
    // VZDY (i pri coalesce) sraz ready flagy → nove hlasy stream mod, a fadene aktivni
    // cache-mod hlasy. Tim odpada okno, kdy by ready=true ukazoval na jeste nepostavenou
    // novou cilovou vrstvu (jinak benigni — hlas by degradoval na ring stream — ale takhle
    // je stav konzistentni). RT-safety: ResonanceVoice re-fetchuje preload_resonance kazdy
    // blok; fade + 120ms sleep zaruci, ze cache-mod hlas je !active driv, nez bg realokuje.
    resonance_->clearCacheReady();
    resonance_->requestRecacheFade();
    // Pokud uz rebuild bezi, jen oznac pending (bezici thread znovu prebuduje na novy cil).
    if (recache_running_.exchange(true, std::memory_order_acq_rel)) {
        recache_has_pending_.store(true, std::memory_order_release);
        return;
    }
    if (recache_thread_.joinable()) recache_thread_.join();   // predchozi uz dobehl
    recache_thread_ = std::thread([this]() {
        for (;;) {
            const float t = recache_target_.load(std::memory_order_acquire);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));  // dobeh fade
            auto ready = ithaca::buildResonanceCache(
                bank_, t, cfg_.resonance_window_ms, log::Logger::default_());
            if (resonance_) resonance_->setCacheReady(ready);
            // Prisel mezitim novy cil? (GUI uz srazil ready + fade) → prebuduj znovu.
            if (recache_has_pending_.exchange(false, std::memory_order_acquire)) continue;
            break;
        }
        recache_running_.store(false, std::memory_order_release);
    });
}

void Engine::setExciteDecayMs(float ms) noexcept {
    // ResonanceEngine::setExciteDecayTimeMs ignoruje tau_ms<=0 (no-op);
    // sub-ms hodnoty projedou a daji decay_per_block_~0.
    if (resonance_) resonance_->setExciteDecayTimeMs(ms, cfg_.block_size,
                                                     (float)cfg_.sample_rate);
}

float Engine::scaledReleaseMs() const {
    // Half-pedal continuous release scaling per spec 5.4:
    //   CC 0   → release_ms × 1.0   (rychly fade)
    //   CC 64  → release_ms × ~4.0  (pomaly s pedalem)
    //   CC 127 → release_ms × ~20.0 (skoro hold)
    const float t  = (float)pedal_.sustainCC() / 127.f;
    const float kf = std::exp(t * std::log(20.f));
    return cfg_.release_ms * kf;
}

} // namespace ithaca
