// tests/test_persistence.cpp - GuiState round-trip + edge cases.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../app/gui/persistence.h"
#include <filesystem>
#include <fstream>

TEST_CASE("Persistence round-trip") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_test_state.json";

    GuiState s;
    s.bank_path           = "/foo/bar/bank";
    s.midi_port_name      = "IAC Driver";
    s.master_gain_db      = -6.0f;
    s.resonance_enabled  = false;
    s.resonance_gain_db  = -9.5f;
    s.resonance_layer_db = -22.f;
    s.release_ms          = 250.f;
    s.excite_decay_ms     = 4000.f;
    s.max_resonance_voices = 16;
    s.window = {200, 300, 800, 600};
    s.log_level = "debug";
    s.midi_channel = 4;   // 0-based; -1 = OMNI

    REQUIRE(saveState(p, s));
    auto loaded = loadState(p);
    REQUIRE(loaded.has_value());
    CHECK(loaded->bank_path == s.bank_path);
    CHECK(loaded->midi_port_name == s.midi_port_name);
    CHECK(loaded->master_gain_db == doctest::Approx(s.master_gain_db));
    CHECK(loaded->resonance_enabled  == s.resonance_enabled);
    CHECK(loaded->resonance_gain_db  == doctest::Approx(s.resonance_gain_db));
    CHECK(loaded->resonance_layer_db == doctest::Approx(s.resonance_layer_db));
    CHECK(loaded->release_ms == doctest::Approx(s.release_ms));
    CHECK(loaded->excite_decay_ms == doctest::Approx(s.excite_decay_ms));
    CHECK(loaded->max_resonance_voices == 16);
    CHECK(loaded->window.w == 800);
    CHECK(loaded->window.h == 600);
    CHECK(loaded->log_level == "debug");
    CHECK(loaded->midi_channel == 4);

    std::filesystem::remove(p);
}

TEST_CASE("Persistence round-trip — engine-tuning pole (vsechna)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_test_state_full.json";

    GuiState s;
    // Vsem polim dame NE-default hodnoty → kdyby nekterou save/load vynechal,
    // nactena hodnota = struct default ≠ nastavena → test selze.
    // (DSP chain uz sem nepatri — jede genericky, viz testy nize.)
    s.bank_search_dir      = "/banks/scan/dir";
    s.resonance_window_ms  = 8000;     // default 12000
    s.preload_ms           = 300;      // default 150
    s.cache_budget_mb      = 2048;     // default 0
    s.config_page = 4;
    s.audio_block_size = 128; s.audio_sample_rate = 44100;

    REQUIRE(saveState(p, s));
    auto l = loadState(p);
    REQUIRE(l.has_value());

    CHECK(l->bank_search_dir == s.bank_search_dir);
    CHECK(l->resonance_window_ms == 8000);
    CHECK(l->preload_ms == 300);
    CHECK(l->cache_budget_mb == 2048);
    CHECK(l->config_page == 4);
    CHECK(l->audio_block_size == 128);
    CHECK(l->audio_sample_rate == 44100);

    std::filesystem::remove(p);
}

TEST_CASE("Persistence — chybejici nova pole spadnou na default (obranne cteni)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_partial_v4.json";
    {
        // v4 soubor BEZ novych klicu (preload_ms / cache_budget_mb / resonance_window_ms)
        std::ofstream f(p);
        f << "{\n  \"schema_version\": 4,\n  \"bank_path\": \"/x\",\n"
             "  \"master_gain_db\": 0,\n  \"release_ms\": 200,\n"
             "  \"excite_decay_ms\": 5000,\n  \"max_resonance_voices\": 32,\n"
             "  \"window_x\": 100,\n  \"window_y\": 100,\n"
             "  \"window_w\": 1280,\n  \"window_h\": 720\n}\n";
    }
    auto l = loadState(p);
    REQUIRE(l.has_value());
    CHECK(l->resonance_window_ms == 12000);  // default
    CHECK(l->preload_ms == 150);             // default
    CHECK(l->cache_budget_mb == 0);          // default (auto)
    CHECK(l->dsp.empty());                   // zadne DSP klice → stage si drzi defaulty
    std::filesystem::remove(p);
}

