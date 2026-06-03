#include "dsp/enhancer.h"
#include <algorithm>
#include <cmath>

namespace ithaca::dsp {

namespace {
constexpr float kXoverLow  = 250.f;    // low/mid
constexpr float kXoverHigh = 3000.f;   // mid/high
constexpr float kHfCap     = 11000.f;  // horni mez HIGH (rolloff po ~10k)
constexpr float kQ         = 0.707f;
constexpr float kApFreq    = 700.f;    // 1.-radovy all-pass (fazove zarovnani, BBE jadro)
constexpr float kAtkMs = 5.f, kRelMs = 80.f;
constexpr float kScaleLo = 0.05f, kScaleHi = 0.35f;   // env→scale prahy (lin)
constexpr float kExcHp  = 4500.f;      // exciter high-pass
constexpr float kExcite = 0.35f;       // max primes exciteru (jemne)
constexpr float kSat    = 0.5f;        // sila asymetricke saturace (2. harm.)
inline float db2lin(float db) { return std::pow(10.f, db / 20.f); }
}

const Param Enhancer::kParams[3] = {
    {"process", "PROCESS", 0.f, 12.f,  0.f, "%.1f dB", false},   // HIGH (dynamicky)
    {"contour", "CONTOUR", 0.f, 12.f,  0.f, "%.1f dB", false},   // LOW
    {"mid",     "MID",    -6.f,  6.f,  0.f, "%.1f dB", false},   // MID
};

void Enhancer::computeCoeffs_() {
    lp_lo_  = rbj_lowpass (kXoverLow,  kQ, sr_);
    hp_lo_  = rbj_highpass(kXoverLow,  kQ, sr_);
    lp_mid_ = rbj_lowpass (kXoverHigh, kQ, sr_);
    hp_hi_  = rbj_highpass(kXoverHigh, kQ, sr_);
    lp_cap_ = rbj_lowpass (kHfCap,     kQ, sr_);
    hp_exc_ = rbj_highpass(kExcHp,     kQ, sr_);
    // 1.-radovy all-pass H(z)=(c+z^-1)/(1+c z^-1); c=(tan(πf/sr)-1)/(tan(πf/sr)+1).
    const float t = std::tan(PI * kApFreq / sr_);
    ap_c_ = (t - 1.f) / (t + 1.f);
}

void Enhancer::prepare(float sr, int /*max_block*/) {
    sr_ = sr;
    computeCoeffs_();
    atk_ = std::exp(-1.f / (kAtkMs * 0.001f * sr_));
    rel_ = std::exp(-1.f / (kRelMs * 0.001f * sr_));
    reset();
}

void Enhancer::reset() {
    for (auto& c : ch_) {
        c.s_lp_lo = c.s_hp_lo = c.s_lp_mid = c.s_hp_hi = c.s_lp_cap = c.s_hp_exc = BiquadState{};
        c.ap_x1 = c.ap_y1 = 0.f;
    }
    env_ = 0.f;
}

void Enhancer::process(float* L, float* R, int n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    const float gLow     = db2lin(p_[1].load(std::memory_order_relaxed));  // CONTOUR
    const float gMid     = db2lin(p_[2].load(std::memory_order_relaxed));  // MID
    const float procDb   = p_[0].load(std::memory_order_relaxed);          // PROCESS [dB]
    const float gHiMax   = db2lin(procDb);
    const float procNorm = procDb * (1.f / 12.f);                          // 0..1 (exciter)

    for (int i = 0; i < n; ++i) {
        float xL = L[i], xR = R[i];
        // broadband peak env (linkovany pres kanaly)
        float a = std::max(std::fabs(xL), std::fabs(xR));
        env_ = (a > env_) ? (atk_ * env_ + (1.f - atk_) * a)
                          : (rel_ * env_ + (1.f - rel_) * a);
        const float scale = smoothstep(env_, kScaleLo, kScaleHi);
        const float gHi   = 1.f + (gHiMax - 1.f) * scale;   // dynamicky boost-when-loud
        const float excite_amt = kExcite * procNorm * scale;

        for (int chi = 0; chi < 2; ++chi) {
            Chan& c = ch_[chi];
            float x = (chi == 0) ? xL : xR;

            // Band-filtry z dry. PARALLEL boost (nikdy band-replace) → pri unity
            // ziscich vystup = aligned dry, takze zadny comb notch ve stredech.
            float low  = biquad_tick(x, lp_lo_, c.s_lp_lo);
            float rest = biquad_tick(x, hp_lo_, c.s_hp_lo);
            float mid  = biquad_tick(rest, lp_mid_, c.s_lp_mid);
            float high = biquad_tick(x, hp_hi_, c.s_hp_hi);
            high       = biquad_tick(high, lp_cap_, c.s_lp_cap);

            // Harmonicky exciter z HIGH, navazany na PROCESS+scale. high*high je
            // suda nelinearita → generuje 2. harmonickou (+ DC, ktere hp_exc_ odrizne).
            float exc = high + kSat * high * high;
            exc       = biquad_tick(exc, hp_exc_, c.s_hp_exc);

            // Fazove zarovnani: 1.-radovy all-pass na dry (magnitudove ploche).
            float xa = ap_c_ * x + c.ap_x1 - ap_c_ * c.ap_y1;
            c.ap_x1 = x; c.ap_y1 = xa;

            // Parallel boost: aligned dry + (g-1)*band (+exciter). Unity → xa.
            float out = xa
                      + (gLow - 1.f) * low
                      + (gMid - 1.f) * mid
                      + (gHi  - 1.f) * high
                      + exc * excite_amt;
            if (chi == 0) L[i] = out; else R[i] = out;
        }
    }
}

} // namespace ithaca::dsp
