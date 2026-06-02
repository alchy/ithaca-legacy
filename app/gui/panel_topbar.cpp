// app/gui/panel_topbar.cpp - viz panel_topbar.h. Art Deco top bar:
// logo ITHACA (zlate, brand font) | MIDI IN dropdown + RESCAN | CHANNEL
// (OMNI/1-16) | LOG level + RESET (vpravo). BANK selektor je v panel_bank,
// MASTER slider v panel_params (VOICE). Kresli inline do ##topbar childu.
#include "panel_topbar.h"
#include "app_context.h"
#include "theme.h"
#include "midi/midi_input.h"
#include "util/log.h"
#include "imgui.h"
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
    // Popisek v body fontu (stejna velikost jako tlacitko RESCAN), tlumena barva.
    // AlignTextToFramePadding → vertikalni stred s combo/tlacitkem na radku.
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("MIDI IN");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300);   // delsi nazvy portu (IAC Driver Bus 1 ...)
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
    ImGui::Button("RESCAN##reload");
    ImGui::SameLine(0, 18);

    // CHANNEL dropdown: OMNI + 1..16. Popisek v body fontu (jako RESCAN).
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("CH");
    ImGui::PopStyleColor();
    ImGui::SameLine(); ImGui::SetNextItemWidth(90);
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

    // -- SAMPLE RATE (read-only) | BUFFER (runtime combo + ms) | DSP LOAD --
    const int   sr   = ctx.engine.sampleRate();
    const float sr_f = (float)(sr > 0 ? sr : 48000);

    ImGui::SameLine(0, 18);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("SR");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    { char b[16]; std::snprintf(b, sizeof(b), "%g kHz", sr_f / 1000.0);
      ImGui::TextUnformatted(b); }

    ImGui::SameLine(0, 18);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("BUFFER");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(72);
    {
        static const int kBufs[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
        const int cur_bs = ctx.engine.blockSize();
        char curlbl[8]; std::snprintf(curlbl, sizeof(curlbl), "%d", cur_bs);
        if (ImGui::BeginCombo("##buffer", curlbl)) {
            for (int v : kBufs) {
                char b[8]; std::snprintf(b, sizeof(b), "%d", v);
                if (ImGui::Selectable(b, v == cur_bs) && v != cur_bs)
                    ctx.setAudioBlockSize(v);
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    { char b[16]; std::snprintf(b, sizeof(b), "%.1f ms",
                                (float)ctx.engine.blockSize() * 1000.0f / sr_f);
      ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
      ImGui::TextUnformatted(b);
      ImGui::PopStyleColor(); }

    ImGui::SameLine(0, 18);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("DSP");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    {
        const bool  overload = ctx.engine.overloadRecent(4000.f);
        const ImVec4 col = overload ? Colors::v(IM_COL32(0xd0, 0x5a, 0x4a, 255))
                                    : Colors::v(Colors::muted);
        char b[8]; std::snprintf(b, sizeof(b), "%.0f%%", ctx.engine.dspLoadPeak() * 100.f);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(b);
        ImGui::PopStyleColor();
    }

    // LOG level + RESET — vpravo. (MASTER se presunul do VOICE panelu jako
    // primarni slider.) RESET vraci vsechny VOICE/master parametry na default.
    const float right_margin = 290.f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - right_margin);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("LOG");
    ImGui::PopStyleColor();
    ImGui::SameLine(); ImGui::SetNextItemWidth(120);
    {
        static const char* kLevels[] = { "debug","info","warn","error","fatal","off" };
        constexpr int kNum = IM_ARRAYSIZE(kLevels);
        int cur = 1;
        for (int i=0;i<kNum;++i) if (ctx.state.log_level==kLevels[i]){cur=i;break;}
        if (ImGui::Combo("##log", &cur, kLevels, kNum)) {
            ctx.state.log_level = kLevels[cur];
            log::Logger::default_().setMinSeverity(
                log::severity_from_string(ctx.state.log_level.c_str(), log::Severity::Info));
        }
    }
    ImGui::SameLine(0, 16);
    if (ImGui::Button("RESET")) {
        ctx.state.resonance_strength = 0.5f;
        ctx.state.release_ms         = 200.f;
        ctx.state.excite_decay_ms    = 5000.f;
        ctx.state.master_gain_db     = 0.f;
        ctx.engine.setResonanceStrength(0.5f);
        ctx.engine.setReleaseMs(200.f);
        ctx.engine.setExciteDecayMs(5000.f);
        ctx.engine.setMasterGain(1.f);
    }
}

} // namespace ithaca::gui
