#include "dsp/limiter.h"
#include <algorithm>
#include <cmath>

namespace ithaca::dsp {

const Param Limiter::kParams[2] = {
    {"threshold_db", "THRESHOLD", -40.f, 0.f,    0.f,   "%.1f dB", false},
    {"release_ms",   "RELEASE",    10.f, 2000.f, 200.f, "%.0f ms", false},
};

void Limiter::applyParams_(bool force) {
    const float thr_db = thr_db_.load(std::memory_order_relaxed);
    const float rel_ms = rel_ms_.load(std::memory_order_relaxed);
    if (force || thr_db != last_thr_db_) { thr_lin_ = db_to_lin(thr_db); last_thr_db_ = thr_db; }
    if (force || rel_ms != last_rel_ms_) { rel_ = decay_coeff(rel_ms * 0.001f, sr_); last_rel_ms_ = rel_ms; }
}

void Limiter::process(float* L, float* R, int n) {
    if (!enabled_.load(std::memory_order_relaxed)) { gr_db_.store(0.f, std::memory_order_relaxed); return; }
    applyParams_(/*force=*/false);
    for (int i = 0; i < n; ++i) {
        float peak = std::max(std::abs(L[i]), std::abs(R[i]));
        float target = (peak > thr_lin_ && peak > 1e-9f) ? thr_lin_ / peak : 1.f;
        gain_ = gain_envelope_smooth(gain_, target, atk_, rel_);
        gain_ = std::min(gain_, 1.f);
        L[i] *= gain_;
        R[i] *= gain_;
    }
    gr_db_.store(lin_to_db(std::max(gain_, 1e-9f)), std::memory_order_relaxed);
}

} // namespace ithaca::dsp
