#pragma once
// engine/dsp/enhancer.h — Enhancer (BBE-style Sonic Maximizer) jako DspStage.
// Hybrid: 3-pasmovy split (LOW/MID/HIGH) + per-pasmove group-delay zarovnani
// + dynamicky boost-when-loud na HIGH (PROCESS) + jemny harmonicky exciter.
// Viz spec 2026-06-03. Original piano enhancer (ne klon BBE).
#include "dsp/dsp_stage.h"
#include "dsp/dsp_math.h"
#include <atomic>
#include <vector>

namespace ithaca::dsp {

class Enhancer : public DspStage {
public:
    const char* name() const override { return "ENHANCER"; }
    void prepare(float sr, int max_block) override;
    void reset() override;

    int paramCount() const override { return 3; }
    const Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override { return p_[(size_t)i].load(std::memory_order_relaxed); }
    void set(int i, float v) override {
        v = (v < kParams[i].min) ? kParams[i].min : (v > kParams[i].max ? kParams[i].max : v);
        p_[(size_t)i].store(v, std::memory_order_relaxed);
    }
    bool hasEnable() const override { return true; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool on) override { enabled_.store(on, std::memory_order_relaxed); }
    bool meter(float&, const char*&) const override { return false; }

    void process(float* L, float* R, int n) override;

private:
    static const Param kParams[3];   // 0=PROCESS(HIGH), 1=CONTOUR(LOW), 2=MID
    std::atomic<float> p_[3]{};      // dB
    std::atomic<bool>  enabled_{false};
    float sr_ = 48000.f;

    BiquadCoeffs lp_lo_{}, hp_lo_{}, lp_mid_{}, hp_hi_{}, lp_cap_{}, hp_exc_{};

    struct Chan {
        BiquadState s_lp_lo, s_hp_lo, s_lp_mid, s_hp_hi, s_lp_cap, s_hp_exc;
        std::vector<float> dl_low, dl_mid;
        int wl = 0, wm = 0;
    } ch_[2];
    int   delay_low_n_ = 0, delay_mid_n_ = 0;

    float env_ = 0.f, atk_ = 0.f, rel_ = 0.f;

    void computeCoeffs_();
    static float pushDelay_(std::vector<float>& dl, int& w, int n, float x) {
        if (n <= 0) return x;
        float y = dl[(size_t)w];
        dl[(size_t)w] = x;
        w = (w + 1) % n;
        return y;
    }
};

} // namespace ithaca::dsp
