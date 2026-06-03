// app/gui/panel_params.cpp - genericky renderer libovolne IParamPage (VOICE i DSP
// stage). Nadpis = page.name(); volitelny ON/OFF toggle; smycka DecoSlideru pres
// parametry (readonly -> disabled); volitelny metr na konci.
#include "panel_params.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "layout.h"
#include "imgui.h"
#include <cstdio>

namespace ithaca::gui {

void renderParamPage(AppContext& ctx, ithaca::dsp::IParamPage& page) {
    (void)ctx;
    using theme::Colors;
    namespace L = ithaca::gui::layout;
    ImGui::Dummy({0, 4}); ImGui::Indent(L::Dims::pad_panel);
    wdg::Eyebrow(page.name(), Colors::silver2);
    ImGui::Dummy({0, L::Dims::row_gap});

    if (page.hasEnable()) {
        if (wdg::ToggleChip(page.name(), page.enabled()))
            page.setEnabled(!page.enabled());
        // Volic (napr. IR typ u Convolveru) hned vedle ON/OFF — kompaktni, setri misto.
        if (page.choiceCount() > 0) {
            ImGui::SameLine(0, 12);
            int cur = page.currentChoice();
            const char* curName = (cur >= 0) ? page.choiceName(cur) : "(none)";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - L::Dims::pad_panel);
            if (ImGui::BeginCombo("##choice", curName)) {
                for (int i = 0; i < page.choiceCount(); ++i)
                    if (ImGui::Selectable(page.choiceName(i), i == cur) && i != cur)
                        page.selectChoice(i);
                ImGui::EndCombo();
            }
        }
        ImGui::Dummy({0, L::Dims::row_gap});
    }

    for (int i = 0; i < page.paramCount(); ++i) {
        const auto& p = page.param(i);
        float v = page.get(i);
        const ImU32 accent = (i == 0) ? Colors::gold : Colors::silver2;
        if (wdg::DecoSlider(p.label, &v, p.min, p.max, p.fmt, accent, /*enabled=*/!p.readonly))
            page.set(i, v);
        ImGui::Dummy({0, L::Dims::row_gap});
    }

    // Fallback: stranka s volicem ale bez ON/OFF → volic dole (zadna takova teď neni).
    if (!page.hasEnable() && page.choiceCount() > 0) {
        ImGui::Dummy({0, L::Dims::row_gap});
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
        ImGui::TextUnformatted(page.choiceLabel());
        ImGui::PopStyleColor();
        int cur = page.currentChoice();
        const char* curName = (cur >= 0) ? page.choiceName(cur) : "(none)";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - L::Dims::pad_panel);
        if (ImGui::BeginCombo("##choice", curName)) {
            for (int i = 0; i < page.choiceCount(); ++i)
                if (ImGui::Selectable(page.choiceName(i), i == cur) && i != cur)
                    page.selectChoice(i);
            ImGui::EndCombo();
        }
    }

    float mv; const char* ml;
    if (page.meter(mv, ml)) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.2f", mv);
        wdg::Eyebrow(ml);
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::ink));
        ImGui::TextUnformatted(buf);
        ImGui::PopStyleColor();
    }
    ImGui::Unindent(L::Dims::pad_panel);
}

} // namespace ithaca::gui
