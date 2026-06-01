#pragma once
// engine/dsp/bbe.h — zjednoduseny BBE Sonic Maximizer jako DspStage.
// Dva RBJ shelving biquady na kanal: DEFINITION (high shelf 5 kHz, 0..12 dB),
// BASS (low shelf 180 Hz, 0..10 dB). Portovano z icr/engine/dsp/bbe.
#include "dsp/dsp_stage.h"
#include "dsp/dsp_math.h"
#include <atomic>

namespace ithaca::dsp {

class BBE : public DspStage {
public:
    const char* name() const override { return "BBE"; }
    void prepare(float sr, int /*max_block*/) override { sr_ = sr; applyParams_(true); reset(); }
    void reset() override { def_l_={}; def_r_={}; bass_l_={}; bass_r_={}; }

    int paramCount() const override { return 2; }
    const Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override {
        return (i==0) ? def_db_.load(std::memory_order_relaxed)
                      : bass_db_.load(std::memory_order_relaxed);
    }
    void set(int i, float v) override {
        v = (v < kParams[i].min) ? kParams[i].min : (v > kParams[i].max ? kParams[i].max : v);
        if (i==0) def_db_.store(v, std::memory_order_relaxed);
        else      bass_db_.store(v, std::memory_order_relaxed);
    }
    bool hasEnable() const override { return true; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool on) override { enabled_.store(on, std::memory_order_relaxed); }
    bool meter(float&, const char*&) const override { return false; }

    void process(float* L, float* R, int n) override;

private:
    static const Param kParams[2];
    std::atomic<float> def_db_{0.f};
    std::atomic<float> bass_db_{0.f};
    std::atomic<bool>  enabled_{false};
    float sr_ = 48000.f;
    float last_def_db_ = 1e9f, last_bass_db_ = 1e9f;
    BiquadCoeffs def_c_{}, bass_c_{};
    BiquadState  def_l_{}, def_r_{}, bass_l_{}, bass_r_{};
    void applyParams_(bool force);
};

} // namespace ithaca::dsp
