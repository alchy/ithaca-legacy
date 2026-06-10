// app/gui/panel_log.cpp - scrollable list poslednich log eventu z LogRingBuffer.
//
// Snapshot 50 nejnovejsich zaznamu (lokalni kopie na stacku/staticky array, takze
// nikdo nedrzi mutex bufferu behem renderu). Barva podle severity: Info=seda,
// Warning=zluta, Error/Fatal=cervena. Auto-scroll k novemu eventu jen kdyz user
// je na konci — pri rucnim scrollovani nahoru se nesnaze "uchytit" nove zpravy.
#include "panel_log.h"
#include "app_context.h"
#include "theme.h"
#include "imgui.h"
#include <array>

namespace ithaca::gui {

void renderLogPanel(AppContext& ctx) {
    using theme::Colors;
    static std::array<log::LogEntry, 50> tmp;
    const int n = ctx.log_buf.snapshot(tmp.data(), (int)tmp.size());
    if (ImGui::BeginChild("##loglist", {0,0}, false, ImGuiWindowFlags_HorizontalScrollbar)) {
        for (int i = 0; i < n; ++i) {
            const auto& e = tmp[i];
            ImU32 col = Colors::muted;
            if (e.sev == log::Severity::Warning) col = Colors::gold;
            if ((int)e.sev >= (int)log::Severity::Error)   // Error i Fatal cervene
                col = IM_COL32(0xd0,0x5a,0x4a,255);
            ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(col));
            ImGui::Text("[%s] %s", e.topic.c_str(), e.message.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f) ImGui::SetScrollHereY(1.f);
    }
    ImGui::EndChild();
    // (zadny ImGui::End() — kreslime do ##log childu z main.cpp)
}

} // namespace ithaca::gui
