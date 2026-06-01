// app/gui/panel_params.cpp - viz panel_params.h.
#include "panel_params.h"
#include "app_context.h"
#include "imgui.h"
#include "util/log.h"
#include <cmath>

namespace ithaca::gui {

void renderParamsPanel(AppContext& ctx) {
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

    // Log level (runtime) — meni min severity loggeru za behu. Vyssi nez Info
    // = audio thread preskoci formatovani ladicich RT zprav (vykon).
    {
        static const char* kLevels[] = { "debug", "info", "warn", "error", "fatal" };
        constexpr int kNumLevels = IM_ARRAYSIZE(kLevels);
        int cur = 1;   // default info
        for (int i = 0; i < kNumLevels; ++i)
            if (ctx.state.log_level == kLevels[i]) { cur = i; break; }
        if (ImGui::Combo("Log level", &cur, kLevels, kNumLevels)) {
            ctx.state.log_level = kLevels[cur];
            log::Logger::default_().setMinSeverity(
                log::severity_from_string(ctx.state.log_level.c_str(),
                                          log::Severity::Info));
        }
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
}

} // namespace ithaca::gui
