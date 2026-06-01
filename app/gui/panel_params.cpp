// app/gui/panel_params.cpp - viz panel_params.h.
#include "panel_params.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "layout.h"
#include "imgui.h"
#include "util/log.h"

namespace ithaca::gui {

void renderParamsPanel(AppContext& ctx) {
    using theme::Colors;
    namespace L = ithaca::gui::layout;
    ImGui::Dummy({0, 4}); ImGui::Indent(L::Dims::pad_panel);
    wdg::Eyebrow("VOICE", Colors::silver2);
    ImGui::Dummy({0, L::Dims::row_gap});

    // RESONANCE — zlata zarazka (primarni).
    if (wdg::DecoSlider("RESONANCE", &ctx.state.resonance_strength, 0.f, 1.f, "%.2f", Colors::gold))
        ctx.engine.setResonanceStrength(ctx.state.resonance_strength);
    ImGui::Dummy({0, L::Dims::row_gap});

    if (wdg::DecoSlider("RELEASE", &ctx.state.release_ms, 50.f, 2000.f, "%.0f ms"))
        ctx.engine.setReleaseMs(ctx.state.release_ms);
    ImGui::Dummy({0, L::Dims::row_gap});

    if (wdg::DecoSlider("EXCITE DECAY", &ctx.state.excite_decay_ms, 500.f, 30000.f, "%.0f ms"))
        ctx.engine.setExciteDecayMs(ctx.state.excite_decay_ms);
    ImGui::Dummy({0, L::Dims::row_gap});

    // MAX RESONANCE — init-only, disabled (zustava default ImGui slider, je read-only).
    ImGui::BeginDisabled();
    wdg::Eyebrow("MAX RESONANCE");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - L::Dims::pad_panel);
    ImGui::SliderInt("##maxres", &ctx.state.max_resonance_voices, 1, 64);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vyzaduje restart aplikace");
    ImGui::Dummy({0, L::Dims::row_gap});

    // LOG LEVEL combo.
    {
        static const char* kLevels[] = { "debug","info","warn","error","fatal" };
        constexpr int kNum = IM_ARRAYSIZE(kLevels);
        int cur = 1;
        for (int i=0;i<kNum;++i) if (ctx.state.log_level==kLevels[i]){cur=i;break;}
        wdg::Eyebrow("LOG LEVEL");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - L::Dims::pad_panel);
        if (ImGui::Combo("##log", &cur, kLevels, kNum)) {
            ctx.state.log_level = kLevels[cur];
            log::Logger::default_().setMinSeverity(
                log::severity_from_string(ctx.state.log_level.c_str(), log::Severity::Info));
        }
    }
    ImGui::Dummy({0, L::Dims::row_gap});
    if (ImGui::Button("RESET")) {
        ctx.state.resonance_strength = 0.5f;
        ctx.state.release_ms = 200.f;
        ctx.state.excite_decay_ms = 5000.f;
        ctx.state.master_gain_db = 0.f;
        ctx.engine.setResonanceStrength(0.5f);
        ctx.engine.setReleaseMs(200.f);
        ctx.engine.setExciteDecayMs(5000.f);
        ctx.engine.setMasterGain(1.f);
    }
    ImGui::Unindent(L::Dims::pad_panel);
}

} // namespace ithaca::gui
