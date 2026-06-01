#include "panel_dsp.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "imgui.h"

namespace ithaca::gui {

void renderDspRack(AppContext& ctx) {
    using theme::Colors;
    (void)ctx;
    ImGui::Dummy({0,4}); ImGui::Indent(14.f);
    wdg::Eyebrow("DSP RACK", Colors::silver2);
    ImGui::Dummy({0,8});
    struct Mod { const char* name; const char* val; bool on; };
    // FUTURE: realne moduly + ovladace z DSP chain featury (AGC/conv/BBE/limiter).
    const Mod mods[] = {
        {"AGC", "-2.1 dB", true},
        {"CONVOLVER", "body \xC2\xB7 38%", true},
        {"BBE", "+ presence", true},
        {"LIMITER", "off", false},
    };
    for (const auto& m : mods) {
        ImU32 led = m.on ? Colors::gold : Colors::line;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled({p.x+4, p.y+8}, 3.5f, led);
        ImGui::Dummy({14,16}); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(m.on?Colors::silver:Colors::muted));
        ImGui::TextUnformatted(m.name);
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.f);
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
        ImGui::TextUnformatted(m.val);
        ImGui::PopStyleColor();
        ImGui::Dummy({0,4});
    }
    ImGui::Dummy({0,4});
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::line));
    if (theme::Fonts::eyebrow) ImGui::PushFont(theme::Fonts::eyebrow);
    ImGui::TextUnformatted("signal > AGC > conv > bbe > lim > out");
    if (theme::Fonts::eyebrow) ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Unindent(14.f);
}

} // namespace ithaca::gui
