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
    s.window_x = 200; s.window_y = 300; s.window_w = 800; s.window_h = 600;
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
    CHECK(loaded->window_w == 800);
    CHECK(loaded->window_h == 600);
    CHECK(loaded->log_level == "debug");
    CHECK(loaded->midi_channel == 4);

    std::filesystem::remove(p);
}

TEST_CASE("Persistence round-trip — DSP + engine-tuning pole (vsechna)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_test_state_full.json";

    GuiState s;
    // Vsem polim dame NE-default hodnoty → kdyby nekterou save/load vynechal,
    // nactena hodnota = struct default ≠ nastavena → test selze.
    s.bank_search_dir      = "/banks/scan/dir";
    s.resonance_window_ms  = 8000;     // default 12000
    s.preload_ms           = 300;      // default 150
    s.cache_budget_mb      = 2048;     // default 0
    s.agc_enabled = true;  s.agc_target = 0.2f;  s.agc_release_ms = 150.f; s.agc_floor = 0.1f;
    s.enhancer_enabled = true; s.enhancer_process = 6.f; s.enhancer_contour = 3.f; s.enhancer_mid = -2.f;
    s.limiter_enabled = true; s.limiter_threshold_db = -3.f; s.limiter_release_ms = 100.f;
    s.convolver_enabled = true; s.convolver_mix = 0.4f; s.convolver_choice = 1;
    s.convolver_decay = 0.3f; s.convolver_tone = 0.7f; s.convolver_size = 0.55f;
    s.config_page = 4;
    s.audio_block_size = 128; s.audio_sample_rate = 44100;

    REQUIRE(saveState(p, s));
    auto l = loadState(p);
    REQUIRE(l.has_value());

    CHECK(l->bank_search_dir == s.bank_search_dir);
    CHECK(l->resonance_window_ms == 8000);
    CHECK(l->preload_ms == 300);
    CHECK(l->cache_budget_mb == 2048);
    CHECK(l->agc_enabled == true);
    CHECK(l->agc_target == doctest::Approx(0.2f));
    CHECK(l->agc_release_ms == doctest::Approx(150.f));
    CHECK(l->agc_floor == doctest::Approx(0.1f));
    CHECK(l->enhancer_enabled == true);
    CHECK(l->enhancer_process == doctest::Approx(6.f));
    CHECK(l->enhancer_contour == doctest::Approx(3.f));
    CHECK(l->enhancer_mid == doctest::Approx(-2.f));
    CHECK(l->limiter_enabled == true);
    CHECK(l->limiter_threshold_db == doctest::Approx(-3.f));
    CHECK(l->limiter_release_ms == doctest::Approx(100.f));
    CHECK(l->convolver_enabled == true);
    CHECK(l->convolver_mix == doctest::Approx(0.4f));
    CHECK(l->convolver_choice == 1);
    CHECK(l->convolver_decay == doctest::Approx(0.3f));
    CHECK(l->convolver_tone == doctest::Approx(0.7f));
    CHECK(l->convolver_size == doctest::Approx(0.55f));
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
    CHECK(l->convolver_enabled == false);    // default
    std::filesystem::remove(p);
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
    CHECK(st->window_w >= 320);
    CHECK(st->window_h >= 240);
    CHECK(st->midi_channel == -1);   // mimo rozsah → OMNI
    std::filesystem::remove(p);
}