// -- Genericka DSP persistence (schema v5) ----------------------------------
// Stage stav se uklada pod klicem "dsp.<STAGE>.<Param::id>" — pridani parametru
// do stage uz nevyzaduje zadnou zmenu v GuiState ani v persistence.

TEST_CASE("DSP stage stav se persistuje genericky podle Param::id") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_dsp_generic.json";

    GuiState s;
    s.dsp["CONVOLVER"] = {true, 1, {{"mix", 0.4f}, {"decay", 0.3f},
                                    {"tone", 0.7f}, {"size", 0.55f}}};
    s.dsp["AGC"]       = {true, -1, {{"target_rms", 0.2f}, {"release_ms", 150.f},
                                     {"gain_floor", 0.1f}}};
    s.dsp["LIMITER"]   = {false, -1, {{"threshold_db", -3.f}, {"release_ms", 100.f}}};

    REQUIRE(saveState(p, s));
    auto l = loadState(p);
    REQUIRE(l.has_value());

    REQUIRE(l->dsp.count("CONVOLVER") == 1);
    CHECK(l->dsp.at("CONVOLVER").enabled == true);
    CHECK(l->dsp.at("CONVOLVER").choice == 1);
    CHECK(l->dsp.at("CONVOLVER").params.at("mix")   == doctest::Approx(0.4f));
    CHECK(l->dsp.at("CONVOLVER").params.at("decay") == doctest::Approx(0.3f));
    CHECK(l->dsp.at("CONVOLVER").params.at("tone")  == doctest::Approx(0.7f));
    CHECK(l->dsp.at("CONVOLVER").params.at("size")  == doctest::Approx(0.55f));

    REQUIRE(l->dsp.count("AGC") == 1);
    CHECK(l->dsp.at("AGC").enabled == true);
    CHECK(l->dsp.at("AGC").params.at("target_rms") == doctest::Approx(0.2f));
    CHECK(l->dsp.at("AGC").params.at("gain_floor") == doctest::Approx(0.1f));

    REQUIRE(l->dsp.count("LIMITER") == 1);
    CHECK(l->dsp.at("LIMITER").enabled == false);
    CHECK(l->dsp.at("LIMITER").params.at("threshold_db") == doctest::Approx(-3.f));

    std::filesystem::remove(p);
}

TEST_CASE("Genericka persistence unese NEZNAMY parametr (pridani do stage)") {
    // Smysl cele zmeny: novy parametr ve stage projde persistenci bez zasahu
    // do GuiState / loadState / saveState.
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_dsp_newparam.json";

    GuiState s;
    s.dsp["ENHANCER"] = {true, -1, {{"process", 6.f}, {"zcela_novy_param", 42.5f}}};
    REQUIRE(saveState(p, s));
    auto l = loadState(p);
    REQUIRE(l.has_value());
    CHECK(l->dsp.at("ENHANCER").params.at("zcela_novy_param") == doctest::Approx(42.5f));

    std::filesystem::remove(p);
}

