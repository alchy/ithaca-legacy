// app/gui/panel_topbar.cpp - viz panel_topbar.h. Art Deco top bar:
// logo ITHACA (zlate, brand font) | MIDI IN dropdown + RESCAN | CHANNEL
// (OMNI/1-16) | LOG level + RESET (vpravo). BANK selektor je v panel_bank,
// MASTER slider v panel_params (VOICE). Kresli inline do ##topbar childu.
#include "panel_topbar.h"
#include "app_context.h"
#include "theme.h"
#include "layout.h"
#include "midi/midi_input.h"
#include "util/log.h"
#include "imgui.h"
#include <cstdio>
#include <string>
#include <vector>

namespace ithaca::gui {

namespace L = ithaca::gui::layout;

void renderTopBar(AppContext& ctx, ithaca::dsp::IParamPage** reset_pages, int n_reset) {
    using theme::Colors; using theme::Fonts;

    // Logo ITHACA — zlate, brand font. AlignTextToFramePadding stejne jako
    // ostatni popisky na radku: bez nej sedi logo (brand 20 px) vys nez
    // popisky (body 18 px) zarovnane ke combum a radek opticky poskakuje.
    ImGui::AlignTextToFramePadding();
    if (Fonts::brand) ImGui::PushFont(Fonts::brand);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::gold));
    ImGui::TextUnformatted("ITHACA");
    ImGui::PopStyleColor();
    if (Fonts::brand) ImGui::PopFont();
    ImGui::SameLine(0, 24);

    // MIDI IN dropdown + RESCAN. Seznam portu je CACHOVANY — listPorts()
    // konstruuje RtMidi klienta (OS IPC) a per-frame volani bylo nejdrazsi
    // operace celeho GUI. Rescan: prvni frame, otevreni comba, tlacitko RESCAN.
    static std::vector<std::string> ports;
    static bool ports_scanned  = false;
    static bool combo_was_open = false;
    if (!ports_scanned) { ports = ithaca::MidiInput::listPorts(); ports_scanned = true; }
    // Popisek v body fontu (stejna velikost jako tlacitko RESCAN), tlumena barva.
    // AlignTextToFramePadding → vertikalni stred s combo/tlacitkem na radku.
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("MIDI IN");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(L::Dims::tb_midi_w);   // misto pro SR/BUFFER skupinu
    const char* cur = ctx.state.midi_port_name.empty() ? "(none)"
                    : ctx.state.midi_port_name.c_str();
    if (ImGui::BeginCombo("##midi", cur)) {
        if (!combo_was_open) {   // rescan pri otevreni comba (1x per open)
            ports = ithaca::MidiInput::listPorts();
            combo_was_open = true;
        }
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
    } else {
        combo_was_open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("RESCAN##reload"))
        ports = ithaca::MidiInput::listPorts();
    ImGui::SameLine(0, L::Dims::tb_gap);

    // CHANNEL dropdown: OMNI + 1..16. Popisek v body fontu (jako RESCAN).
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("CH");
    ImGui::PopStyleColor();
    ImGui::SameLine(); ImGui::SetNextItemWidth(L::Dims::tb_ch_w);
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

    // -- SAMPLE RATE (read-only) | BUFFER (runtime combo + ms) --
    // DSP LOAD metr je v indicator stripu (panel_indicators.cpp) vedle ostatnich
    // mericu (VOICES/RINGS), ne zde.
    const int   sr   = ctx.engine.sampleRate();
    const float sr_f = (float)(sr > 0 ? sr : 48000);

    ImGui::SameLine(0, L::Dims::tb_gap);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("SR");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    { char b[16]; std::snprintf(b, sizeof(b), "%g kHz", sr_f / 1000.0);
      ImGui::TextUnformatted(b); }

    ImGui::SameLine(0, L::Dims::tb_gap);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("BUFFER");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(L::Dims::tb_buffer_w);
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

    // LOG level + RESET — vpravo. (MASTER se presunul do VOICE panelu jako
    // primarni slider.) RESET vraci vsechny VOICE/master parametry na default.
    // Prava skupina zacina na hranici CONFIG sloupce — at LOG/RESET lici
    // se sloupcem pod nimi. Drive tu bylo hardcoded 290.f (= tataz hodnota
    // jako col_dsp, jen nesvazana).
    const float right_margin = L::Dims::col_dsp;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - right_margin);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("LOG");
    ImGui::PopStyleColor();
    ImGui::SameLine(); ImGui::SetNextItemWidth(L::Dims::tb_log_w);
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
    // RESET jede genericky pres Param::def. Drive tu byly defaulty vypsane
    // POTRETI (vedle GuiState defaultu a Param::def) a chybel mezi nimi
    // max_resonance_voices — ten se tedy nikdy neresetoval. set() na strance
    // zapisuje do ctx.state i vola prislusny engine setter, takze rucni
    // volani setteru uz tu nejsou potreba.
    if (ImGui::Button("RESET")) {
        for (int i = 0; i < n_reset; ++i) reset_pages[i]->resetToDefaults();
    }
}

} // namespace ithaca::gui
