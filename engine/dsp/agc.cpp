#include "dsp/agc.h"
#include <cmath>

namespace ithaca::dsp {

const Param AGC::kParams[3] = {
    {"target_rms", "TARGET",     0.01f, 0.5f,   0.15f, "%.3f",    false},
    {"release_ms", "RELEASE",    10.f,  2000.f, 200.f, "%.0f ms", false},
    {"gain_floor", "GAIN FLOOR", 0.f,   1.f,    0.05f, "%.2f",    false},
};

void AGC::applyParams_(bool force) {
    const float rel_ms = release_ms_.load(std::memory_order_relaxed);
    if (force || rel_ms != last_rel_ms_) {
        rel_ = 1.f - std::exp(-1.f / (rel_ms * 0.001f * sr_));
        last_rel_ms_ = rel_ms;
    }
}

void AGC::process(float* L, float* R, int n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    applyParams_(/*force=*/false);
    const float target_rms = target_.load(std::memory_order_relaxed);
    const float gain_floor = floor_.load(std::memory_order_relaxed);

    // Per-blok RMS pres oba kanaly.
    float sum_sq = 0.f;
    for (int i = 0; i < n; ++i) sum_sq += L[i]*L[i] + R[i]*R[i];
    float rms = std::sqrt(sum_sq / (float)(n * 2));

    float target_gain = 1.f;
    if (rms > 1e-6f) {
        target_gain = target_rms / rms;
        if (target_gain > 1.f) target_gain = 1.f;
        if (target_gain < gain_floor) target_gain = gain_floor;
    }
    // Rychly attack (snizovani), pomaly release (zotaveni).
    const float coeff = (target_gain < gain_) ? atk_ : rel_;
    for (int i = 0; i < n; ++i) {
        gain_ += (target_gain - gain_) * coeff;
        L[i] *= gain_;
        R[i] *= gain_;
    }
    cur_gain_.store(gain_, std::memory_order_relaxed);
}

} // namespace ithaca::dsp
