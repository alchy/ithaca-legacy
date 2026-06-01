#pragma once
// engine/dsp/agc.h — RMS-following downward AGC jako DspStage. Meri per-blok RMS
// a vyhlazene snizuje gain k target RMS; nikdy nezesiluje, nikdy pod floor.
// Algoritmus portovan z icr/engine/dsp/agc.h (AgcState/agc_process).
#include "dsp/dsp_stage.h"
#include <atomic>
#include <cmath>

namespace ithaca::dsp {

class AGC : public DspStage {
public:
    const char* name() const override { return "AGC"; }
    void prepare(float sr, int /*max_block*/) override {
        sr_ = sr;
        atk_ = 1.f - std::exp(-1.f / (5.f * 0.001f * sr_));   // fixni 5 ms attack
        applyParams_(true);
        reset();
    }
    void reset() override { gain_ = 1.f; cur_gain_.store(1.f, std::memory_order_relaxed); }

    int paramCount() const override { return 3; }
    const Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override {
        if (i==0) return target_.load(std::memory_order_relaxed);
        if (i==1) return release_ms_.load(std::memory_order_relaxed);
        return floor_.load(std::memory_order_relaxed);
    }
    void set(int i, float v) override {
        v = (v < kParams[i].min) ? kParams[i].min : (v > kParams[i].max ? kParams[i].max : v);
        if (i==0) target_.store(v, std::memory_order_relaxed);
        else if (i==1) release_ms_.store(v, std::memory_order_relaxed);
        else floor_.store(v, std::memory_order_relaxed);
    }
    bool hasEnable() const override { return true; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool on) override { enabled_.store(on, std::memory_order_relaxed); }
    bool meter(float& value, const char*& label) const override {
        value = cur_gain_.load(std::memory_order_relaxed); label = "CURRENT GAIN"; return true;
    }

    void process(float* L, float* R, int n) override;

private:
    static const Param kParams[3];
    std::atomic<float> target_{0.15f};
    std::atomic<float> release_ms_{200.f};
    std::atomic<float> floor_{0.05f};
    std::atomic<bool>  enabled_{false};
    float sr_ = 48000.f, gain_ = 1.f, atk_ = 0.f, rel_ = 0.f;
    float last_rel_ms_ = -1.f;
    std::atomic<float> cur_gain_{1.f};
    void applyParams_(bool force);
};

} // namespace ithaca::dsp
