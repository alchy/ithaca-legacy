#include "panel_dsp.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "layout.h"
#include "imgui.h"

namespace ithaca::gui {

void renderDspRack(AppContext& ctx) {
    using theme::Colors;
    namespace L = ithaca::gui::layout;
    (void)ctx;
    const float pad = L::Dims::pad_panel;
    ImGui::Dummy({0,4}); ImGui::Indent(pad);
    wdg::Eyebrow("DSP RACK", Colors::silver2);
    ImGui::Dummy({0, L::Dims::row_gap});

    struct Mod { const char* name; const char* val; bool on; };
    // FUTURE: realne moduly + ovladace z DSP chain featury (AGC/conv/BBE/limiter).
    const Mod mods[] = {
        {"AGC", "-2.1 dB", true},
        {"CONVOLVER", "body \xC2\xB7 38%", true},
        {"BBE", "+ presence", true},
        {"LIMITER", "off", false},
    };
    // Modul = DVA radky: [LED] NAZEV (radek 1, silver/gold) + hodnota pod nim
    // mensim eyebrow fontem (radek 2, muted). Dvouradkove, protoze dlouhe nazvy
    // (CONVOLVER) se na ~290px sloupci nevejdou vedle hodnoty na jeden radek.
    // Kreslime absolutnimi screen coords pres draw list (spolehlive).
    const float avail = ImGui::GetContentRegionAvail().x - pad;
    for (const auto& m : mods) {
        // radek 1: LED (draw list) + nazev (normalni text, body font).
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            {p.x + 4.f, p.y + 9.f}, 3.5f, m.on ? Colors::gold : Colors::line);
        ImGui::Indent(18.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(m.on ? Colors::silver : Colors::muted));
        ImGui::TextUnformatted(m.name);
        ImGui::PopStyleColor();
        // radek 2: hodnota — eyebrow font pres normalni Text (respektuje
        // FontGlobalScale; draw-list s rucni velikosti delal mikro-text).
        wdg::Eyebrow(m.val, Colors::muted);
        ImGui::Unindent(18.f);
        ImGui::Dummy({avail, L::Dims::row_gap_s});  // mezera mezi moduly
    }
    ImGui::Dummy({0, L::Dims::row_gap_s});
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::line));
    if (theme::Fonts::eyebrow) ImGui::PushFont(theme::Fonts::eyebrow);
    ImGui::TextUnformatted("signal > AGC > conv > bbe > lim > out");
    if (theme::Fonts::eyebrow) ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Unindent(pad);
}

} // namespace ithaca::gui