TEST_CASE("Migrace v4 -> v5: ploche DSP klice se prevedou na genericke") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_v4_dsp.json";
    {
        std::ofstream f(p);
        f << "{\n  \"schema_version\": 4,\n  \"bank_path\": \"/x\",\n"
             "  \"convolver_enabled\": true,\n  \"convolver_mix\": 0.4,\n"
             "  \"convolver_choice\": 2,\n  \"convolver_decay\": 0.3,\n"
             "  \"convolver_tone\": 0.7,\n  \"convolver_size\": 0.55,\n"
             "  \"agc_enabled\": true,\n  \"agc_target\": 0.2,\n"
             "  \"agc_release_ms\": 150,\n  \"agc_floor\": 0.1,\n"
             "  \"enhancer_enabled\": true,\n  \"enhancer_process\": 6,\n"
             "  \"enhancer_contour\": 3,\n  \"enhancer_mid\": -2,\n"
             "  \"limiter_enabled\": true,\n  \"limiter_threshold_db\": -3,\n"
             "  \"limiter_release_ms\": 100\n}\n";
    }
    auto l = loadState(p);
    REQUIRE(l.has_value());
    CHECK(l->schema_version == 6);          // po nacteni se uklada jako v6
    CHECK(l->dsp.at("CONVOLVER").enabled == true);
    CHECK(l->dsp.at("CONVOLVER").choice == 2);
    CHECK(l->dsp.at("CONVOLVER").params.at("mix") == doctest::Approx(0.4f));
    CHECK(l->dsp.at("AGC").params.at("target_rms") == doctest::Approx(0.2f));
    CHECK(l->dsp.at("AGC").params.at("gain_floor") == doctest::Approx(0.1f));
    CHECK(l->dsp.at("AGC").params.at("release_ms") == doctest::Approx(150.f));
    CHECK(l->dsp.at("ENHANCER").params.at("process") == doctest::Approx(6.f));
    CHECK(l->dsp.at("ENHANCER").params.at("contour") == doctest::Approx(3.f));
    CHECK(l->dsp.at("ENHANCER").params.at("mid") == doctest::Approx(-2.f));
    CHECK(l->dsp.at("LIMITER").params.at("threshold_db") == doctest::Approx(-3.f));
    CHECK(l->dsp.at("LIMITER").params.at("release_ms") == doctest::Approx(100.f));
    std::filesystem::remove(p);
}

TEST_CASE("Migrace v3 bbe_* -> ENHANCER prezila prechod na genericke klice") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_v3_bbe.json";
    {
        std::ofstream f(p);
        f << "{\n  \"schema_version\": 3,\n  \"bbe_enabled\": true,\n"
             "  \"bbe_definition\": 5,\n  \"bbe_bass\": 4\n}\n";
    }
    auto l = loadState(p);
    REQUIRE(l.has_value());
    CHECK(l->dsp.at("ENHANCER").enabled == true);
    CHECK(l->dsp.at("ENHANCER").params.at("process") == doctest::Approx(5.f));
    CHECK(l->dsp.at("ENHANCER").params.at("contour") == doctest::Approx(4.f));
    std::filesystem::remove(p);
}

// -- Porovnani stavu (podklad pro persistence debounce v main.cpp) -----------

TEST_CASE("GuiState porovnani zachyti zmenu libovolneho pole") {
    using namespace ithaca::gui;
    GuiState a, b;
    CHECK(a == b);

    b = a; b.dsp["AGC"].params["target_rms"] = 0.3f;
    CHECK_FALSE(a == b);

    b = a; b.dsp["AGC"].enabled = true;
    CHECK_FALSE(a == b);

    // bank_search_dir driv v rucnim debounce retezci CHYBEL (nalez revize).
    b = a; b.bank_search_dir = "/nove/banky";
    CHECK_FALSE(a == b);

    b = a; b.master_gain_db = -3.f;
    CHECK_FALSE(a == b);

    b = a; b.window.w = 640;
    CHECK_FALSE(a == b);   // geometrie je soucasti rovnosti; debounce ji resi zvlast
}

TEST_CASE("Persistence missing file") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_nonexistent_xyz.json";
    std::filesystem::remove(p);
    CHECK_FALSE(loadState(p).has_value());
}

TEST_CASE("Persistence wrong schema_version") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_bad_schema.json";
    {
        std::ofstream f(p);
        f << "{\"schema_version\":99,\"bank_path\":\"\"}\n";
    }
    CHECK_FALSE(loadState(p).has_value());
    std::filesystem::remove(p);
}

TEST_CASE("Persistence schema v1 odmitnuta (zadna zpetna kompatibilita)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_v1_schema.json";
    {
        std::ofstream f(p);
        f << "{\"schema_version\":1,\"bank_path\":\"/old/bank\"}\n";
    }
    CHECK_FALSE(loadState(p).has_value());   // v1 se zahodi → GUI nastartuje s defaulty
    std::filesystem::remove(p);
}

TEST_CASE("Persistence schema v2 odmitnuta (po bumpu na 3)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_v2_schema.json";
    { std::ofstream f(p); f << "{\"schema_version\":2,\"bank_path\":\"/x\"}\n"; }
    CHECK_FALSE(loadState(p).has_value());
    std::filesystem::remove(p);
}

