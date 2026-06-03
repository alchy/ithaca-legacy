// app/gui/persistence.cpp - viz persistence.h.
//
// Mini JSON: flat key:value bez nesting. Pro slozitejsi schema (vnorene
// objekty, pole) by se hodilo vendorovat nlohmann/json, ale na nasich 12
// klicu staci primitivni parser.
#include "persistence.h"
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ithaca::gui {

namespace {

// Konfigracni adresar per OS.
std::filesystem::path platformConfigDir() {
#ifdef __APPLE__
    if (const char* h = std::getenv("HOME"))
        return std::filesystem::path(h) / "Library" / "Application Support";
#elif defined(_WIN32)
    if (const char* a = std::getenv("APPDATA"))
        return std::filesystem::path(a);
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"))
        return std::filesystem::path(x);
    if (const char* h = std::getenv("HOME"))
        return std::filesystem::path(h) / ".config";
#endif
    return std::filesystem::current_path();
}

// Escape JSON stringu (jen \", \\, \n).
std::string jsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            default:   o += c;
        }
    }
    return o;
}

// Primitivni vyhledani hodnoty pro klic v flat JSONu. Vraci raw string
// hodnoty (bez quotes a escape resolve pro stringy provedeny).
// Pri chybe vraci prazdny string.
std::string findValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return {};
    size_t c = json.find(':', k);
    if (c == std::string::npos) return {};
    ++c;
    while (c < json.size() && std::isspace((unsigned char)json[c])) ++c;
    if (c >= json.size()) return {};
    // String value
    if (json[c] == '"') {
        size_t e = c + 1;
        std::string v;
        while (e < json.size() && json[e] != '"') {
            if (json[e] == '\\' && e + 1 < json.size()) {
                char nxt = json[e + 1];
                if (nxt == 'n') v += '\n';
                else v += nxt;
                e += 2;
            } else {
                v += json[e++];
            }
        }
        return v;
    }
    // Numeric value (do , nebo } nebo whitespace)
    size_t e = c;
    while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != '\n')
        ++e;
    std::string v = json.substr(c, e - c);
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
    return v;
}

} // namespace

std::filesystem::path defaultStatePath() {
    return platformConfigDir() / "ithaca-legacy" / "state.json";
}

std::optional<GuiState> loadState(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::stringstream ss; ss << f.rdbuf();
    const std::string json = ss.str();

    GuiState s;
    try {
        std::string sv = findValue(json, "schema_version");
        if (sv.empty()) return std::nullopt;
        s.schema_version = std::stoi(sv);
        if (s.schema_version != 3 && s.schema_version != 4) return std::nullopt;
        s.bank_search_dir       = findValue(json, "bank_search_dir");
        s.bank_path             = findValue(json, "bank_path");
        s.midi_port_name        = findValue(json, "midi_port_name");
        s.log_level             = findValue(json, "log_level");
        if (s.log_level.empty()) s.log_level = "info";
        { std::string mc = findValue(json, "midi_channel");
          s.midi_channel = mc.empty() ? -1 : std::stoi(mc); }
        s.master_gain_db        = std::stof(findValue(json, "master_gain_db"));
        s.release_ms            = std::stof(findValue(json, "release_ms"));
        s.excite_decay_ms       = std::stof(findValue(json, "excite_decay_ms"));
        s.max_resonance_voices  = std::stoi(findValue(json, "max_resonance_voices"));
        s.window_x = std::stoi(findValue(json, "window_x"));
        s.window_y = std::stoi(findValue(json, "window_y"));
        s.window_w = std::stoi(findValue(json, "window_w"));
        s.window_h = std::stoi(findValue(json, "window_h"));
        // DSP pole (schema v4). Cteno obranne — chybejici klic (napr. v3 soubor)
        // -> default ze struktury, takze stara konfigurace nacte cisty (DSP off).
        auto readF = [&](const char* k, float dv){ std::string v = findValue(json, k); return v.empty() ? dv : std::stof(v); };
        auto readB = [&](const char* k, bool dv){ std::string v = findValue(json, k); return v.empty() ? dv : (v == "true" || v == "1"); };
        auto readI = [&](const char* k, int dv){ std::string v = findValue(json, k); return v.empty() ? dv : std::stoi(v); };
        s.agc_enabled          = readB("agc_enabled", s.agc_enabled);
        s.agc_target           = readF("agc_target", s.agc_target);
        s.agc_release_ms       = readF("agc_release_ms", s.agc_release_ms);
        s.agc_floor            = readF("agc_floor", s.agc_floor);
        // Enhancer (ex-BBE): cti enhancer_*, fallback na stare bbe_* (migrace).
        s.enhancer_enabled = readB("enhancer_enabled", readB("bbe_enabled", s.enhancer_enabled));
        s.enhancer_process = readF("enhancer_process", readF("bbe_definition", s.enhancer_process));
        s.enhancer_contour = readF("enhancer_contour", readF("bbe_bass", s.enhancer_contour));
        s.enhancer_mid     = readF("enhancer_mid", s.enhancer_mid);
        s.limiter_enabled      = readB("limiter_enabled", s.limiter_enabled);
        s.limiter_threshold_db = readF("limiter_threshold_db", s.limiter_threshold_db);
        s.limiter_release_ms   = readF("limiter_release_ms", s.limiter_release_ms);
        s.config_page          = readI("config_page", s.config_page);
        s.convolver_enabled = readB("convolver_enabled", s.convolver_enabled);
        s.convolver_mix     = readF("convolver_mix", s.convolver_mix);
        s.convolver_choice  = readI("convolver_choice", s.convolver_choice);
        s.convolver_decay = readF("convolver_decay", s.convolver_decay);
        s.convolver_tone  = readF("convolver_tone",  s.convolver_tone);
        s.convolver_size  = readF("convolver_size",  s.convolver_size);
        s.resonance_enabled  = readB("resonance_enabled", s.resonance_enabled);
        s.resonance_gain_db  = readF("resonance_gain_db", s.resonance_gain_db);
        s.resonance_layer_db = readF("resonance_layer_db", s.resonance_layer_db);
        s.resonance_window_ms = readI("resonance_window_ms", s.resonance_window_ms);
        s.audio_block_size  = readI("audio_block_size", s.audio_block_size);
        s.audio_sample_rate = readI("audio_sample_rate", s.audio_sample_rate);
        s.schema_version = 4;   // po nacteni vzdy ulozime jako v4
    } catch (...) {
        return std::nullopt;
    }
    return s;
}

