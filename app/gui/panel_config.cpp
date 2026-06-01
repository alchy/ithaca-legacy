// app/gui/panel_config.cpp - viz panel_config.h.
#include "panel_config.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "layout.h"
#include "imgui.h"
#include <string>

namespace ithaca::gui {

void renderConfigPanel(AppContext& ctx, ithaca::dsp::IParamPage** pages, int n, int& selected) {
    (void)ctx;
    using theme::Colors;
    namespace L = ithaca::gui::layout;
    ImGui::Dummy({0, 4}); ImGui::Indent(L::Dims::pad_panel);
    wdg::Eyebrow("CONFIG", Colors::silver2);
    ImGui::Dummy({0, L::Dims::row_gap});

    auto* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < n; ++i) {
        ithaca::dsp::IParamPage* p = pages[i];
        const bool sel = (i == selected);
        const bool on  = p->enabled();   // VOICE vraci true
        ImVec2 pos = ImGui::GetCursorScreenPos();
        // LED kolecko (zlate = on, tlumene = off).
        dl->AddCircleFilled({pos.x + 4.f, pos.y + 9.f}, 3.5f,
                            on ? Colors::gold : Colors::line);
        // Nazev (zlaty kdyz vybrany, jinak ink). Kreslime po LED.
        const float w = ImGui::GetContentRegionAvail().x - L::Dims::pad_panel;
        ImGui::SetCursorScreenPos({pos.x + 16.f, pos.y});
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(sel ? Colors::gold : Colors::ink));
        ImGui::TextUnformatted(p->name());
        ImGui::PopStyleColor();
        // Klikatelny radek pres celou sirku (vizual uz vykreslen, tohle resi klik).
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton((std::string("##cfg_") + p->name()).c_str(),
                                   {w, 20.f}))
            selected = i;
        ImGui::Dummy({0, L::Dims::row_gap_s});
    }
    ImGui::Unindent(L::Dims::pad_panel);
}

} // namespace ithaca::gui