TEST_CASE("defaultStatePath neni prazdne") {
    using namespace ithaca::gui;
    CHECK_FALSE(defaultStatePath().empty());
}

TEST_CASE("Persistence escape v cestach") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_escape.json";
    GuiState s;
    s.bank_path = R"(C:\Path with "quote" and \backslash)";
    REQUIRE(saveState(p, s));
    auto loaded = loadState(p);
    REQUIRE(loaded.has_value());
    CHECK(loaded->bank_path == s.bank_path);
    std::filesystem::remove(p);
}

TEST_CASE("poskozena numericka hodnota nezahodi cely stav (bank_path prezije)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_test_state_corrupt.json";
    {
        std::ofstream f(p);
        f << "{\n  \"schema_version\": 4,\n  \"bank_path\": \"/moje/banka\",\n"
             "  \"master_gain_db\": abc,\n  \"window_w\": 1280\n}\n";
    }
    auto st = loadState(p);
    REQUIRE(st.has_value());
    CHECK(st->bank_path == "/moje/banka");
    CHECK(st->master_gain_db == doctest::Approx(GuiState{}.master_gain_db));
    std::filesystem::remove(p);
}

TEST_CASE("window geometrie se sanitizuje (0x0 z minimalizovaneho okna nezabije start)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_test_state_geom.json";
    {
        std::ofstream f(p);
        f << "{\n  \"schema_version\": 4,\n  \"window_w\": 0,\n  \"window_h\": -5,\n"
             "  \"midi_channel\": 99\n}\n";
    }
    auto st = loadState(p);
    REQUIRE(st.has_value());
    CHECK(st->window.w >= 320);
    CHECK(st->window.h >= 240);
    CHECK(st->midi_channel == -1);   // mimo rozsah → OMNI
    std::filesystem::remove(p);
}

// -- JSON escape / unescape -------------------------------------------------
// Parser drive dekodoval jen \n, \\ a obecne \x -> x. Tabulator, CR i \uXXXX
// se tedy dekodovaly nespravne (\t -> 't'). Zapisovac zase control znaky
// < 0x20 psal syrove, coz je nevalidni JSON.

TEST_CASE("Round-trip retezce s control znaky (tab, CR, LF)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_ctrl.json";
    GuiState s;
    s.bank_path      = "/cesta/s\ttabem/a\rCR";
    s.midi_port_name = "port\ns novym radkem";
    s.bank_search_dir = "a\bb\fc";

    REQUIRE(saveState(p, s));
    auto l = loadState(p);
    REQUIRE(l.has_value());
    CHECK(l->bank_path == s.bank_path);
    CHECK(l->midi_port_name == s.midi_port_name);
    CHECK(l->bank_search_dir == s.bank_search_dir);
    std::filesystem::remove(p);
}

TEST_CASE("Zapsany JSON neobsahuje syrove control znaky") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_ctrl_raw.json";
    GuiState s;
    s.bank_path = "x\ty";
    REQUIRE(saveState(p, s));

    std::ifstream f(p);
    std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // Uvnitr hodnot nesmi byt syrovy tab; struktura pouziva jen mezery a \n.
    CHECK(txt.find('\t') == std::string::npos);
    std::filesystem::remove(p);
}

TEST_CASE("Dekodovani \\uXXXX escape sekvenci") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_uesc.json";
    {
        std::ofstream f(p);
        // "Bösendorfer" pres \u00f6 a ceske "č" pres \u010d
        f << "{\n  \"schema_version\": 5,\n"
             "  \"bank_path\": \"B\\u00f6sendorfer \\u010desky\",\n"
             "  \"midi_port_name\": \"tab:\\there\"\n}\n";
    }
    auto l = loadState(p);
    REQUIRE(l.has_value());
    CHECK(l->bank_path == "B\xC3\xB6sendorfer \xC4\x8D" "esky");   // UTF-8
    CHECK(l->midi_port_name == "tab:\there");
    std::filesystem::remove(p);
}