bool saveState(const std::filesystem::path& path, const GuiState& s) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    auto tmp = path; tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << "{\n";
        f << "  \"schema_version\": "    << s.schema_version             << ",\n";
        f << "  \"bank_search_dir\": \"" << jsonEscape(s.bank_search_dir) << "\",\n";
        f << "  \"bank_path\": \""        << jsonEscape(s.bank_path)      << "\",\n";
        f << "  \"midi_port_name\": \""   << jsonEscape(s.midi_port_name) << "\",\n";
        f << "  \"log_level\": \""       << jsonEscape(s.log_level)      << "\",\n";
        f << "  \"midi_channel\": " << s.midi_channel << ",\n";
        f << "  \"master_gain_db\": "     << s.master_gain_db             << ",\n";
        f << "  \"resonance_enabled\": "  << (s.resonance_enabled ? "true" : "false") << ",\n";
        f << "  \"resonance_gain_db\": "  << s.resonance_gain_db  << ",\n";
        f << "  \"resonance_layer_db\": " << s.resonance_layer_db << ",\n";
        f << "  \"release_ms\": "         << s.release_ms                 << ",\n";
        f << "  \"excite_decay_ms\": "    << s.excite_decay_ms            << ",\n";
        f << "  \"max_resonance_voices\": " << s.max_resonance_voices     << ",\n";
        f << "  \"resonance_window_ms\": " << s.resonance_window_ms       << ",\n";
        f << "  \"window_x\": " << s.window_x << ",\n";
        f << "  \"window_y\": " << s.window_y << ",\n";
        f << "  \"window_w\": " << s.window_w << ",\n";
        f << "  \"window_h\": " << s.window_h << ",\n";
        f << "  \"agc_enabled\": "        << (s.agc_enabled ? "true" : "false") << ",\n";
        f << "  \"agc_target\": "         << s.agc_target          << ",\n";
        f << "  \"agc_release_ms\": "     << s.agc_release_ms       << ",\n";
        f << "  \"agc_floor\": "          << s.agc_floor            << ",\n";
        f << "  \"enhancer_enabled\": "   << (s.enhancer_enabled ? "true" : "false") << ",\n";
        f << "  \"enhancer_process\": "   << s.enhancer_process     << ",\n";
        f << "  \"enhancer_contour\": "   << s.enhancer_contour     << ",\n";
        f << "  \"enhancer_mid\": "       << s.enhancer_mid         << ",\n";
        f << "  \"limiter_enabled\": "    << (s.limiter_enabled ? "true" : "false") << ",\n";
        f << "  \"limiter_threshold_db\": " << s.limiter_threshold_db << ",\n";
        f << "  \"limiter_release_ms\": " << s.limiter_release_ms   << ",\n";
        f << "  \"config_page\": "        << s.config_page          << ",\n";
        f << "  \"convolver_enabled\": " << (s.convolver_enabled ? "true":"false") << ",\n";
        f << "  \"convolver_mix\": "     << s.convolver_mix     << ",\n";
        f << "  \"convolver_choice\": "  << s.convolver_choice  << ",\n";
        f << "  \"convolver_decay\": " << s.convolver_decay << ",\n";
        f << "  \"convolver_tone\": "  << s.convolver_tone  << ",\n";
        f << "  \"convolver_size\": "  << s.convolver_size  << ",\n";
        f << "  \"audio_block_size\": "   << s.audio_block_size     << ",\n";
        f << "  \"audio_sample_rate\": "  << s.audio_sample_rate    << "\n";
        f << "}\n";
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

} // namespace ithaca::gui
