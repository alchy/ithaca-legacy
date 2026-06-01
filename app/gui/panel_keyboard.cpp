// app/gui/panel_keyboard.cpp - 88-key piano keyboard (MIDI 21..108).
// Renderuje pres wdg::Keyboard (Art Deco styl): aktivni noty zlate, sympaticky
// rezonujici tlumene. Bez popisku (rozsah/sustain ukazuje strip).
#include "panel_keyboard.h"
#include "app_context.h"
#include "widgets.h"
#include "layout.h"
#include "imgui.h"

namespace ithaca::gui {

void renderKeyboardPanel(AppContext& ctx) {
    namespace L = ithaca::gui::layout;
    bool active[128];  ctx.engine.activeMidiNotes(active);
    bool reso[128];    ctx.engine.resonatingMidiNotes(reso);
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy({0,4});
    wdg::Keyboard(w, L::Dims::kbd_keys_h,
                  [&](int m){ return m>=0 && m<128 && active[m]; },   // primarni (zlate)
                  [&](int m){ return m>=0 && m<128 && reso[m]; });    // rezonujici (tlumene)
}

} // namespace ithaca::gui
