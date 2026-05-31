// app/gui/panel_topbar.cpp - viz panel_topbar.h.
#include "panel_topbar.h"
#include "app_context.h"
#include "midi/midi_input.h"
#include "imgui.h"
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace ithaca::gui {

namespace {

// Spocte seznam podadresaru (kandidati na banku) v adresari nad aktualni cestou.
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

void renderTopBar(AppContext& ctx) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)ctx.state.window_w, 36.f});
    ImGui::Begin("##topbar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    // Bank dropdown - search nad aktualni cestou (parent dir).
    static std::vector<std::string> bank_candidates;
    static std::string              last_root;
    std::string search_root = ctx.state.bank_path.empty()
        ? "" : std::filesystem::path(ctx.state.bank_path).parent_path().string();
    if (search_root != last_root) {
        bank_candidates = scanBanks(search_root);
        last_root = search_root;
    }

    ImGui::Text("Bank:"); ImGui::SameLine();
    std::string curr_bank_label = ctx.state.bank_path.empty()
        ? std::string("(none)")
        : std::filesystem::path(ctx.state.bank_path).filename().string();
    ImGui::PushItemWidth(280);
    if (ImGui::BeginCombo("##bank", curr_bank_label.c_str())) {
        for (const auto& b : bank_candidates) {
            const std::string label = std::filesystem::path(b).filename().string();
            bool sel = (b == ctx.state.bank_path);
            if (ImGui::Selectable(label.c_str(), sel)) {
                if (b != ctx.state.bank_path) {
                    ctx.state.bank_path = b;
                    // Sync load - UI freezne na ~200ms (acceptable MVP).
                    ctx.engine.loadBank(b);
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    // MIDI dropdown - vsechny porty z RtMidi.
    ImGui::SameLine();
    ImGui::Text("MIDI:"); ImGui::SameLine();
    auto ports = ithaca::MidiInput::listPorts();
    const char* curr_midi = ctx.state.midi_port_name.empty() ? "(none)"
                          : ctx.state.midi_port_name.c_str();
    ImGui::PushItemWidth(220);
    if (ImGui::BeginCombo("##midi", curr_midi)) {
        if (ImGui::Selectable("(none)", ctx.state.midi_port_name.empty())) {
            ctx.midi.close();
            ctx.state.midi_port_name.clear();
        }
        for (size_t i = 0; i < ports.size(); ++i) {
            const auto& p = ports[i];
            bool sel = (p == ctx.state.midi_port_name);
            if (ImGui::Selectable(p.c_str(), sel)) {
                if (p != ctx.state.midi_port_name) {
                    ctx.midi.close();
                    if (ctx.midi.open(ctx.engine, (int)i)) {
                        ctx.state.midi_port_name = p;
                    }
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    // Master slider - vpravo (zarovnani na konec okna).
    ImGui::SameLine();
    const float right_margin = 280.f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - right_margin);
    ImGui::Text("Master:"); ImGui::SameLine();
    ImGui::PushItemWidth(180);
    if (ImGui::SliderFloat("##master", &ctx.state.master_gain_db,
                            -60.f, 6.f, "%.1f dB")) {
        const float g = std::pow(10.f, ctx.state.master_gain_db / 20.f);
        ctx.engine.setMasterGain(g);
    }
    ImGui::PopItemWidth();

    ImGui::End();
}

} // namespace ithaca::gui
