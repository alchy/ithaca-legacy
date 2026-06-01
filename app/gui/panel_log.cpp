// app/gui/panel_log.cpp - scrollable list poslednich log eventu z LogRingBuffer.
//
// Snapshot 50 nejnovejsich zaznamu (lokalni kopie na stacku/staticky array, takze
// nikdo nedrzi mutex bufferu behem renderu). Barva podle severity: Info=seda,
// Warning=zluta, Error=cervena. Auto-scroll k novemu eventu jen kdyz user je na
// konci — pri rucnim scrollovani nahoru se nesnaze "uchytit" nove zpravy.
#include "panel_log.h"
#include "app_context.h"
#include "imgui.h"
#include <array>

namespace ithaca::gui {

void renderLogPanel(AppContext& ctx) {
    // Snapshot poslednich 50 zaznamu (lokalni copy, GUI thread).
    static std::array<log::LogEntry, 50> tmp;
    const int n = ctx.log_buf.snapshot(tmp.data(), (int)tmp.size());

    if (ImGui::BeginChild("##loglist", {0, 0}, false,
            ImGuiWindowFlags_HorizontalScrollbar)) {
        for (int i = 0; i < n; ++i) {
            const auto& e = tmp[i];
            ImVec4 col = ImVec4(0.7f, 0.7f, 0.7f, 1.f);
            if (e.sev == log::Severity::Warning) col = ImVec4(1.f, 0.85f, 0.3f, 1.f);
            if (e.sev == log::Severity::Error)   col = ImVec4(1.f, 0.4f, 0.4f, 1.f);
            ImGui::TextColored(col, "[%s] %s", e.topic.c_str(), e.message.c_str());
        }
        // Auto-scroll k novemu eventu kdyz uzivatel je na konci (jinak respektuj scroll).
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f) {
            ImGui::SetScrollHereY(1.f);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace ithaca::gui
