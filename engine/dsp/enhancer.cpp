#include "dsp/enhancer.h"
#include <algorithm>
#include <cmath>

namespace ithaca::dsp {

namespace {
constexpr float kXoverLow  = 250.f;
constexpr float kXoverHigh = 3000.f;
constexpr float kHfCap     = 11000.f;
constexpr float kQ         = 0.707f;
constexpr float kDelayLowMs = 2.5f;
constexpr float kDelayMidMs = 0.5f;
constexpr float kAtkMs = 5.f, kRelMs = 80.f;
constexpr float kScaleLo = 0.05f, kScaleHi = 0.35f;
constexpr float kExcHp  = 4500.f;
constexpr float kExcite = 0.35f;
constexpr float kSat    = 0.5f;
inline float db2lin(float db) { return std::pow(10.f, db / 20.f); }
}

const Param Enhancer::kParams[3] = {
    {"process", "PROCESS", 0.f, 12.f,  0.f, "%.1f dB", false},
    {"contour", "CONTOUR", 0.f, 12.f,  0.f, "%.1f dB", false},
    {"mid",     "MID",    -6.f,  6.f,  0.f, "%.1f dB", false},
};

void Enhancer::computeCoeffs_() {
    lp_lo_  = rbj_lowpass (kXoverLow,  kQ, sr_);
    hp_lo_  = rbj_highpass(kXoverLow,  kQ, sr_);
    lp_mid_ = rbj_lowpass (kXoverHigh, kQ, sr_);
    hp_hi_  = rbj_highpass(kXoverHigh, kQ, sr_);
    lp_cap_ = rbj_lowpass (kHfCap,     kQ, sr_);
    hp_exc_ = rbj_highpass(kExcHp,     kQ, sr_);
}

void Enhancer::prepare(float sr, int /*max_block*/) {
    sr_ = sr;
    computeCoeffs_();
    delay_low_n_ = (int)(kDelayLowMs * 0.001f * sr_ + 0.5f);
    delay_mid_n_ = (int)(kDelayMidMs * 0.001f * sr_ + 0.5f);
    for (auto& c : ch_) {
        c.dl_low.assign((size_t)std::max(1, delay_low_n_), 0.f);
        c.dl_mid.assign((size_t)std::max(1, delay_mid_n_), 0.f);
        c.wl = c.wm = 0;
    }
    atk_ = std::exp(-1.f / (kAtkMs * 0.001f * sr_));
    rel_ = std::exp(-1.f / (kRelMs * 0.001f * sr_));
    reset();
}

void Enhancer::reset() {
    for (auto& c : ch_) {
        c.s_lp_lo = c.s_hp_lo = c.s_lp_mid = c.s_hp_hi = c.s_lp_cap = c.s_hp_exc = BiquadState{};
        std::fill(c.dl_low.begin(), c.dl_low.end(), 0.f);
        std::fill(c.dl_mid.begin(), c.dl_mid.end(), 0.f);
        c.wl = c.wm = 0;
    }
    env_ = 0.f;
}

void Enhancer::process(float* L, float* R, int n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    const float gLow     = db2lin(p_[1].load(std::memory_order_relaxed));  // CONTOUR
    const float gMid     = db2lin(p_[2].load(std::memory_order_relaxed));  // MID
    const float procDb   = p_[0].load(std::memory_order_relaxed);          // PROCESS [dB]
    const float gHiMax   = db2lin(procDb);
    const float procNorm = procDb * (1.f / 12.f);                          // 0..1

    for (int i = 0; i < n; ++i) {
        float xL = L[i], xR = R[i];
        float a = std::max(std::fabs(xL), std::fabs(xR));
        env_ = (a > env_) ? (atk_ * env_ + (1.f - atk_) * a)
                          : (rel_ * env_ + (1.f - rel_) * a);
        const float scale = smoothstep(env_, kScaleLo, kScaleHi);
        const float gHi   = 1.f + (gHiMax - 1.f) * scale;
        const float excite_amt = kExcite * procNorm * scale;

        for (int chi = 0; chi < 2; ++chi) {
            Chan& c = ch_[chi];
            float x = (chi == 0) ? xL : xR;
            float low  = biquad_tick(x, lp_lo_, c.s_lp_lo);
            float rest = biquad_tick(x, hp_lo_, c.s_hp_lo);
            float mid  = biquad_tick(rest, lp_mid_, c.s_lp_mid);
            float high = biquad_tick(x, hp_hi_, c.s_hp_hi);
            high       = biquad_tick(high, lp_cap_, c.s_lp_cap);
            float exc  = high + kSat * high * std::fabs(high);
            exc        = biquad_tick(exc, hp_exc_, c.s_hp_exc);
            float lowD = pushDelay_(c.dl_low, c.wl, delay_low_n_, low);
            float midD = pushDelay_(c.dl_mid, c.wm, delay_mid_n_, mid);
            float out  = lowD * gLow + midD * gMid + high * gHi + exc * excite_amt;
            if (chi == 0) L[i] = out; else R[i] = out;
        }
    }
}

} // namespace ithaca::dsp
