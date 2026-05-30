// engine/voice/voice_pool.cpp — viz voice_pool.h.
#include "voice/voice_pool.h"

#include <algorithm>
#include <cmath>

namespace ithaca {

namespace {
// Pan z MIDI noty: stred + rozprostreni dle vzdalenosti od C4 (midi 60).
void panForNote(int midi, float spread, float& pan_l, float& pan_r) {
    constexpr float kPi = 3.14159265f;
    float angle = (kPi / 4.f) + ((float)midi - 64.5f) / 87.f * spread * 0.5f;
    pan_l = std::cos(angle);
    pan_r = std::sin(angle);
}
} // namespace

VoicePool::VoicePool(int pool_size) {
    int n = (std::max)(1, (std::min)(pool_size, kMaxPoolSize));
    voices_.resize((size_t)n);
}

int VoicePool::findSlot() {
    // 1. Volny slot.
    for (int i = 0; i < (int)voices_.size(); ++i)
        if (!voices_[i].active()) return i;
    // 2. Kradez: nejtissi hlas z celeho poolu (spec rozhodnuti).
    int best = 0;
    float best_level = voices_[0].currentLevel();
    for (int i = 1; i < (int)voices_.size(); ++i) {
        float lvl = voices_[i].currentLevel();
        if (lvl < best_level) { best_level = lvl; best = i; }
    }
    return best;
}

void VoicePool::noteOn(int midi, const VoiceSpec& spec, float engine_sr,
                       float keyboard_spread) {
    if (!spec.asset) return;

    // Retrigger: pokud uz nektery hlas hraje tuto notu, damp ho (click-free).
    for (auto& v : voices_)
        if (v.active() && v.midi() == midi && !v.releasing())
            v.prepareDamp(engine_sr);

    int slot = findSlot();
    Voice& v = voices_[slot];
    // Kdyz krademe aktivni hlas, damp i jeho (jiny ton) → bez lupnuti.
    if (v.active()) v.prepareDamp(engine_sr);

    float pl, pr;
    panForNote(midi, keyboard_spread, pl, pr);
    v.start(spec.asset, spec.pitch_ratio, spec.vel_gain, pl, pr, engine_sr);
    v.setMidi(midi);
}

void VoicePool::noteOff(int midi, float release_ms, float engine_sr) {
    for (auto& v : voices_)
        if (v.active() && v.midi() == midi && !v.releasing())
            v.release(release_ms, engine_sr);
}

void VoicePool::allNotesOff(float release_ms, float engine_sr) {
    for (auto& v : voices_)
        if (v.active() && !v.releasing())
            v.release(release_ms, engine_sr);
}

bool VoicePool::processBlock(float* out_l, float* out_r, int n_samples,
                             float engine_sr) noexcept {
    (void)engine_sr;
    bool any = false;
    for (auto& v : voices_) {
        if (!v.active()) continue;
        if (v.process(out_l, out_r, n_samples)) any = true;
    }
    return any;
}

int VoicePool::activeCount() const noexcept {
    int n = 0;
    for (const auto& v : voices_) if (v.active()) n++;
    return n;
}

} // namespace ithaca
