// app/gui/panel_topbar.cpp - viz panel_topbar.h. Art Deco top bar:
// logo ITHACA (zlate, brand font) | MIDI IN dropdown + ⟳ rescan | CHANNEL
// (OMNI/1-16) | MASTER slider (vpravo). BANK selektor je presunut do
// panel_bank (levy sloupec). Kresli inline do aktualniho ##topbar childu.
#include "panel_topbar.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "midi/midi_input.h"
#include "imgui.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ithaca::gui {

void renderTopBar(AppContext& ctx) {
    using theme::Colors; using theme::Fonts;

    // Logo ITHACA — zlate, brand font.
    if (Fonts::brand) ImGui::PushFont(Fonts::brand);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::gold));
    ImGui::TextUnformatted("ITHACA");
    ImGui::PopStyleColor();
    if (Fonts::brand) ImGui::PopFont();
    ImGui::SameLine(0, 24);

    // MIDI IN dropdown + ⟳ rescan.
    auto ports = ithaca::MidiInput::listPorts();
    wdg::Eyebrow("MIDI IN"); ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    const char* cur = ctx.state.midi_port_name.empty() ? "(none)"
                    : ctx.state.midi_port_name.c_str();
    if (ImGui::BeginCombo("##midi", cur)) {
        if (ImGui::Selectable("(none)", ctx.state.midi_port_name.empty())) {
            ctx.midi.close();
            ctx.state.midi_port_name.clear();
        }
        for (size_t i = 0; i < ports.size(); ++i) {
            bool sel = ports[i] == ctx.state.midi_port_name;
            if (ImGui::Selectable(ports[i].c_str(), sel)) {
                if (ports[i] != ctx.state.midi_port_name) {
                    ctx.midi.close();
                    if (ctx.midi.open(ctx.engine, (int)i)) {
                        ctx.state.midi_port_name = ports[i];
                        ctx.midi.setChannel(ctx.state.midi_channel);
                    }
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    // ⟳ rescan: listPorts() se vola kazdy frame (zive), tlacitko je vizualni
    // hook (a misto pro budouci cache-clear). U+21BB = ↻.
    ImGui::Button("\xE2\x86\xBB##reload");
    ImGui::SameLine(0, 18);

    // CHANNEL dropdown: OMNI + 1..16.
    wdg::Eyebrow("CH"); ImGui::SameLine(); ImGui::SetNextItemWidth(70);
    char chlbl[8];
    if (ctx.state.midi_channel < 0) std::snprintf(chlbl, sizeof(chlbl), "OMNI");
    else std::snprintf(chlbl, sizeof(chlbl), "%d", ctx.state.midi_channel + 1);
    if (ImGui::BeginCombo("##ch", chlbl)) {
        if (ImGui::Selectable("OMNI", ctx.state.midi_channel < 0)) {
            ctx.state.midi_channel = -1; ctx.midi.setChannel(-1);
        }
        for (int c = 0; c < 16; ++c) {
            char b[4]; std::snprintf(b, sizeof(b), "%d", c + 1);
            if (ImGui::Selectable(b, ctx.state.midi_channel == c)) {
                ctx.state.midi_channel = c; ctx.midi.setChannel(c);
            }
        }
        ImGui::EndCombo();
    }

    // MASTER slider — vpravo.
    const float right_margin = 240.f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - right_margin);
    wdg::Eyebrow("MASTER"); ImGui::SameLine(); ImGui::SetNextItemWidth(150);
    if (ImGui::SliderFloat("##master", &ctx.state.master_gain_db, -60.f, 6.f, "%.1f dB")) {
        ctx.engine.setMasterGain(std::pow(10.f, ctx.state.master_gain_db / 20.f));
    }
}

} // namespace ithaca::gui
