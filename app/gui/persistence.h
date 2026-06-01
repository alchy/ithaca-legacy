// app/gui/persistence.h - JSON load/save GUI state.
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace ithaca::gui {

struct GuiState {
    int         schema_version    = 4;
    // Adresar, ve kterem se hleda banky (dropdown ho scanu). Pri prazdnem
    // bank_path je tohle jediny zdroj kandidatu — bez ne by uzivatel nemel
    // jak vybrat banku z GUI. Settable pres --bank-dir CLI flag nebo
    // dropdown (Browse... v budoucnu).
    std::string bank_search_dir;
    std::string bank_path;
    std::string midi_port_name;
    std::string log_level         = "info";   // debug|info|warn|error|fatal
    int         midi_channel      = -1;   // -1 = OMNI, 0..15 = MIDI kanal (0-based)
    float       master_gain_db      = 0.f;
    float       resonance_strength  = 0.5f;
    float       release_ms          = 200.f;
    float       excite_decay_ms     = 5000.f;
    int         max_resonance_voices = 32;
    // -- DSP chain (defaulty = zadna zmena chovani: vsechny stage vyplé) --
    bool  agc_enabled = false;     float agc_target = 0.15f;  float agc_release_ms = 200.f;  float agc_floor = 0.05f;
    bool  bbe_enabled = false;     float bbe_definition = 0.f; float bbe_bass = 0.f;
    bool  limiter_enabled = false; float limiter_threshold_db = 0.f; float limiter_release_ms = 200.f;
    int   config_page = 0;         // 0 = VOICE, 1 = AGC, 2 = BBE, 3 = LIMITER
    int         window_x = 100, window_y = 100;
    int         window_w = 1280, window_h = 820;
};

// Najit cestu k state.json podle OS:
//  macOS: $HOME/Library/Application Support/ithaca-legacy/state.json
//  Linux: $XDG_CONFIG_HOME/ithaca-legacy/state.json (fallback $HOME/.config/...)
//  Win:   %APPDATA%/ithaca-legacy/state.json
std::filesystem::path defaultStatePath();

// Nacti state z path. Vraci nullopt pri chybe (missing/invalid/wrong version).
std::optional<GuiState> loadState(const std::filesystem::path& path);

// Atomic write: zapis do path.tmp + rename. Vraci true pri uspechu.
bool saveState(const std::filesystem::path& path, const GuiState& s);

} // namespace ithaca::gui
