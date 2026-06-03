// tests/test_pedal_state.cpp
// PedalState je SPOJITY: damping_[N] in [0, 1] dle CC64 a stavu klaves.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "pedal/pedal_state.h"

using namespace ithaca;

TEST_CASE("PedalState: pedal UP (CC=0) — undamped jen drzene klavesy") {
    PedalState p;
    p.noteOn(60); p.noteOn(64); p.noteOn(67);
    p.setSustainCC(0);
    CHECK(p.dampingFor(60) == doctest::Approx(1.0f));   // drzena = vzdy 1.0
    CHECK(p.dampingFor(64) == doctest::Approx(1.0f));
    CHECK(p.dampingFor(67) == doctest::Approx(1.0f));
    CHECK(p.dampingFor(72) == doctest::Approx(0.0f));   // ne drzena + pedal up = 0
    CHECK(p.dampingFor(50) == doctest::Approx(0.0f));
    p.noteOff(64);  // klavesa pustena, pedal up = ztlumi se
    CHECK(p.dampingFor(64) == doctest::Approx(0.0f));
}

TEST_CASE("PedalState: pedal DOWN (CC=127) — vsechny struny undamped (1.0)") {
    PedalState p;
    p.noteOn(60);
    p.setSustainCC(127);
    for (int n = 0; n < 128; ++n)
        CHECK(p.dampingFor(n) == doctest::Approx(1.0f));
    // note-off s pedalem dole nemeni damping (pedal drzi sustain).
    p.noteOff(60);
    CHECK(p.dampingFor(60) == doctest::Approx(1.0f));
}

TEST_CASE("PedalState: half-pedal (CC=64) — ne-drzene maji 0.5, drzene 1.0") {
    PedalState p;
    p.noteOn(60);
    p.setSustainCC(64);
    CHECK(p.dampingFor(60) == doctest::Approx(1.0f));        // drzena = vzdy 1.0
    CHECK(p.dampingFor(72) == doctest::Approx(64.f/127.f).epsilon(0.001));
    // CC=32 = ~25% pedalu
    p.setSustainCC(32);
    CHECK(p.dampingFor(72) == doctest::Approx(32.f/127.f).epsilon(0.001));
}

TEST_CASE("PedalState: spojita zmena CC se promita lineárne") {
    PedalState p;
    // bez drzeni: damping ne-drzene noty = cc/127
    for (int cc : {0, 16, 32, 48, 64, 80, 96, 112, 127}) {
        p.setSustainCC((uint8_t)cc);
        float expected = (float)cc / 127.f;
        CHECK(p.dampingFor(50) == doctest::Approx(expected).epsilon(0.001));
    }
}

TEST_CASE("PedalState: dolni lost-motion dead-zona — pod kDamperBiteCC damping 0") {
    PedalState p;
    for (int cc = 0; cc <= (int)kDamperBiteCC; ++cc) {
        p.setSustainCC((uint8_t)cc);
        CHECK(p.dampingFor(50) == doctest::Approx(0.0f));   // dead-zone → plne tlumeno
        CHECK_FALSE(p.isUndamped(50));
    }
    p.setSustainCC((uint8_t)(kDamperBiteCC + 1));            // tesne nad zonou
    CHECK(p.dampingFor(50) > 0.f);                           // uz lize struny
    CHECK(p.isUndamped(50));
}

TEST_CASE("PedalState: nad dead-zonou spojite cc/127 (half-pedal feel)") {
    PedalState p;
    for (int cc : {16, 32, 48, 64, 96, 127}) {              // vse > kDamperBiteCC
        p.setSustainCC((uint8_t)cc);
        CHECK(p.dampingFor(50) == doctest::Approx((float)cc / 127.f).epsilon(0.001));
    }
}

TEST_CASE("PedalState: helper isPedalDown / sustainCC pro half-pedal release scaling") {
    PedalState p;
    // 0..63 = up, 64..127 = down (klasicky MIDI prah pro on/off).
    p.setSustainCC(63);  CHECK_FALSE(p.isPedalDown());
    p.setSustainCC(64);  CHECK(p.isPedalDown());
    // Continuous hodnota pro release scaling (faze 5: scaledReleaseMs).
    p.setSustainCC(96);  CHECK(p.sustainCC() == 96);
}

TEST_CASE("PedalState: allNotesOff — drzene klavesy se uvolni") {
    PedalState p;
    p.noteOn(60); p.noteOn(64);
    p.setSustainCC(0);
    p.allNotesOff();
    CHECK(p.dampingFor(60) == doctest::Approx(0.0f));
    CHECK(p.dampingFor(64) == doctest::Approx(0.0f));
}
