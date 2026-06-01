#pragma once
// engine/dsp/limiter.h — stereo peak limiter jako DspStage.
// Algoritmus portovan z icr/engine/dsp/limiter/limiter.{h,cpp}: per-sample peak
// max(|L|,|R|), target = threshold/peak, vyhlazena obalka (fast attack / release),
// gain vzdy <= 1. Parametry atomicky; koeficienty se prepocitaji v process().
#include "dsp/dsp_stage.h"
#include "dsp/dsp_math.h"
#include <atomic>

namespace ithaca::dsp {

class Limiter : public DspStage {
public:
    const char* name() const override { return "LIMITER"; }
    void prepare(float sr, int /*max_block*/) override {
        sr_ = sr;
        atk_ = decay_coeff(0.001f, sr_);          // fixni 1 ms attack
        applyParams_(/*force=*/true);
        reset();
    }
    void reset() override { gain_ = 1.f; gr_db_.store(0.f, std::memory_order_relaxed); }

    int paramCount() const override { return 2; }
    const Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override {
        return (i == 0) ? thr_db_.load(std::memory_order_relaxed)
                        : rel_ms_.load(std::memory_order_relaxed);
    }
    void set(int i, float v) override {
        v = (v < kParams[i].min) ? kParams[i].min : (v > kParams[i].max ? kParams[i].max : v);
        if (i == 0) thr_db_.store(v, std::memory_order_relaxed);
        else        rel_ms_.store(v, std::memory_order_relaxed);
    }
    bool hasEnable() const override { return true; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool on) override { enabled_.store(on, std::memory_order_relaxed); }
    bool meter(float& value, const char*& label) const override {
        value = gr_db_.load(std::memory_order_relaxed); label = "GAIN REDUCTION"; return true;
    }

    void process(float* L, float* R, int n) override;

private:
    static const Param kParams[2];
    // GUI-nastavovane parametry (atomic).
    std::atomic<float> thr_db_{0.f};
    std::atomic<float> rel_ms_{200.f};
    std::atomic<bool>  enabled_{false};
    // Audio-thread odvozene koeficienty + stav.
    float sr_ = 48000.f, gain_ = 1.f, thr_lin_ = 1.f, atk_ = 0.f, rel_ = 0.f;
    float last_thr_db_ = 1e9f, last_rel_ms_ = -1.f;
    std::atomic<float> gr_db_{0.f};   // metr (psan audio, cten GUI)
    // Prepocita thr_lin_/rel_ z atomickych parametru kdyz se zmenily.
    void applyParams_(bool force);
};

} // namespace ithaca::dsp
