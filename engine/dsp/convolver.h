#pragma once
// engine/dsp/convolver.h — Convolver (cabinet/body sim) jako DspStage.
// Direct time-domain FIR, mono IR na L/R, wet/dry MIX. Fixni max ring; RT-safe
// 2-slot IR swap (atomicky publikovany aktivni slot + ack od audio threadu:
// process() hlasi cteny slot pres seen_, setIR pred prepsanim slotu pocka, az
// ho audio opusti — dve publikace behem jednoho in-flight bloku uz nemohou
// dealokovat vector pod rukama audio threadu). Viz spec 2026-06-03.
#include "dsp/dsp_stage.h"
#include <atomic>
#include <string>
#include <vector>

namespace ithaca::dsp {

// Energeticky orez koncoveho ocasku IR: vrati nejmensi delku L takovou, ze
// zahozena energie (tapy [L, n)) je <= drop_frac * celkova energie. Tim je orez
// SLUCHOVE TRANSPARENTNI — relativni L2 chyba wet vystupu je ~sqrt(drop_frac)
// (napr. drop_frac=1e-6 → ~-60 dB). Min 1; ticho → 1. Cista fce (testovatelna).
int trimmedIrLength(const std::vector<float>& ir, double drop_frac);

class Convolver : public DspStage {
public:
    static constexpr int kMaxIr = 8192;   // ~170 ms @48k

    const char* name() const override { return "CONVOLVER"; }
    void prepare(float sr, int max_block) override;
    void reset() override;

    int paramCount() const override { return 4; }
    const Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override {
        switch (i) {
            case 1:  return decay_.load(std::memory_order_relaxed);
            case 2:  return tone_.load(std::memory_order_relaxed);
            case 3:  return size_.load(std::memory_order_relaxed);
            default: return mix_.load(std::memory_order_relaxed);
        }
    }
    void set(int i, float v) override {
        v = (v < 0.f) ? 0.f : (v > 1.f ? 1.f : v);
        if (i == 1)      { decay_.store(v, std::memory_order_relaxed); rebuildIr(); }
        else if (i == 2) { tone_.store(v, std::memory_order_relaxed);  rebuildIr(); }
        else if (i == 3) { size_.store(v, std::memory_order_relaxed);  rebuildIr(); }
        else             { mix_.store(v, std::memory_order_relaxed); }
    }
    bool hasEnable() const override { return true; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool on) override { enabled_.store(on, std::memory_order_relaxed); }
    bool meter(float&, const char*&) const override { return false; }

    void process(float* L, float* R, int n) override;

    void setIR(const std::vector<float>& ir);   // off-RT/GUI; normalizuje + publikuje
    int  irLength() const;

    int choiceCount() const override;
    const char* choiceName(int i) const override;
    int currentChoice() const override { return cur_choice_.load(std::memory_order_relaxed); }
    void selectChoice(int i) override;
    const char* choiceLabel() const override { return "IR"; }

private:
    void rebuildIr();
    static const Param kParams[4];
    std::atomic<float> mix_{0.15f};
    std::atomic<float> decay_{0.5f};
    std::atomic<float> tone_{0.6f};
    std::atomic<float> size_{0.5f};
    std::vector<float> base_ir_;
    std::atomic<bool>  enabled_{false};
    float sr_ = 48000.f;

    std::vector<float> ir_[2];
    std::atomic<int>   active_{0};
    std::atomic<int>   seen_{-1};   // slot prave cteny v process(); -1 = audio necte
    std::vector<float> buf_l_, buf_r_;
    int                write_pos_ = 0;

    std::vector<std::string> choice_names_;
    std::atomic<int>   cur_choice_{0};
};

} // namespace ithaca::dsp
