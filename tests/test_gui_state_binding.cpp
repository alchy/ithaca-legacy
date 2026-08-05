// tests/test_gui_state_binding.cpp - mapovani GuiState -> EngineConfig / Engine.
// Tahle vrstva byla drive zavarena uvnitr AppContext::initFromState, tedy
// neotestovatelna (vyzadovala audio device + MIDI + nactenou banku).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../app/gui/state_binding.h"

using namespace ithaca::gui;

TEST_CASE("engineConfigFromState prevede dB na linearni master gain") {
    GuiState s;
    s.master_gain_db = 0.f;
    CHECK(engineConfigFromState(s).master_gain == doctest::Approx(1.f));

    s.master_gain_db = -6.f;
    CHECK(engineConfigFromState(s).master_gain == doctest::Approx(0.501187f));

    s.master_gain_db = 6.f;
    CHECK(engineConfigFromState(s).master_gain == doctest::Approx(1.995262f));
}

TEST_CASE("engineConfigFromState sanitizuje sample rate") {
    // state.json muze byt rucne editovany; SR <= 0 by delilo nulou v load metru.
    GuiState s;
    s.audio_sample_rate = 0;
    CHECK(engineConfigFromState(s).sample_rate == 48000);

    s.audio_sample_rate = -44100;
    CHECK(engineConfigFromState(s).sample_rate == 48000);

    s.audio_sample_rate = 44100;
    CHECK(engineConfigFromState(s).sample_rate == 44100);
}

TEST_CASE("engineConfigFromState clampuje block size na [32, 8192]") {
    GuiState s;
    s.audio_block_size = 8;
    CHECK(engineConfigFromState(s).block_size == 32);

    s.audio_block_size = 999999;
    CHECK(engineConfigFromState(s).block_size == 8192);

    s.audio_block_size = 512;
    CHECK(engineConfigFromState(s).block_size == 512);
}

TEST_CASE("engineConfigFromState zapise validovane hodnoty ZPET do state") {
    // Bez toho by GUI zobrazovalo jinou hodnotu, nez jakou engine skutecne jede,
    // a pri pristim ulozeni by se nesmysl z JSONu zapsal znovu.
    GuiState s;
    s.audio_sample_rate = 0;
    s.audio_block_size  = 4;
    (void)engineConfigFromState(s);
    CHECK(s.audio_sample_rate == 48000);
    CHECK(s.audio_block_size == 32);
}

TEST_CASE("engineConfigFromState zapne RT prioritu") {
    // GUI je realna audio aplikace. Default v EngineConfig je false, aby ji
    // testy a offline render nedostaly — tady ji chceme explicitne.
    GuiState s;
    CHECK(engineConfigFromState(s).rt_priority == true);
}

TEST_CASE("engineConfigFromState prenese rezonancni a ladici pole") {
    GuiState s;
    s.resonance_enabled    = false;
    s.resonance_gain_db    = -9.f;
    s.resonance_layer_db   = -21.f;
    s.release_ms           = 321.f;
    s.excite_decay_ms      = 4321.f;
    s.max_resonance_voices = 17;
    s.resonance_window_ms  = 9000;
    s.preload_ms           = 222;
    s.cache_budget_mb      = 1024;

    const auto cfg = engineConfigFromState(s);
    CHECK(cfg.resonance_enabled == false);
    CHECK(cfg.resonance_gain_db == doctest::Approx(-9.f));
    CHECK(cfg.resonance_layer_db == doctest::Approx(-21.f));
    CHECK(cfg.release_ms == doctest::Approx(321.f));
    CHECK(cfg.excite_decay_ms == doctest::Approx(4321.f));
    CHECK(cfg.max_resonance_voices == 17);
    CHECK(cfg.resonance_window_ms == 9000);
    CHECK(cfg.preload_ms == 222);
    CHECK(cfg.cache_budget_mb == 1024);
}

TEST_CASE("applyStateToEngine nastavi runtime hodnoty na engine") {
    // Pozn.: Engine nema gettery pro resonance enabled/gain, takze overit lze
    // jen maxResonanceVoices. Gettery jen kvuli testu nepridavame — zbytek
    // funkce je primocara sekvence setteru.
    GuiState s;
    s.master_gain_db       = -12.f;
    s.release_ms           = 400.f;
    s.excite_decay_ms      = 7000.f;
    s.resonance_enabled    = false;
    s.resonance_gain_db    = -20.f;
    s.max_resonance_voices = 8;

    ithaca::Engine e;
    auto cfg = engineConfigFromState(s);
    cfg.max_resonance_voices = 32;          // jina hodnota nez ve state
    REQUIRE(e.init(cfg));

    applyStateToEngine(e, s);
    CHECK(e.maxResonanceVoices() == 8);     // prepsano ze state
}
