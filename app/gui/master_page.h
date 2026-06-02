#pragma once
// app/gui/master_page.h — "MASTER" CONFIG stranka: MASTER gain + RELEASE.
// IParamPage adapter nad engine settery + GuiState. hasEnable()=false.
#include "dsp/dsp_stage.h"
#include "app_context.h"
#include <cmath>

namespace ithaca::gui {

class MasterPage : public ithaca::dsp::IParamPage {
public:
    explicit MasterPage(AppContext& ctx) : ctx_(ctx) {}
    const char* name() const override { return "MASTER"; }
    int paramCount() const override { return 2; }
    const ithaca::dsp::Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override {
        return i == 0 ? ctx_.state.master_gain_db : ctx_.state.release_ms;
    }
    void set(int i, float v) override {
        const auto& p = kParams[i];
        v = (v < p.min) ? p.min : (v > p.max ? p.max : v);
        if (i == 0) { ctx_.state.master_gain_db = v;
                      ctx_.engine.setMasterGain(std::pow(10.f, v / 20.f)); }
        else        { ctx_.state.release_ms = v; ctx_.engine.setReleaseMs(v); }
    }
    bool hasEnable() const override { return false; }
    bool enabled() const override { return true; }
    void setEnabled(bool) override {}
    bool meter(float&, const char*&) const override { return false; }
private:
    AppContext& ctx_;
    static constexpr ithaca::dsp::Param kParams[2] = {
        {"master_db",  "MASTER",  -60.f, 6.f,    0.f,   "%.1f dB", false},
        {"release_ms", "RELEASE",  50.f, 2000.f, 200.f, "%.0f ms", false},
    };
};

} // namespace ithaca::gui
