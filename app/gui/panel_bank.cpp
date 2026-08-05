// app/gui/panel_bank.cpp - BANK sloupec (levy). Select banky + TYPE badge
// TYPE badge = autodetekovany format (FIXED/DYNAMIC) z ctx.engine.bankType() + fakta
// o bance + RELOAD. Kresli do ##bank childu.
#include "panel_bank.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "layout.h"
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
    const float pad = layout::Dims::pad_inset;
    ImGui::Dummy({0, 4});
    ImGui::Indent(pad);

    // Sekce titulek
    wdg::Eyebrow("BANK", Colors::silver2);
    ImGui::Dummy({0, 6});

    // Scan kandidatu. Cache zije v ctx.panels (drive function-local static).
    auto& ps = ctx.panels;
    std::string root = !ctx.state.bank_search_dir.empty()
        ? ctx.state.bank_search_dir
        : (ctx.state.bank_path.empty() ? std::string("")
           : std::filesystem::path(ctx.state.bank_path).parent_path().string());
    if (!ps.bank_cands_valid || root != ps.bank_cands_root) {
        ps.bank_cands = scanBanks(root);
        ps.bank_cands_root = root;
        ps.bank_cands_valid = true;
    }

    // Bank dropdown
    std::string curr = ctx.state.bank_path.empty() ? std::string("(none)")
        : std::filesystem::path(ctx.state.bank_path).filename().string();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - pad);
    if (ImGui::BeginCombo("##bank", curr.c_str())) {
        // Rescan pri OTEVRENI comba (1x per open, stejne jako MIDI dropdown).
        // Drive se scanovalo jen pri zmene rootu, takze cerstve zkopirovana
        // banka se v seznamu neobjevila az do restartu aplikace.
        if (!ps.bank_combo_open) {
            ps.bank_cands = scanBanks(root);
            ps.bank_combo_open = true;
        }
        for (const auto& b : ps.bank_cands) {
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
    } else {
        ps.bank_combo_open = false;
    }
    ImGui::Dummy({0, 8});

    // TYPE badge (read-only) — autodetekovany format banky z engine.
    const char* type_label = "—";
    switch (ctx.engine.bankType()) {
        case BankFormat::FixedVelocity:   type_label = "FIXED";   break;
        case BankFormat::DynamicVelocity: type_label = "DYNAMIC"; break;
        case BankFormat::Extended:        type_label = "EXTENDED"; break;
        case BankFormat::PackedIthaca:    type_label = "PACKED";  break;
        case BankFormat::Unknown:         type_label = "—";       break;
    }
    // TYPE (eyebrow 11 px) + hodnota (body 18 px) + "· auto" na JEDNOM radku:
    // pres SameLine by kazdy sedel na jine uctare a text by poskakoval.
    const wdg::Span type_row[] = {
        {"TYPE",            theme::Fonts::eyebrow, Colors::muted},
        {type_label,        theme::Fonts::body,    Colors::gold},
        {"\xC2\xB7 auto",   theme::Fonts::body,    Colors::muted},   // · auto
    };
    wdg::TextRow(type_row, IM_ARRAYSIZE(type_row));
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
    // RELOAD se presunul do horni listy vedle RESET (obe akcni tlacitka
    // pohromade), viz panel_topbar.

    ImGui::Unindent(pad);
}

} // namespace ithaca::gui
