#include "dsp/bbe.h"

namespace ithaca::dsp {

const Param BBE::kParams[2] = {
    {"definition", "DEFINITION", 0.f, 12.f, 0.f, "%.1f dB", false},
    {"bass",       "BASS",       0.f, 10.f, 0.f, "%.1f dB", false},
};

void BBE::applyParams_(bool force) {
    const float d = def_db_.load(std::memory_order_relaxed);
    const float b = bass_db_.load(std::memory_order_relaxed);
    if (force || d != last_def_db_)  { def_c_  = rbj_high_shelf(5000.f, d, sr_); last_def_db_  = d; }
    if (force || b != last_bass_db_) { bass_c_ = rbj_low_shelf (180.f,  b, sr_); last_bass_db_ = b; }
}

void BBE::process(float* L, float* R, int n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    applyParams_(/*force=*/false);
    for (int i = 0; i < n; ++i) {
        L[i] = biquad_tick(L[i], def_c_,  def_l_);
        R[i] = biquad_tick(R[i], def_c_,  def_r_);
        L[i] = biquad_tick(L[i], bass_c_, bass_l_);
        R[i] = biquad_tick(R[i], bass_c_, bass_r_);
    }
}

} // namespace ithaca::dsp
