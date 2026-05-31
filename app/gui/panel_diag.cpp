// app/gui/panel_diag.cpp - viz panel_diag.h.
#include "panel_diag.h"
#include "app_context.h"
#include "imgui.h"
#include <cmath>
#include <cfloat>

namespace ithaca::gui {

namespace {
// Lin -> dB s floor -120 dB (vyhneme se log10(0)).
float toDb(float lin) {
    if (lin < 1e-6f) return -120.f;
    return 20.f * std::log10(lin);
}
// Map dB do progress baru [-60..+6] -> [0..1].
float dbTo01(float db) {
    float t = (db + 60.f) / 66.f;
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    return t;
}
} // namespace

void renderDiagPanel(AppContext& ctx, float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ImGui::Begin("Diag", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    ImGui::Text("Voices:    %3d / 256", ctx.engine.activeVoices());
    ImGui::Text("Resonance: %3d /  32", ctx.engine.resonanceVoices());
    ImGui::Text("Rings:     %3d / 288", ctx.engine.numRingsUsed());

    const int cc = (int)ctx.engine.pedalCC();
    // Barevny indikator pedalu: zeleny=0, zluty=64, cerveny=100+.
    ImVec4 col = (cc >= 100) ? ImVec4(1.f, 0.3f, 0.3f, 1.f)
               : (cc >= 64)  ? ImVec4(1.f, 0.9f, 0.3f, 1.f)
                              : ImVec4(0.4f, 1.f, 0.4f, 1.f);
    ImGui::TextColored(col, "Pedal CC64: %d", cc);

    ImGui::Separator();
    ImGui::Text("Master meter:");
    const float dbL = toDb(ctx.engine.masterPeakL());
    const float dbR = toDb(ctx.engine.masterPeakR());
    ImGui::ProgressBar(dbTo01(dbL), {-FLT_MIN, 12}, "");
    ImGui::SameLine(); ImGui::Text("L %5.1f dB", dbL);
    ImGui::ProgressBar(dbTo01(dbR), {-FLT_MIN, 12}, "");
    ImGui::SameLine(); ImGui::Text("R %5.1f dB", dbR);

    ImGui::End();
}

} // namespace ithaca::gui
