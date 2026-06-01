// tests/test_dsp.cpp — DSP math + stage + chain unit testy (doctest).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/dsp_math.h"
#include <cmath>

using namespace ithaca;

TEST_CASE("db_to_lin / lin_to_db round-trip") {
    CHECK(dsp::db_to_lin(0.f) == doctest::Approx(1.f));
    CHECK(dsp::db_to_lin(-6.f) == doctest::Approx(0.5012f).epsilon(0.01));
    CHECK(dsp::lin_to_db(1.f) == doctest::Approx(0.f));
    CHECK(dsp::lin_to_db(dsp::db_to_lin(-12.f)) == doctest::Approx(-12.f).epsilon(0.001));
}

TEST_CASE("biquad s identitnimi koeficienty propusti signal beze zmeny") {
    dsp::BiquadCoeffs c;            // default b0=1, vse ostatni 0 = passthrough
    dsp::BiquadState s;
    CHECK(dsp::biquad_tick(0.3f, c, s) == doctest::Approx(0.3f));
    CHECK(dsp::biquad_tick(-0.7f, c, s) == doctest::Approx(-0.7f));
}

TEST_CASE("rbj shelf s 0 dB je temer pruhledny na DC") {
    auto c = dsp::rbj_low_shelf(180.f, 0.f, 48000.f);
    dsp::BiquadState s;
    float y = 0.f;
    for (int i = 0; i < 256; ++i) y = dsp::biquad_tick(1.f, c, s);  // DC vstup
    CHECK(y == doctest::Approx(1.f).epsilon(0.02));
}
