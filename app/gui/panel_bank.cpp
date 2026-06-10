// app/gui/panel_bank.cpp - BANK sloupec (levy). Select banky + TYPE badge
// TYPE badge = autodetekovany format (FIXED/DYNAMIC) z ctx.engine.bankType() + fakta
// o bance + RELOAD. Kresli do ##bank childu.
#include "panel_bank.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "imgui.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace ithaca::gui {

namespace {
// Podadresare ve search_root = kandidati na banku.
std::vector<std::string> scanBanks(const std::string& search_root) {
    std::vector<std::string> out;
    if (search_root.empty()) return out;
    std::error_code ec;
    if (!std::filesystem::is_directory(search_root, ec)) return out;
    for (const auto& e : std::filesystem::directory_iterator(search_root, ec)) {
        if (e.is_directory()) out.push_back(e.path().string());
    }
    return out;
}
} // namespace

void renderBankPanel(AppContext& ctx) {
    using theme::Colors;
    const float pad = 14.f;
    ImGui::Dummy({0, 4});
    ImGui::Indent(pad);

    // Sekce titulek
    wdg::Eyebrow("BANK", Colors::silver2);
    ImGui::Dummy({0, 6});

    // Scan kandidatu (cache dle search_root).
    static std::vector<std::string> cands;
    static std::string last_root;
    std::string root = !ctx.state.bank_search_dir.empty()
        ? ctx.state.bank_search_dir
        : (ctx.state.bank_path.empty() ? std::string("")
           : std::filesystem::path(ctx.state.bank_path).parent_path().string());
    if (root != last_root) { cands = scanBanks(root); last_root = root; }

    // Bank dropdown
    std::string curr = ctx.state.bank_path.empty() ? std::string("(none)")
        : std::filesystem::path(ctx.state.bank_path).filename().string();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - pad);
    if (ImGui::BeginCombo("##bank", curr.c_str())) {
        for (const auto& b : cands) {
            std::string label = std::filesystem::path(b).filename().string();
            bool sel = (b == ctx.state.bank_path);
            if (ImGui::Selectable(label.c_str(), sel)) {
                if (b != ctx.state.bank_path) {
                    ctx.state.bank_path = b;
                    ctx.requestBankReload(b);   // async; modal overlay viz main.cpp
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy({0, 8});

    // TYPE badge (read-only) — autodetekovany format banky z engine.
    const char* type_label = "—";
    switch (ctx.engine.bankType()) {
        case BankFormat::FixedVelocity:   type_label = "FIXED";   break;
        case BankFormat::DynamicVelocity: type_label = "DYNAMIC"; break;
        case BankFormat::Extended:        type_label = "EXTENDED"; break;
        case BankFormat::Unknown:         type_label = "—";       break;
    }
    wdg::Eyebrow("TYPE"); ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::gold));
    ImGui::TextUnformatted(type_label);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("\xC2\xB7 auto");   // · auto
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 8});

    // Fakta o bance — realna cisla z engine. Pocet velocity vrstev se neuvadi
    // (u dynamic-velocity je per nota promenny); staci pocet not a samplu.
    char facts[48];
    std::snprintf(facts, sizeof(facts), "%d not \xC2\xB7 %d samplu",
                  ctx.engine.recordedNotes(), ctx.engine.loadedSamples());
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted(facts);
    ImGui::PopStyleColor();
    if (ctx.bank_truncated_) {
        // Posledni load prekrocil RAM budget → banka neuplna (detail v LOG).
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::gold));
        ImGui::TextUnformatted("NEUPLNA (RAM limit)");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy({0, 10});

    // RELOAD
    if (ImGui::Button("RELOAD",
                      ImVec2(ImGui::GetContentRegionAvail().x - pad, 0))) {
        if (!ctx.state.bank_path.empty()) ctx.requestBankReload(ctx.state.bank_path);
    }

    ImGui::Unindent(pad);
}

} // namespace ithaca::gui
