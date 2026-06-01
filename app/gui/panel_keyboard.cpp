// app/gui/panel_keyboard.cpp - 88-key piano keyboard (MIDI 21..108).
// Renderuje pres wdg::Keyboard (Art Deco styl), aktivni noty zlate.
// Popisek pod klaviaturou ukazuje rozsah + aktualni sustain CC.
#include "panel_keyboard.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "imgui.h"
#include <cstdio>

namespace ithaca::gui {

void renderKeyboardPanel(AppContext& ctx) {
    using theme::Colors;
    bool active[128]; ctx.engine.activeMidiNotes(active);
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy({0,4});
    wdg::Keyboard(w, 44.f, [&](int m){ return m>=0 && m<128 && active[m]; });
    // popisek pod klaviaturou
    const int cc = (int)ctx.engine.pedalCC();
    char cap[48];
    std::snprintf(cap, sizeof(cap), "A0  \xE2\x80\x94  SUSTAIN %d  \xE2\x80\x94  C8", cc);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    if (theme::Fonts::eyebrow) ImGui::PushFont(theme::Fonts::eyebrow);
    float tw = ImGui::CalcTextSize(cap).x;
    ImGui::SetCursorPosX((w - tw) * 0.5f);
    ImGui::TextUnformatted(cap);
    if (theme::Fonts::eyebrow) ImGui::PopFont();
    ImGui::PopStyleColor();
}

} // namespace ithaca::gui
