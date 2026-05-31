// engine/engine.cpp — viz engine.h.
#include "engine.h"

#include "sample/sample_store.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>

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
    stream_ = std::make_unique<StreamEngine>(rings_actual, cfg.ring_capacity_frames);
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

void Engine::noteOn(int midi, int velocity) {
    if (velocity <= 0) { noteOff(midi); return; }
    midi_q_.push({MidiEvent::NoteOn, (uint8_t)midi, (uint8_t)velocity});
}
void Engine::noteOff(int midi) {
    midi_q_.push({MidiEvent::NoteOff, (uint8_t)midi, 0});
}
void Engine::allNotesOff() {
    midi_q_.push({MidiEvent::AllNotesOff, 0, 0});
}
void Engine::sustainPedal(uint8_t cc) {
    midi_q_.push({MidiEvent::Sustain, cc, 0});
}

void Engine::processBlock(float* out_l, float* out_r, int n_samples) noexcept {
    if (!initialized_ || !pool_) return;
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
