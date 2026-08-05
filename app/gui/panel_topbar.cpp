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
#include <algorithm>
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
    auto& ps = ctx.panels;   // cache zije v AppContext (drive function-local static)
    if (!ps.midi_ports_scanned) {
        ps.midi_ports = ithaca::MidiInput::listPorts();
        ps.midi_ports_scanned = true;
    }
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
        if (!ps.midi_combo_open) {   // rescan pri otevreni comba (1x per open)
            ps.midi_ports = ithaca::MidiInput::listPorts();
            ps.midi_combo_open = true;
        }
        if (ImGui::Selectable("(none)", ctx.state.midi_port_name.empty())) {
            ctx.midi.close();
            ctx.state.midi_port_name.clear();
        }
        for (size_t i = 0; i < ps.midi_ports.size(); ++i) {
            const std::string& name = ps.midi_ports[i];
            if (ImGui::Selectable(name.c_str(), name == ctx.state.midi_port_name)
                && name != ctx.state.midi_port_name) {
                // Otevirat podle JMENA, ne podle indexu do cachovaneho seznamu:
                // kdyz se zarizeni odpoji mezi otevrenim comba a klikem, index
                // uz ukazuje jinam a otevrel by se cizi port. (initFromState
                // matchuje podle jmena taky — ted je to konzistentni.)
                const auto live = ithaca::MidiInput::listPorts();
                int idx = -1;
                for (size_t k = 0; k < live.size(); ++k)
                    if (live[k] == name) { idx = (int)k; break; }
                if (idx >= 0) {
                    ctx.midi.close();
                    ctx.midi.setChannel(ctx.state.midi_channel);   // pred open
                    if (ctx.midi.open(ctx.engine, idx))
                        ctx.state.midi_port_name = name;
                } else {
                    log::Logger::default_().log("gui", log::Severity::Warning,
                        "MIDI port zmizel: %s", name.c_str());
                    ps.midi_ports = live;
                }
            }
        }
        ImGui::EndCombo();
    } else {
        ps.midi_combo_open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("RESCAN##reload"))
        ps.midi_ports = ithaca::MidiInput::listPorts();
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

    // Prava skupina: LOG level + RELOAD + RESET. RELOAD byl drive dole v BANK
    // panelu; obe akcni tlacitka jsou ted vedle sebe na horni liste.
    //
    // Sirku skupiny MERIME z obsahu, nedrzime ji v konstante: pevna hodnota se
    // pri pridani tlacitka rozejde se skutecnosti a skupina zacne lezt do
    // BUFFERu vlevo (presne to se stalo, kdyz k RESETu pribyl RELOAD).
    // Clamp na konec leve skupiny navic zajisti, ze se v uzkem okne nic
    // nepreklopi pres sebe — skupina se nanejvys prilepi hned za BUFFER.
    auto btnW = [](const char* t) {
        return ImGui::CalcTextSize(t).x + ImGui::GetStyle().FramePadding.x * 2.f;
    };
    constexpr float kGapLogBtn = 16.f, kGapBtnBtn = 10.f;
    const float group_w = ImGui::CalcTextSize("LOG").x
                        + ImGui::GetStyle().ItemSpacing.x
                        + L::Dims::tb_log_w
                        + kGapLogBtn + btnW("RELOAD")
                        + kGapBtnBtn + btnW("RESET");
    ImGui::SameLine();
    const float after_left = ImGui::GetCursorPosX();   // konec leve skupiny
    ImGui::SetCursorPosX(std::max(after_left,
                                  ImGui::GetWindowWidth() - group_w));
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
    ImGui::SameLine(0, kGapLogBtn);
    // RELOAD: znovu nacte aktualne vybranou banku (async, prubeh kryje modalni
    // overlay). Bez vybrane banky nema co delat → disabled, aby bylo videt proc.
    {
        const bool has_bank = !ctx.state.bank_path.empty();
        ImGui::BeginDisabled(!has_bank);
        if (ImGui::Button("RELOAD")) ctx.requestBankReload(ctx.state.bank_path);
        ImGui::EndDisabled();
    }

    ImGui::SameLine(0, kGapBtnBtn);
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
