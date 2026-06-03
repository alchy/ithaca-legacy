#pragma once
// engine/dsp/enhancer.h — Enhancer (BBE-style Sonic Maximizer) jako DspStage.
// Original piano enhancer (NE klon BBE). Parallel-boost model: pasma se PRICITAJI
// jako boosty na dry (transparentni pri unity → zadny comb notch), fazove
// zarovnani jemnym 1.-radovym all-passem (~700 Hz), dynamicky boost-when-loud na
// HIGH (PROCESS) + jemny harmonicky exciter. Viz spec 2026-06-03.
#include "dsp/dsp_stage.h"
#include "dsp/dsp_math.h"
#include <atomic>

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

    // Band filtry (sdilene koef pres kanaly): LOW=LP250, MID=BP(HP250→LP3k),
    // HIGH=HP3k→LPcap. hp_exc_ = exciter high-pass.
    BiquadCoeffs lp_lo_{}, hp_lo_{}, lp_mid_{}, hp_hi_{}, lp_cap_{}, hp_exc_{};
    float ap_c_ = 0.f;   // 1.-radovy all-pass koef (fazove zarovnani)

    struct Chan {
        BiquadState s_lp_lo, s_hp_lo, s_lp_mid, s_hp_hi, s_lp_cap, s_hp_exc;
        float ap_x1 = 0.f, ap_y1 = 0.f;   // all-pass stav
    } ch_[2];

    float env_ = 0.f, atk_ = 0.f, rel_ = 0.f;   // broadband peak monitor (linkovany)

    void computeCoeffs_();
};

} // namespace ithaca::dsp
