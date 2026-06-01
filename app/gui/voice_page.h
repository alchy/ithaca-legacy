#pragma once
// app/gui/voice_page.h — "VOICE" stranka pro CONFIG prepinac. Neni soucasti
// audio chainu; je to IParamPage adapter nad existujicimi engine settery +
// GuiState. hasEnable()=false (vzdy on). MAX RESONANCE je readonly (init-only).
#include "dsp/dsp_stage.h"
#include "app_context.h"
#include <cmath>

namespace ithaca::gui {

class VoicePage : public ithaca::dsp::IParamPage {
public:
    explicit VoicePage(AppContext& ctx) : ctx_(ctx) {}

    const char* name() const override { return "VOICE"; }
    int paramCount() const override { return 5; }
    const ithaca::dsp::Param& param(int i) const override { return kParams[i]; }

    float get(int i) const override {
        switch (i) {
            case 0: return ctx_.state.master_gain_db;
            case 1: return ctx_.state.resonance_strength;
            case 2: return ctx_.state.release_ms;
            case 3: return ctx_.state.excite_decay_ms;
            default: return (float)ctx_.state.max_resonance_voices;
        }
    }
    void set(int i, float v) override {
        const auto& p = kParams[i];
        v = (v < p.min) ? p.min : (v > p.max ? p.max : v);
        switch (i) {
            case 0: ctx_.state.master_gain_db = v;
                    ctx_.engine.setMasterGain(std::pow(10.f, v / 20.f)); break;
            case 1: ctx_.state.resonance_strength = v;
                    ctx_.engine.setResonanceStrength(v); break;
            case 2: ctx_.state.release_ms = v;
                    ctx_.engine.setReleaseMs(v); break;
            case 3: ctx_.state.excite_decay_ms = v;
                    ctx_.engine.setExciteDecayMs(v); break;
            default: break;   // MAX RESONANCE readonly
        }
    }
    bool hasEnable() const override { return false; }
    bool enabled() const override { return true; }
    void setEnabled(bool) override {}
    bool meter(float&, const char*&) const override { return false; }

private:
    AppContext& ctx_;
    static constexpr ithaca::dsp::Param kParams[5] = {
        {"master_db",  "MASTER",        -60.f, 6.f,    0.f,    "%.1f dB", false},
        {"resonance",  "RESONANCE",       0.f, 1.f,    0.5f,   "%.2f",    false},
        {"release_ms", "RELEASE",        50.f, 2000.f, 200.f,  "%.0f ms", false},
        {"excite_ms",  "EXCITE DECAY",  500.f, 30000.f,5000.f, "%.0f ms", false},
        {"max_res",    "MAX RESONANCE",   1.f, 64.f,   32.f,   "%.0f",    true},
    };
};

} // namespace ithaca::gui
