// app/gui/persistence.cpp - viz persistence.h.
//
// Mini JSON: flat key:value bez nesting. Pro slozitejsi schema (vnorene
// objekty, pole) by se hodilo vendorovat nlohmann/json, ale na nasich ~40
// klicich staci primitivni parser.
#include "persistence.h"
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

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

// Escape JSON stringu. Krome uvozovek a zpetneho lomitka musi odejit VSECHNY
// control znaky < 0x20 — syrovy tabulator v hodnote je nevalidni JSON.
// UTF-8 (diakritika v cestach) prochazi beze zmeny; escapovat ho neni treba.
std::string jsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    o += buf;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

// Prida code point jako UTF-8.
void appendUtf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Precte 4 hexa cifry na pozici i (nepohybuje s i). false = nevalidni.
bool readHex4(const std::string& j, size_t i, unsigned int& out) {
    if (i + 4 > j.size()) return false;
    unsigned int v = 0;
    for (size_t k = 0; k < 4; ++k) {
        const char c = j[i + k];
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

// Rozbali escape sekvenci zacinajici na j[i] == '\\'. Posune i za ni.
// Drive se dekodovalo jen \n, \\ a obecne \x -> x, takze \t koncil jako 't'
// a \uXXXX jako 'uXXXX'.
void decodeEscape(const std::string& j, size_t& i, std::string& val) {
    const char nxt = j[i + 1];
    i += 2;
    switch (nxt) {
        case 'n':  val += '\n'; break;
        case 'r':  val += '\r'; break;
        case 't':  val += '\t'; break;
        case 'b':  val += '\b'; break;
        case 'f':  val += '\f'; break;
        case '"':  val += '"';  break;
        case '\\': val += '\\'; break;
        case '/':  val += '/';  break;
        case 'u': {
            unsigned int cp = 0;
            if (!readHex4(j, i, cp)) { val += 'u'; break; }   // nevalidni → literal
            i += 4;
            // Surrogate par: high D800–DBFF + low DC00–DFFF → jeden code point.
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < j.size()
                && j[i] == '\\' && j[i + 1] == 'u') {
                unsigned int lo = 0;
                if (readHex4(j, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    i += 6;
                }
            }
            appendUtf8(val, cp);
            break;
        }
        default: val += nxt; break;   // neznamy escape → literalni znak
    }
}

// Rozparsuje cely flat JSON na mapu klic -> raw hodnota (u stringu s vyresenymi
// escapy, u cisel jako text). Jeden pruchod misto ~40 opakovanych find() volani
// a hlavne: umi VYJMENOVAT klice, coz je potreba pro genericke "dsp.*" (jejich
// jmena persistence dopredu nezna — pochazi z Param::id jednotlivych stage).
std::map<std::string, std::string> parseFlatJson(const std::string& j) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < j.size()) {
        // Hledej zacatek klice; '}' ukoncuje objekt (nesting nepodporujeme).
        while (i < j.size() && j[i] != '"') {
            if (j[i] == '}') return out;
            ++i;
        }
        if (i >= j.size()) break;
        ++i;
        std::string key;
        while (i < j.size() && j[i] != '"') {
            if (j[i] == '\\' && i + 1 < j.size()) { key += j[i + 1]; i += 2; }
            else                                    key += j[i++];
        }
        if (i < j.size()) ++i;                       // za uzaviraci "
        while (i < j.size() && std::isspace((unsigned char)j[i])) ++i;
        if (i >= j.size() || j[i] != ':') continue;  // nebyl to klic, hledej dal
        ++i;
        while (i < j.size() && std::isspace((unsigned char)j[i])) ++i;

        std::string val;
        if (i < j.size() && j[i] == '"') {           // string hodnota
            ++i;
            while (i < j.size() && j[i] != '"') {
                if (j[i] == '\\' && i + 1 < j.size()) decodeEscape(j, i, val);
                else                                  val += j[i++];
            }
            if (i < j.size()) ++i;
        } else {                                     // cislo / bool
            while (i < j.size() && j[i] != ',' && j[i] != '}' && j[i] != '\n')
                val += j[i++];
            while (!val.empty() && std::isspace((unsigned char)val.back()))
                val.pop_back();
        }
        out[key] = val;
    }
    return out;
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
        const auto kv = parseFlatJson(json);
        auto raw = [&](const std::string& k) -> std::string {
            auto it = kv.find(k);
            return (it == kv.end()) ? std::string{} : it->second;
        };
        std::string sv = raw("schema_version");
        if (sv.empty()) return std::nullopt;
        s.schema_version = std::stoi(sv);
        if (s.schema_version < 3 || s.schema_version > 7) return std::nullopt;
        s.bank_search_dir       = raw("bank_search_dir");
        s.bank_path             = raw("bank_path");
        s.midi_port_name        = raw("midi_port_name");
        s.log_level             = raw("log_level");
        if (s.log_level.empty()) s.log_level = "info";
        // Defenzivni ctecky: chybejici NEBO poskozeny klic → default ze
        // struktury. Drive stof("")/stof("abc") → vyjimka → cely stav zahozen
        // vc. bank_path a MIDI (jeden vadny klic smazal nesouvisejici pole).
        auto readF = [&](const char* k, float dv){
            std::string v = raw(k);
            if (v.empty()) return dv;
            try { return std::stof(v); } catch (...) { return dv; }
        };
        auto readB = [&](const char* k, bool dv){
            std::string v = raw(k);
            return v.empty() ? dv : (v == "true" || v == "1");
        };
        auto readI = [&](const char* k, int dv){
            std::string v = raw(k);
            if (v.empty()) return dv;
            try { return std::stoi(v); } catch (...) { return dv; }
        };
        // v7: maska kanalu. Starsi soubory maji jediny index `midi_channel`
        // (-1 = OMNI) — odvodime z nej masku, at se nastaveni neztrati.
        {
            const int m = readI("midi_channel_mask", -1);
            if (m >= 0) {
                s.midi_channel_mask = (uint16_t)(m & 0xFFFF);
            } else {
                int legacy = readI("midi_channel", -1);
                if (legacy < -1 || legacy > 15) legacy = -1;
                s.midi_channel_mask = (legacy < 0) ? 0xFFFFu
                                                   : (uint16_t)(1u << legacy);
            }
        }
        s.master_gain_db        = readF("master_gain_db", s.master_gain_db);
        s.release_ms            = readF("release_ms", s.release_ms);
        s.excite_decay_ms       = readF("excite_decay_ms", s.excite_decay_ms);
        s.max_resonance_voices  = readI("max_resonance_voices", s.max_resonance_voices);
        s.window.x = readI("window_x", s.window.x);
        s.window.y = readI("window_y", s.window.y);
        s.window.w = readI("window_w", s.window.w);
        s.window.h = readI("window_h", s.window.h);
        // Sanitizace geometrie: minimalizovane okno (Windows) uklada 0x0 a
        // glfwCreateWindow(0,0) pri pristim startu selze → app nejde spustit.
        if (s.window.w < 320) s.window.w = 1280;
        if (s.window.h < 240) s.window.h = 720;
        s.config_page          = readI("config_page", s.config_page);
        s.resonance_enabled  = readB("resonance_enabled", s.resonance_enabled);
        s.resonance_gain_db  = readF("resonance_gain_db", s.resonance_gain_db);
        s.resonance_layer_db = readF("resonance_layer_db", s.resonance_layer_db);
        s.resonance_window_ms = readI("resonance_window_ms", s.resonance_window_ms);
        s.preload_ms          = readI("preload_ms", s.preload_ms);
        s.cache_budget_mb     = readI("cache_budget_mb", s.cache_budget_mb);
        s.audio_block_size  = readI("audio_block_size", s.audio_block_size);
        s.audio_sample_rate = readI("audio_sample_rate", s.audio_sample_rate);
        // Zar vlny. Strop odpovida dosahu ~1300 px, tedy vic nez cely panel —
        // nad tim uz to neni nastaveni, ale preklep. Rucne editovany soubor
        // muze obsahovat i NaN, proto sanitizeGlow a ne std::clamp.
        s.wave_glow = sanitizeGlow(readF("wave_glow", s.wave_glow), kWaveGlowMax);
        s.wave_glow_budget_ms =
            sanitizeGlow(readF("wave_glow_budget_ms", s.wave_glow_budget_ms),
                         kWaveBudgetMax);

        // -- Genericke sekce "<prefix><STAGE>.<Param::id>" ---------------------
        // Persistence jmena parametru nezna — proste vezme vse pod prefixem
        // a naleje do mapy. Novy parametr ve stage tedy projde bez jakekoli
        // zmeny tady. Stejny tvar pouziva "dsp." (v5+) i "defaults." (v6+).
        auto readSection = [&](const std::string& prefix,
                               std::map<std::string, DspStageState>& into) {
            const size_t pl = prefix.size();
            for (const auto& [k, v] : kv) {
                if (k.rfind(prefix, 0) != 0) continue;
                const size_t dot = k.find('.', pl);
                if (dot == std::string::npos) continue;
                const std::string stage = k.substr(pl, dot - pl);
                const std::string field = k.substr(dot + 1);
                if (stage.empty() || field.empty()) continue;
                auto& st = into[stage];
                if      (field == "enabled") st.enabled = (v == "true" || v == "1");
                else if (field == "choice")  { try { st.choice = std::stoi(v); } catch (...) {} }
                else                         { try { st.params[field] = std::stof(v); } catch (...) {} }
            }
        };
        readSection("dsp.", s.dsp);
        readSection("defaults.", s.defaults);

        // -- Migrace v3/v4 -> v5: ploche DSP klice na genericke ----------------
        // Klice odpovidaji Param::id v jednotlivych stage (agc.cpp, enhancer.cpp,
        // limiter.cpp, convolver.cpp). `legacy` pokryva jeste starsi bbe_* nazvy
        // z v3 (Enhancer se drive jmenoval BBE).
        if (s.schema_version < 5) {
            struct MigF { const char* stage; const char* id; const char* key; const char* legacy; };
            static const MigF kMigF[] = {
                {"CONVOLVER", "mix",          "convolver_mix",        nullptr},
                {"CONVOLVER", "decay",        "convolver_decay",      nullptr},
                {"CONVOLVER", "tone",         "convolver_tone",       nullptr},
                {"CONVOLVER", "size",         "convolver_size",       nullptr},
                {"AGC",       "target_rms",   "agc_target",           nullptr},
                {"AGC",       "release_ms",   "agc_release_ms",       nullptr},
                {"AGC",       "gain_floor",   "agc_floor",            nullptr},
                {"ENHANCER",  "process",      "enhancer_process",     "bbe_definition"},
                {"ENHANCER",  "contour",      "enhancer_contour",     "bbe_bass"},
                {"ENHANCER",  "mid",          "enhancer_mid",         nullptr},
                {"LIMITER",   "threshold_db", "limiter_threshold_db", nullptr},
                {"LIMITER",   "release_ms",   "limiter_release_ms",   nullptr},
            };
            for (const auto& m : kMigF) {
                std::string v = raw(m.key);
                if (v.empty() && m.legacy) v = raw(m.legacy);
                if (v.empty()) continue;
                try { s.dsp[m.stage].params[m.id] = std::stof(v); } catch (...) {}
            }
            struct MigB { const char* stage; const char* key; const char* legacy; };
            static const MigB kMigB[] = {
                {"CONVOLVER", "convolver_enabled", nullptr},
                {"AGC",       "agc_enabled",       nullptr},
                {"ENHANCER",  "enhancer_enabled",  "bbe_enabled"},
                {"LIMITER",   "limiter_enabled",   nullptr},
            };
            for (const auto& m : kMigB) {
                std::string v = raw(m.key);
                if (v.empty() && m.legacy) v = raw(m.legacy);
                if (!v.empty()) s.dsp[m.stage].enabled = (v == "true" || v == "1");
            }
            if (std::string c = raw("convolver_choice"); !c.empty())
                { try { s.dsp["CONVOLVER"].choice = std::stoi(c); } catch (...) {} }
        }

        s.schema_version = 7;   // po nacteni vzdy ulozime jako v7
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
        f << "  \"midi_channel_mask\": " << (unsigned)s.midi_channel_mask << ",\n";
        f << "  \"master_gain_db\": "     << s.master_gain_db             << ",\n";
        f << "  \"resonance_enabled\": "  << (s.resonance_enabled ? "true" : "false") << ",\n";
        f << "  \"resonance_gain_db\": "  << s.resonance_gain_db  << ",\n";
        f << "  \"resonance_layer_db\": " << s.resonance_layer_db << ",\n";
        f << "  \"release_ms\": "         << s.release_ms                 << ",\n";
        f << "  \"excite_decay_ms\": "    << s.excite_decay_ms            << ",\n";
        f << "  \"max_resonance_voices\": " << s.max_resonance_voices     << ",\n";
        f << "  \"resonance_window_ms\": " << s.resonance_window_ms       << ",\n";
        f << "  \"preload_ms\": "          << s.preload_ms                << ",\n";
        f << "  \"cache_budget_mb\": "     << s.cache_budget_mb           << ",\n";
        f << "  \"window_x\": " << s.window.x << ",\n";
        f << "  \"window_y\": " << s.window.y << ",\n";
        f << "  \"window_w\": " << s.window.w << ",\n";
        f << "  \"window_h\": " << s.window.h << ",\n";
        f << "  \"config_page\": "        << s.config_page          << ",\n";
        f << "  \"audio_block_size\": "   << s.audio_block_size     << ",\n";
        f << "  \"wave_glow\": "          << s.wave_glow            << ",\n";
        f << "  \"wave_glow_budget_ms\": " << s.wave_glow_budget_ms << ",\n";
        f << "  \"audio_sample_rate\": "  << s.audio_sample_rate;
        // Genericke sekce: "<prefix><STAGE>.<Param::id>". Carka se pise PRED
        // kazdy radek (ne za), takze prazdna mapa nenecha visici carku.
        auto writeSection = [&](const char* prefix,
                                const std::map<std::string, DspStageState>& m) {
            for (const auto& [stage, st] : m) {
                f << ",\n  \"" << prefix << stage << ".enabled\": "
                  << (st.enabled ? "true" : "false");
                if (st.choice >= 0)
                    f << ",\n  \"" << prefix << stage << ".choice\": " << st.choice;
                for (const auto& [id, v] : st.params)
                    f << ",\n  \"" << prefix << stage << "." << id << "\": " << v;
            }
        };
        writeSection("dsp.", s.dsp);
        writeSection("defaults.", s.defaults);
        f << "\n}\n";
        f.flush();
        if (!f.good()) {
            // Plny disk / IO chyba: NIKDY neprepisuj dobry config torzem —
            // smysl tmp+rename vzoru je prave atomicita proti poskozeni.
            f.close();
            std::error_code rec;
            std::filesystem::remove(tmp, rec);
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

} // namespace ithaca::gui