TEST_CASE("Dekodovani surrogate paru (\\uD83C\\uDFB9 = klavir)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_surrogate.json";
    {
        std::ofstream f(p);
        f << "{\n  \"schema_version\": 5,\n"
             "  \"bank_path\": \"\\uD83C\\uDFB9 banka\"\n}\n";
    }
    auto l = loadState(p);
    REQUIRE(l.has_value());
    CHECK(l->bank_path == "\xF0\x9F\x8E\xB9 banka");   // U+1F3B9 v UTF-8
    std::filesystem::remove(p);
}

TEST_CASE("UTF-8 v ceste projde beze zmeny (bez escapovani)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_utf8.json";
    GuiState s;
    s.bank_path = "/Users/j/Zvuky/piáno-ěščřžýá";
    REQUIRE(saveState(p, s));
    auto l = loadState(p);
    REQUIRE(l.has_value());
    CHECK(l->bank_path == s.bank_path);
    std::filesystem::remove(p);
}

// -- Uzivatelske defaulty (sekce "defaults", v6) -----------------------------

TEST_CASE("Defaults: round-trip cele sekce vcetne volice a enabled") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_defaults_rt.json";

    GuiState s;
    // Snapshot pokryva i stranky, ktere zadna DSP stage nejsou.
    s.defaults["MASTER"]    = {true, -1, {{"master_db", -3.5f}, {"release_ms", 320.f}}};
    s.defaults["RESONANCE"] = {false, -1, {{"reso_gain_db", -8.f}}};
    s.defaults["CONVOLVER"] = {true, 1, {{"mix", 0.42f}}};
    // Zivy stav se lisi od defaultu — obe sekce musi prezit nezavisle.
    s.dsp["CONVOLVER"]      = {false, 0, {{"mix", 0.9f}}};

    REQUIRE(saveState(p, s));
    auto l = loadState(p);
    REQUIRE(l.has_value());

    CHECK(l->defaults.at("MASTER").params.at("master_db") == doctest::Approx(-3.5f));
    CHECK(l->defaults.at("MASTER").params.at("release_ms") == doctest::Approx(320.f));
    CHECK(l->defaults.at("RESONANCE").enabled == false);
    CHECK(l->defaults.at("CONVOLVER").choice == 1);
    CHECK(l->defaults.at("CONVOLVER").params.at("mix") == doctest::Approx(0.42f));
    // Klicove: "dsp." a "defaults." se nesmi michat.
    CHECK(l->dsp.at("CONVOLVER").choice == 0);
    CHECK(l->dsp.at("CONVOLVER").enabled == false);
    CHECK(l->dsp.at("CONVOLVER").params.at("mix") == doctest::Approx(0.9f));
    CHECK(l->defaults.size() == 3);

    std::filesystem::remove(p);
}

TEST_CASE("Defaults: v5 soubor je bez nich a nacte se prazdny (ne chyba)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_v5_nodefaults.json";
    {
        std::ofstream f(p);
        f << "{\n  \"schema_version\": 5,\n  \"bank_path\": \"/x\",\n"
             "  \"dsp.AGC.enabled\": true,\n  \"dsp.AGC.target_rms\": 0.2\n}\n";
    }
    auto l = loadState(p);
    REQUIRE(l.has_value());
    CHECK(l->defaults.empty());             // zadne uzivatelske → RESET jede tovarne
    CHECK(l->dsp.at("AGC").params.at("target_rms") == doctest::Approx(0.2f));
    CHECK(l->schema_version == 6);
    std::filesystem::remove(p);
}

TEST_CASE("Defaults: prazdna mapa nenechá v JSONu visici carku") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_defaults_empty.json";
    GuiState s;                              // zadne dsp, zadne defaults
    REQUIRE(saveState(p, s));
    auto l = loadState(p);
    REQUIRE(l.has_value());                  // parsovatelne = carka nevisi
    CHECK(l->defaults.empty());
    std::filesystem::remove(p);
}

TEST_CASE("Defaults se ucastni porovnani pro persistence debounce") {
    using namespace ithaca::gui;
    GuiState a, b;
    CHECK(a == b);
    b.defaults["MASTER"] = {true, -1, {{"master_db", -1.f}}};
    CHECK_FALSE(a == b);                     // jinak by se snapshot neulozil
}
