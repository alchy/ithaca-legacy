// app/gui/persistence.h - JSON load/save GUI state.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace ithaca::gui {

// Stav jedne DSP stage. Parametry jsou klicovane pres Param::id (stabilni klic
// urceny prave pro persistenci) — pridani parametru do stage tedy nevyzaduje
// ZADNOU zmenu tady ani v load/save. Drive bylo kazde pole vypsane zvlast na
// 6 mistech (GuiState, loadState, saveState, initFromState, zrcadleni, debounce).
struct DspStageState {
    bool enabled = false;
    int  choice  = -1;                     // -1 = stage nema volic (jen Convolver ma)
    std::map<std::string, float> params;   // Param::id -> hodnota
    bool operator==(const DspStageState&) const = default;
};

// Geometrie okna. Vydelena zvlast, protoze se meni kazdy frame (tazeni/resize)
// a do persistence debounce nepatri — uklada se az pri shutdownu.
struct WindowGeom {
    int x = 100, y = 100;
    int w = 1280, h = 720;                 // HW cilovy display 1280x720
    bool operator==(const WindowGeom&) const = default;
};

struct GuiState {
    int         schema_version    = 7;
    // Adresar, ve kterem se hleda banky (dropdown ho scanu). Pri prazdnem
    // bank_path je tohle jediny zdroj kandidatu — bez ne by uzivatel nemel
    // jak vybrat banku z GUI. Settable pres --bank-dir CLI flag nebo
    // dropdown (Browse... v budoucnu).
    std::string bank_search_dir;
    std::string bank_path;
    std::string midi_port_name;
    std::string log_level         = "info";   // debug|info|warn|error|fatal
    // Maska prijimanych MIDI kanalu: bit i = kanal i+1. 0xFFFF = vsechny
    // (drive OMNI). Maska misto jednoho indexu proto, ze na panelu jde zapnout
    // libovolna podmnozina — OMNI uz neni zvlastni polozka nabidky, uzivatel
    // si ho naklika zapnutim vsech kanalu.
    uint16_t    midi_channel_mask = 0xFFFFu;
    float       master_gain_db      = 0.f;
    bool        resonance_enabled  = true;
    float       resonance_gain_db  = -12.f;
    float       resonance_layer_db = -30.f;
    float       release_ms          = 200.f;
    float       excite_decay_ms     = 5000.f;
    int         max_resonance_voices = 32;
    int         resonance_window_ms  = 12000; // RAM cache rezonance [ms]; jen JSON (ne GUI)
    int         preload_ms           = 150;   // preload hlavy samplu [ms]; jen JSON (ne GUI)
    int         cache_budget_mb      = 0;     // RAM budget banky [MB], 0=auto; jen JSON (ne GUI)
    int   config_page = 0;         // 0 = MASTER, 1 = RESONANCE, 2 = CONVOLVER, 3 = AGC, 4 = ENHANCER, 5 = LIMITER
    // -- Zar vlny v pozadi --
    // Nasobitel DOSAHU zare (zakladni dosah je ~10 px u nejsirsi stuhy):
    //   0    = hola cara bez zare, nejlevnejsi rezim
    //   1    = vychozi vzhled
    //   >1   = sirsi rozostreni; 100 je uz pruh pres cely displej
    // Dosah je jedina vec na panelu, ktera roste s VYPLNI, takze je to hlavni
    // paka pro slabsi grafiku. Pocet drah profilu se dopocita sam (glowLanes).
    // POZOR pri nastavovani: hodnota MUSI projit sanitizeGlow(). state.json se
    // edituje rucne a CLI bere cokoli, takze sem muze prijit i NaN nebo 1e9.
    float wave_glow      = 1.f;
    // Strop, kolik smi kresleni stuh stat na snimek [ms]. Kdyz se prekroci,
    // regulator dosah SNIZI (viz glow_auto.h). 0 = automatika vypnuta.
    float wave_glow_budget_ms = 0.f;
    // -- Audio (Faze 8) --
    int   audio_block_size  = 256;    // runtime-menitelny z GUI (BUFFER combo)
    int   audio_sample_rate = 48000;  // jen z JSONu; GUI zobrazuje read-only

    // Stav celeho DSP chainu, klicovany jmenem stage ("CONVOLVER", "AGC", ...).
    // Nahradilo 16 plochych poli (agc_target, convolver_mix, ...) — ta se pri
    // pridani parametru musela rucne doplnit na 6 mistech. Plni/aplikuje se
    // genericky pres dspStateFromChain()/applyDspStateToChain(), viz dsp_state.h.
    // Prazdna mapa = stage si drzi vlastni defaulty (vsechny vyple).
    std::map<std::string, DspStageState> dsp;

    // UZIVATELSKE vychozi hodnoty — to, na co vraci RESET PARAMS na strance SYS.
    // Neplest s Param::def: ten je TOVARNI konstanta v kodu a o konkretni bance
    // ani sestave nic nevi. Tohle je snapshot, ktery si uzivatel poridi tlacitkem
    // SAVE AS DEFAULT ve chvili, kdy nastroj zni tak, jak ma.
    //
    // Klicem je jmeno IParamPage ("MASTER", "RESONANCE", "CONVOLVER", ...), takze
    // pokryva VSECHNY stranky parametru stejnym mechanismem jako `dsp` — vcetne
    // MASTER a RESONANCE, ktere zadna DSP stage nejsou. Prazdna mapa = uzivatel
    // si zadne neulozil a RESET jede na tovarni Param::def.
    std::map<std::string, DspStageState> defaults;

    WindowGeom window;

    // Rovnost vsech persistovanych poli. Persistence debounce v main.cpp ji
    // pouziva misto rucniho retezce porovnani (ten drive vynechaval
    // bank_search_dir a pri pridani pole se na nej snadno zapomnelo).
    bool operator==(const GuiState&) const = default;
};

// Ocisteni vzhledovych parametru zare. JEDNO misto, kterym musi projit vsechno,
// co prijde zvenci — state.json (rucne editovatelny) i CLI.
//
// Nestaci std::clamp: ten pri NaN vraci NaN, protoze vsechna porovnani s NaN
// jsou nepravdiva. NaN by se propsal do souradnic vrcholu vlny.
inline float sanitizeGlow(float v, float max_v) {
    if (!(v >= 0.f)) return 0.f;              // chyti NaN i zapornou hodnotu
    return (v > max_v) ? max_v : v;
}
inline constexpr float kWaveGlowMax   = 128.f;   // ~1300 px dosahu, vic nez panel
inline constexpr float kWaveBudgetMax = 1000.f;  // 1 s na snimek uz neni rozpocet

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
