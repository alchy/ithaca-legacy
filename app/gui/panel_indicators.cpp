// app/gui/panel_indicators.cpp - indikatorovy strip: MIDI lampy + sustain bar
// (col1) | diag dlazdice VOICES/RESONANCE/RINGS (center) | peak L/R bars (col3).
// VOICES zlate (primarni), RESONANCE stribro (sekundarni). Kresli do root okna.
#include "panel_indicators.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ithaca::gui {

namespace {
float toDb(float lin) { return lin < 1e-6f ? -120.f : 20.f * std::log10(lin); }
float dbTo01(float db) { float t=(db+60.f)/60.f; return t<0?0:(t>1?1:t); }
} // namespace

void renderIndicatorStrip(AppContext& ctx, float col1_w, float col3_w) {
    using theme::Colors;
    const float H = 78.f;

    // --- col1: MIDI lampy + SUSTAIN ---
    ImGui::BeginChild("##ind_midi", {col1_w, H}, false);
    ImGui::Dummy({0,4}); ImGui::Indent(14.f);
    bool active[128]; ctx.engine.activeMidiNotes(active);
    bool any_note = false; for (int i=0;i<128;++i) if (active[i]) { any_note=true; break; }
    wdg::Lamp("NOTE", any_note, Colors::gold);
    ImGui::SameLine(0, 14);
    wdg::Lamp("CC", false, Colors::silver);   // CC blik: zatim staticky off (FUTURE: blik na CC zmenu)
    ImGui::Dummy({0,6});
    const int cc = (int)ctx.engine.pedalCC();
    char sus[24]; std::snprintf(sus, sizeof(sus), "SUSTAIN  %d", cc);
    wdg::Eyebrow(sus);
    wdg::HBar((float)cc/127.f, ImGui::GetContentRegionAvail().x - 14.f, 8.f,
              Colors::gold, Colors::gold, 0.5f);   // ryska = half-pedal prah
    ImGui::Unindent(14.f);
    ImGui::EndChild();

    ImGui::SameLine(0,0);

    // --- center: diag dlazdice ---
    float center_w = ImGui::GetContentRegionAvail().x - col3_w;
    ImGui::BeginChild("##ind_diag", {center_w, H}, false);
    ImGui::Dummy({0,6});
    char vbuf[8], rbuf[8], gbuf[16];
    std::snprintf(vbuf, sizeof(vbuf), "%d", ctx.engine.activeVoices());
    std::snprintf(rbuf, sizeof(rbuf), "%d", ctx.engine.resonanceVoices());
    std::snprintf(gbuf, sizeof(gbuf), "%d", ctx.engine.numRingsUsed());
    float third = center_w / 3.f;
    ImGui::BeginChild("##t_v", {third, H-6}, false); wdg::StatTile("VOICES", vbuf, Colors::gold);   ImGui::EndChild(); ImGui::SameLine(0,0);
    ImGui::BeginChild("##t_r", {third, H-6}, false); wdg::StatTile("RESONANCE", rbuf, Colors::silver); ImGui::EndChild(); ImGui::SameLine(0,0);
    ImGui::BeginChild("##t_g", {third, H-6}, false); wdg::StatTile("RINGS", gbuf, Colors::silver);   ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine(0,0);

    // --- col3: peak L/R ---
    ImGui::BeginChild("##ind_peak", {col3_w, H}, false);
    ImGui::Dummy({0,8}); ImGui::Indent(14.f);
    wdg::Eyebrow("PEAK  L \xC2\xB7 R");
    float bw = ImGui::GetContentRegionAvail().x - 14.f;
    wdg::HBar(dbTo01(toDb(ctx.engine.masterPeakL())), bw, 6.f, Colors::silver2, Colors::ink);
    ImGui::Dummy({0,3});
    wdg::HBar(dbTo01(toDb(ctx.engine.masterPeakR())), bw, 6.f, Colors::silver2, Colors::ink);
    ImGui::Unindent(14.f);
    ImGui::EndChild();
}

} // namespace ithaca::gui
