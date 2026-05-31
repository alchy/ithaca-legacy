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
        if (s.schema_version != 1) return std::nullopt;
        s.bank_path             = findValue(json, "bank_path");
        s.midi_port_name        = findValue(json, "midi_port_name");
        s.master_gain_db        = std::stof(findValue(json, "master_gain_db"));
        s.resonance_strength    = std::stof(findValue(json, "resonance_strength"));
        s.release_ms            = std::stof(findValue(json, "release_ms"));
        s.excite_decay_ms       = std::stof(findValue(json, "excite_decay_ms"));
        s.max_resonance_voices  = std::stoi(findValue(json, "max_resonance_voices"));
        s.window_x = std::stoi(findValue(json, "window_x"));
        s.window_y = std::stoi(findValue(json, "window_y"));
        s.window_w = std::stoi(findValue(json, "window_w"));
        s.window_h = std::stoi(findValue(json, "window_h"));
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
        f << "  \"bank_path\": \""        << jsonEscape(s.bank_path)      << "\",\n";
        f << "  \"midi_port_name\": \""   << jsonEscape(s.midi_port_name) << "\",\n";
        f << "  \"master_gain_db\": "     << s.master_gain_db             << ",\n";
        f << "  \"resonance_strength\": " << s.resonance_strength         << ",\n";
        f << "  \"release_ms\": "         << s.release_ms                 << ",\n";
        f << "  \"excite_decay_ms\": "    << s.excite_decay_ms            << ",\n";
        f << "  \"max_resonance_voices\": " << s.max_resonance_voices     << ",\n";
        f << "  \"window_x\": " << s.window_x << ",\n";
        f << "  \"window_y\": " << s.window_y << ",\n";
        f << "  \"window_w\": " << s.window_w << ",\n";
        f << "  \"window_h\": " << s.window_h << "\n";
        f << "}\n";
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

} // namespace ithaca::gui
