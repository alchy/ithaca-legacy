// app/gui/panel_indicators.cpp - indikatorovy strip: MIDI lampy + sustain bar
// (col1) | diag dlazdice VOICES/RESONANCE/RINGS (center) | peak L/R bars (col3).
// VOICES zlate (primarni), RESONANCE stribro (sekundarni). Kresli do root okna.
#include "panel_indicators.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "imgui.h"
#include <cmath>
#include <cstdio>

namespace ithaca::gui {

namespace {
float toDb(float lin) { return lin < 1e-6f ? -120.f : 20.f * std::log10(lin); }
float dbTo01(float db) { float t=(db+60.f)/60.f; return t<0?0:(t>1?1:t); }
} // namespace

void renderIndicatorStrip(AppContext& ctx, float col1_w, float col3_w) {
    using theme::Colors;
    const float H = ImGui::GetContentRegionAvail().y;  // = strip_h z shellu
    const float pad = 14.f;

    // --- col1: MIDI lampy + SUSTAIN ---
    ImGui::BeginChild("##ind_midi", {col1_w, H}, false);
    ImGui::Dummy({0,6}); ImGui::Indent(pad);
    // NOTE/OFF lampy: kratke bliknuti na MIDI note-on / note-off event
    // (engine drzi timestamp; ~120 ms blik). Drive NOTE svitil trvale, protoze
    // activeMidiNotes() zahrnuje i release/pedal-drzene hlasy.
    constexpr float kFlashMs = 120.f;
    wdg::Lamp("ON",  ctx.engine.noteOnRecent(kFlashMs),  Colors::gold);
    ImGui::SameLine(0, 14);
    wdg::Lamp("OFF", ctx.engine.noteOffRecent(kFlashMs), Colors::silver);
    ImGui::Dummy({0,8});
    const int cc = (int)ctx.engine.pedalCC();
    char sus[24]; std::snprintf(sus, sizeof(sus), "SUSTAIN  %d", cc);
    wdg::Eyebrow(sus);
    ImGui::Dummy({0,3});
    wdg::HBar((float)cc/127.f, ImGui::GetContentRegionAvail().x - pad, 8.f,
              Colors::gold, Colors::gold, 0.5f);   // ryska = half-pedal prah
    ImGui::Unindent(pad);
    ImGui::EndChild();

    ImGui::SameLine(0,0);

    // --- center: 5 ciselnych dlazdic roztazenych pres celou sirku ---
    // VOICES (vlevo) | RESONANCE | MAIN RINGS | RESO RINGS | DSP LOAD (vpravo).
    // Ring dlazdice ukazuji used/total a CISLO ZCERVENA na 4 s po underrunu
    // daneho poolu (engine drzi timestamp) — uzivatel taha MAX RESONANCE dolu
    // dokud RESO RINGS neprestane cervenat. DSP LOAD = peak-hold zatizeni audio
    // threadu (cas renderu / perioda bloku); ZCERVENA na 4 s po overloadu
    // (load >= 1.0 = blok minul deadline). Zarovnani resi StatTile (align+margin).
    float center_w = ImGui::GetContentRegionAvail().x - col3_w;
    ImGui::BeginChild("##ind_diag", {center_w, H}, false);
    const bool main_ur  = ctx.engine.mainStreamUnderrunRecent(4000.f);
    const bool res_ur   = ctx.engine.resonanceStreamUnderrunRecent(4000.f);
    const bool overload = ctx.engine.overloadRecent(4000.f);
    const ImU32 ring_red = IM_COL32(0xd0, 0x5a, 0x4a, 255);
    char vbuf[8], rbuf[8], mbuf[16], gbuf[16], dbuf[8];
    std::snprintf(vbuf, sizeof(vbuf), "%d", ctx.engine.activeVoices());
    std::snprintf(rbuf, sizeof(rbuf), "%d", ctx.engine.resonanceVoices());
    std::snprintf(mbuf, sizeof(mbuf), "%d/%d",
                  ctx.engine.mainRingsUsed(), ctx.engine.mainRingsTotal());
    std::snprintf(gbuf, sizeof(gbuf), "%d/%d",
                  ctx.engine.resonanceRingsUsed(), ctx.engine.resonanceRingsTotal());
    std::snprintf(dbuf, sizeof(dbuf), "%.0f%%", ctx.engine.dspLoadPeak() * 100.f);
    float fifth = center_w / 5.f;
    ImGui::BeginChild("##t_v", {fifth, H}, false);
        ImGui::Dummy({0,8}); wdg::StatTile("VOICES", vbuf, Colors::gold, 0.f, pad);
    ImGui::EndChild(); ImGui::SameLine(0,0);
    ImGui::BeginChild("##t_r", {fifth, H}, false);
        ImGui::Dummy({0,8}); wdg::StatTile("RESONANCE", rbuf, Colors::silver, 0.5f, pad);
    ImGui::EndChild(); ImGui::SameLine(0,0);
    ImGui::BeginChild("##t_m", {fifth, H}, false);
        ImGui::Dummy({0,8}); wdg::StatTile("MAIN RINGS", mbuf,
                                           main_ur ? ring_red : Colors::silver, 0.5f, pad);
    ImGui::EndChild(); ImGui::SameLine(0,0);
    ImGui::BeginChild("##t_g", {fifth, H}, false);
        ImGui::Dummy({0,8}); wdg::StatTile("RESO RINGS", gbuf,
                                           res_ur ? ring_red : Colors::silver, 0.5f, pad);
    ImGui::EndChild(); ImGui::SameLine(0,0);
    ImGui::BeginChild("##t_d", {fifth, H}, false);
        ImGui::Dummy({0,8}); wdg::StatTile("DSP LOAD", dbuf,
                                           overload ? ring_red : Colors::silver, 1.f, pad);
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine(0,0);

    // --- col3: peak L/R ---
    ImGui::BeginChild("##ind_peak", {col3_w, H}, false);
    ImGui::Dummy({0,10}); ImGui::Indent(pad);
    wdg::Eyebrow("PEAK  L \xC2\xB7 R");
    ImGui::Dummy({0,5});
    float bw = ImGui::GetContentRegionAvail().x - pad;
    wdg::HBar(dbTo01(toDb(ctx.engine.masterPeakL())), bw, 7.f, Colors::silver2, Colors::ink);
    ImGui::Dummy({0,5});
    wdg::HBar(dbTo01(toDb(ctx.engine.masterPeakR())), bw, 7.f, Colors::silver2, Colors::ink);
    ImGui::Unindent(pad);
    ImGui::EndChild();
}

} // namespace ithaca::gui
