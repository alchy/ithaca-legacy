#pragma once
// app/gui/resonance_page.h — "RESONANCE" CONFIG stranka: Resonance Layer (dB,
// dynamicky rozsah z banky) + Resonance Gain (dB) + Excite Decay + Max Resonance.
// hasEnable()=true → enabled mapuje na state.resonance_enabled + engine.
#include "dsp/dsp_stage.h"
#include "app_context.h"
#include <algorithm>

namespace ithaca::gui {

class ResonancePage : public ithaca::dsp::IParamPage {
public:
    explicit ResonancePage(AppContext& ctx) : ctx_(ctx) {}
    const char* name() const override { return "RESONANCE"; }
    int paramCount() const override { return 4; }

    const ithaca::dsp::Param& param(int i) const override {
        if (i == 0) {   // RESONANCE LAYER — dynamicky rozsah z banky
            float lo = ctx_.engine.bankPeakRmsMinDb();
            float hi = ctx_.engine.bankPeakRmsMaxDb();
            if (hi <= lo) hi = lo + 1.f;   // degenerace (1 vrstva / bez banky)
            layer_param_.min = lo; layer_param_.max = hi;
            return layer_param_;
        }
        return kParams[i - 1];
    }
    float get(int i) const override {
        switch (i) {
            case 0: {
                float lo = ctx_.engine.bankPeakRmsMinDb(), hi = ctx_.engine.bankPeakRmsMaxDb();
                if (hi < lo) hi = lo;
                return std::clamp(ctx_.state.resonance_layer_db, lo, hi);
            }
            case 1: return ctx_.state.resonance_gain_db;
            case 2: return ctx_.state.excite_decay_ms;
            default: return (float)ctx_.state.max_resonance_voices;
        }
    }
    void set(int i, float v) override {
        switch (i) {
            case 0: ctx_.state.resonance_layer_db = v;
                    ctx_.engine.setResonanceLayerDb(v); break;
            case 1: { const auto& p = kParams[0]; v = std::clamp(v, p.min, p.max);
                      ctx_.state.resonance_gain_db = v; ctx_.engine.setResonanceGainDb(v); } break;
            case 2: { const auto& p = kParams[1]; v = std::clamp(v, p.min, p.max);
                      ctx_.state.excite_decay_ms = v; ctx_.engine.setExciteDecayMs(v); } break;
            default: { const auto& p = kParams[2]; v = std::clamp(v, p.min, p.max);
                       ctx_.state.max_resonance_voices = (int)v;
                       ctx_.engine.setMaxResonanceVoices((int)v); } break;
        }
    }
    bool hasEnable() const override { return true; }
    bool enabled() const override { return ctx_.state.resonance_enabled; }
    void setEnabled(bool on) override {
        ctx_.state.resonance_enabled = on;
        ctx_.engine.setResonanceEnabled(on);
    }
    bool meter(float&, const char*&) const override { return false; }
private:
    AppContext& ctx_;
    mutable ithaca::dsp::Param layer_param_ =
        {"reso_layer_db", "RESONANCE LAYER", -60.f, 0.f, -30.f, "%.1f dB", false};
    static constexpr ithaca::dsp::Param kParams[3] = {
        {"reso_gain_db", "RESONANCE GAIN", -60.f, 0.f,     -12.f,  "%.1f dB", false},
        {"excite_ms",    "EXCITE DECAY",   500.f, 30000.f, 5000.f, "%.0f ms", false},
        {"max_res",      "MAX RESONANCE",    1.f, 64.f,    32.f,   "%.0f",    false},
    };
};

} // namespace ithaca::gui
