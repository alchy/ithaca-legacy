// tests/test_engine_diagnostics.cpp - Engine gettery pro GUI diagnostiku.
// Overuje, ze cerstve inicializovany Engine (bez nactene banky) hlasi
// vsechny diagnosticke metriky jako "nic": zadne aktivni hlasy/ringy/pedal.
// Cilem je pojistit, ze gettery jsou bezpecne volat i v "prazdnem" stavu
// (typicke pri startu GUI, kdy se panely vykresluji jeste pred loadBank).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine.h"

TEST_CASE("Engine diagnostic getters - fresh engine no bank") {
    using namespace ithaca;
    Engine e;
    EngineConfig cfg;
    cfg.sample_rate = 48000;
    cfg.block_size  = 256;
    REQUIRE(e.init(cfg));

    // Cerstvy engine: nic neni aktivni.
    CHECK(e.activeVoices()    == 0);
    CHECK(e.resonanceVoices() == 0);
    CHECK(e.numRingsUsed()    == 0);
    CHECK(e.pedalCC()         == 0);

    // Maska 128 not — vse false.
    bool mask[128];
    e.activeMidiNotes(mask);
    for (int i = 0; i < 128; ++i) CHECK_FALSE(mask[i]);

    // currentGainFor: 0 pro platnou notu i out-of-range (musi byt safe).
    CHECK(e.currentGainFor(60)  == 0.f);
    CHECK(e.currentGainFor(-1)  == 0.f);
    CHECK(e.currentGainFor(128) == 0.f);
}
