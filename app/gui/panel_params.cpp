// app/gui/panel_params.cpp - viz panel_params.h.
#include "panel_params.h"
#include "app_context.h"
#include "imgui.h"
#include <cmath>

namespace ithaca::gui {

void renderParamsPanel(AppContext& ctx, float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ImGui::Begin("Params", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    if (ImGui::SliderFloat("Resonance str", &ctx.state.resonance_strength, 0.f, 1.f, "%.2f")) {
        ctx.engine.setResonanceStrength(ctx.state.resonance_strength);
    }
    if (ImGui::SliderFloat("Release ms", &ctx.state.release_ms, 50.f, 2000.f, "%.0f")) {
        ctx.engine.setReleaseMs(ctx.state.release_ms);
    }
    if (ImGui::SliderFloat("Excite decay ms", &ctx.state.excite_decay_ms,
                            500.f, 30000.f, "%.0f")) {
        ctx.engine.setExciteDecayMs(ctx.state.excite_decay_ms);
    }

    // max_resonance_voices je init-only - slider DISABLED s tooltipem.
    ImGui::BeginDisabled();
    ImGui::SliderInt("Max resonance", &ctx.state.max_resonance_voices, 1, 64);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Vyzaduje restart aplikace");
    }

    ImGui::Separator();
    if (ImGui::Button("Reset to defaults")) {
        ctx.state.resonance_strength = 0.5f;
        ctx.state.release_ms         = 200.f;
        ctx.state.excite_decay_ms    = 5000.f;
        ctx.state.master_gain_db     = 0.f;
        ctx.engine.setResonanceStrength(ctx.state.resonance_strength);
        ctx.engine.setReleaseMs(ctx.state.release_ms);
        ctx.engine.setExciteDecayMs(ctx.state.excite_decay_ms);
        ctx.engine.setMasterGain(1.f);
    }

    ImGui::End();
}

} // namespace ithaca::gui
