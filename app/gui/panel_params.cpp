// app/gui/panel_params.cpp - viz panel_params.h.
#include "panel_params.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "layout.h"
#include "imgui.h"
#include <cmath>

namespace ithaca::gui {

void renderParamsPanel(AppContext& ctx) {
    using theme::Colors;
    namespace L = ithaca::gui::layout;
    ImGui::Dummy({0, 4}); ImGui::Indent(L::Dims::pad_panel);
    wdg::Eyebrow("VOICE", Colors::silver2);
    ImGui::Dummy({0, L::Dims::row_gap});

    // MASTER — primarni vystupni uroven celeho nastroje (prvni, zlata zarazka).
    if (wdg::DecoSlider("MASTER", &ctx.state.master_gain_db, -60.f, 6.f, "%.1f dB", Colors::gold))
        ctx.engine.setMasterGain(std::pow(10.f, ctx.state.master_gain_db / 20.f));
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
    // LOG LEVEL + RESET jsou presunute do topbaru (panel_topbar.cpp).
    ImGui::Unindent(L::Dims::pad_panel);
}

} // namespace ithaca::gui
