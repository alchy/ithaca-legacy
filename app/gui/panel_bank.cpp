// app/gui/panel_bank.cpp - BANK sloupec (levy). Select banky + TYPE badge
// (zatim hardcoded LEGACY — autodetekce je samostatny engine task) + fakta
// o bance + RELOAD. Kresli do ##bank childu.
#include "panel_bank.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "imgui.h"
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

    // SELECT dropdown
    wdg::Eyebrow("SELECT");
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
                    ctx.engine.reloadBank(b);   // safe drain→silence→load→resume
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy({0, 8});

    // TYPE badge (read-only). FUTURE: ctx.engine.bankType() az bude folder-type
    // loader + autodetekce; zatim podporujeme jen legacy.
    wdg::Eyebrow("TYPE"); ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::gold));
    ImGui::TextUnformatted("LEGACY");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("\xC2\xB7 auto");   // · auto
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 8});

    // Fakta o bance. FUTURE: vytahnout realna cisla z engine (pocet samplu,
    // RAM, SR) az bude API; zatim staticke + co lze (jen orientacni).
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("8 velocity vrstev");
    ImGui::TextUnformatted("88 not \xC2\xB7 48 kHz");
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 10});

    // RELOAD
    if (ImGui::Button("RELOAD",
                      ImVec2(ImGui::GetContentRegionAvail().x - pad, 0))) {
        if (!ctx.state.bank_path.empty()) ctx.engine.reloadBank(ctx.state.bank_path);
    }

    ImGui::Unindent(pad);
}

} // namespace ithaca::gui
