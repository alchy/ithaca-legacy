// engine/engine.cpp — viz engine.h.
#include "engine.h"

#include "sample/sample_store.h"
#include "util/log.h"

#include <cmath>

namespace ithaca {

Engine::Engine() {}
Engine::~Engine() {}

bool Engine::init(const EngineConfig& cfg) {
    cfg_ = cfg;
    pool_ = std::make_unique<VoicePool>(cfg.max_voices);
    master_gain_.store(cfg.master_gain, std::memory_order_relaxed);
    initialized_ = true;
    return true;
}

bool Engine::loadBank(const std::string& dir) {
    auto& L = log::Logger::default_();
    bank_ = loadLegacyBank(dir, L, /*cache_budget_mb=*/0,
                           cfg_.midi_from, cfg_.midi_to, cfg_.preload_ms);
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

void Engine::processBlock(float* out_l, float* out_r, int n_samples) noexcept {
    if (!initialized_ || !pool_) return;
    const float sr = (float)cfg_.sample_rate;

    // 1. Vyprazdni MIDI frontu (audio thread) → akce do voice poolu.
    MidiEvent e;
    while (midi_q_.pop(e)) {
        switch (e.type) {
            case MidiEvent::NoteOn: {
                VoiceSpec vs = selectVoice(bank_, e.data1, e.data2, rr_);
                if (vs.asset)
                    pool_->noteOn(e.data1, vs, sr, cfg_.keyboard_spread);
                break;
            }
            case MidiEvent::NoteOff:
                pool_->noteOff(e.data1, cfg_.release_ms, sr);
                break;
            case MidiEvent::AllNotesOff:
                pool_->allNotesOff(cfg_.release_ms, sr);
                break;
            case MidiEvent::Sustain:
                break;   // pedal je faze 5
        }
    }

    // 2. Render hlasu (caller buffery vynuloval).
    pool_->processBlock(out_l, out_r, n_samples, sr);

    // 3. Master gain post-mix.
    float g = master_gain_.load(std::memory_order_relaxed);
    if (std::fabs(g - 1.f) > 0.001f)
        for (int i = 0; i < n_samples; ++i) { out_l[i] *= g; out_r[i] *= g; }
}

} // namespace ithaca
