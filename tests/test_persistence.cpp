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
    s.resonance_strength  = 0.7f;
    s.release_ms          = 250.f;
    s.excite_decay_ms     = 4000.f;
    s.max_resonance_voices = 16;
    s.window_x = 200; s.window_y = 300; s.window_w = 800; s.window_h = 600;

    REQUIRE(saveState(p, s));
    auto loaded = loadState(p);
    REQUIRE(loaded.has_value());
    CHECK(loaded->bank_path == s.bank_path);
    CHECK(loaded->midi_port_name == s.midi_port_name);
    CHECK(loaded->master_gain_db == doctest::Approx(s.master_gain_db));
    CHECK(loaded->resonance_strength == doctest::Approx(s.resonance_strength));
    CHECK(loaded->release_ms == doctest::Approx(s.release_ms));
    CHECK(loaded->excite_decay_ms == doctest::Approx(s.excite_decay_ms));
    CHECK(loaded->max_resonance_voices == 16);
    CHECK(loaded->window_w == 800);
    CHECK(loaded->window_h == 600);

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
