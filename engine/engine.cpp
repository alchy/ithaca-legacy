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
    if (stream_) stream_->stop();
}

bool Engine::init(const EngineConfig& cfg) {
    cfg_ = cfg;
    pool_   = std::make_unique<VoicePool>(cfg.max_voices);
    // Bezpecnostni klamr: ring pool MUSI kryt plnou polyfonii (hlavni + rezonance),
    // jinak hraje pri pedalu se rezonanci docasi k `acquireRing → nullptr` a hlas
    // utichne po preload_head (fullyloaded_past_head ring=no). Vetsi nez cfg jen
    // zvedneme; mensi nikdy.
    const int rings_min = cfg.max_voices + cfg.max_resonance_voices;
    const int rings_actual = (cfg.num_rings >= rings_min) ? cfg.num_rings : rings_min;
    if (cfg.num_rings < rings_min) {
        log::Logger::default_().log("stream", log::Severity::Info,
            "num_rings %d < max_voices+max_resonance=%d, zvysuji na %d",
            cfg.num_rings, rings_min, rings_actual);
    }
    cfg_.num_rings = rings_actual;
    stream_ = std::make_unique<StreamEngine>(rings_actual, cfg.ring_capacity_frames,
                                             cfg.stream_threads);
    pool_->setStreamEngine(stream_.get());
    recomputeRefillThreshold();
    stream_->start();
    // Faze 5: sympaticka rezonance — sdileny stream engine, vlastni hlasy.
    resonance_ = std::make_unique<ResonanceEngine>(cfg.max_resonance_voices);
    resonance_->setStrength(cfg.resonance_strength);
    resonance_->setStreamEngine(stream_.get());
    resonance_->setExciteDecayTimeMs(cfg.excite_decay_ms, cfg.block_size,
                                     (float)cfg.sample_rate);
    master_gain_.store(cfg.master_gain, std::memory_order_relaxed);
    initialized_ = true;
    return true;
}

bool Engine::loadBank(const std::string& dir) {
    auto& L = log::Logger::default_();
    bank_ = loadLegacyBank(dir, L, /*cache_budget_mb=*/0,
                           cfg_.midi_from, cfg_.midi_to,
                           cfg_.preload_ms, cfg_.resonance_window_ms);
    return bank_.loaded_samples > 0;
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

void Engine::noteOn(int midi, int velocity) {
    if (velocity <= 0) { noteOff(midi); return; }
    last_note_on_us_.store(nowMicros(), std::memory_order_relaxed);
    midi_q_.push({MidiEvent::NoteOn, (uint8_t)midi, (uint8_t)velocity});
}
void Engine::noteOff(int midi) {
    last_note_off_us_.store(nowMicros(), std::memory_order_relaxed);
    midi_q_.push({MidiEvent::NoteOff, (uint8_t)midi, 0});
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
                if (v == 0) {
                    // MIDI konvence: NoteOn s vel=0 = NoteOff.
                    pedal_.noteOff(m);
                    pool_->noteOff(m, scaledReleaseMs(), sr);
                } else {
                    pedal_.noteOn(m);
                    // Pravidlo B PRED voice noteOn: pokud N uz rezonuje,
                    // zafade rezonanci.
                    resonance_->onSelfNoteOn(m, sr);
                    VoiceSpec spec = selectVoice(bank_, m, v, rr_);
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
                float rms = scaledReleaseMs();
                const bool sustained = pedal_.isUndamped(m);
                log::Logger::default_().log("midi_off", log::Severity::Info,
                    "noteOff midi=%d release_ms=%.0f cc64=%d sustained=%d",
                    m, rms, (int)pedal_.sustainCC(), (int)sustained);
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
                // hlasy, jejichz nota neni aktualne drzena.
                if (was_down && !now_down) {
                    pool_->releasePendingNotes(pedal_, scaledReleaseMs(), sr);
                }
                break;
            }
            case MidiEvent::AllNotesOff: {
                log::Logger::default_().log("midi_off", log::Severity::Info,
                    "AllNotesOff");
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
    return new_block_size;
}

void Engine::recomputeRefillThreshold() noexcept {
    if (!stream_) return;
    const int cap = stream_->capacityFrames();
    // Pravidlo: aspon pulka ringu NEBO 4 audio bloky napred, podle toho co je vetsi.
    int thr = (std::max)(cap / 2, cfg_.block_size * 4);
    // Sanitace: prah nesmi prekrocit kapacitu ringu (jinak by Voice zadal
    // request kazdy blok → fronta pretece).
    if (thr > cap - 64) thr = cap - 64;
    if (thr < 0) thr = 0;
    stream_->setRefillThresholdFrames(thr);
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
    // StreamEngine::numRingsUsed dela snapshot pres rings_[*].in_use_ atomic.
    return stream_ ? stream_->numRingsUsed() : 0;
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

void Engine::setResonanceStrength(float s) noexcept {
    // ResonanceEngine::setStrength si sama clampuje 0..1 a uklada atomic.
    if (resonance_) resonance_->setStrength(s);
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
