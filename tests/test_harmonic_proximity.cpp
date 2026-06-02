// tests/test_harmonic_proximity.cpp — partial-coincidence rezonancni model.
// POZN.: model uz NENI symetricky (oktava nahoru != oktava dolu) — budi se
// jine parcialy, viz spec 2026-06-02-resonance-partial-coincidence-design.md.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "resonance/harmonic_proximity.h"
#include <cmath>
using namespace ithaca;

// harmonicProximity(target N, source P): jak silne nota P budi strunu N.

TEST_CASE("self je 0 (play-on-self resi voice pool)") {
    CHECK(harmonicProximity(60, 60) == 0.f);
}

TEST_CASE("poradi sily nahoru: oktava > kvinta > V.tercie") {
    CHECK(harmonicProximity(72, 60) > harmonicProximity(67, 60));   // okt > kvinta
    CHECK(harmonicProximity(67, 60) > harmonicProximity(64, 60));   // kvinta > tercie
}

TEST_CASE("up/down asymetrie: oktava nahoru > oktava dolu > 0") {
    CHECK(harmonicProximity(72, 60) > harmonicProximity(48, 60));   // up > down
    CHECK(harmonicProximity(48, 60) > 0.f);                          // dolu stale rezonuje
}

TEST_CASE("oktava nahoru = normalizovane maximum (~1.0)") {
    CHECK(harmonicProximity(72, 60) == doctest::Approx(1.0f).epsilon(0.001));
    for (int t = 0; t < 128; ++t)
        CHECK(harmonicProximity(t, 60) <= harmonicProximity(72, 60) + 1e-4f);
}

TEST_CASE("pokles se vzdalenosti: 2 oktavy nahoru < 1 oktava nahoru") {
    CHECK(harmonicProximity(84, 60) < harmonicProximity(72, 60));
}

TEST_CASE("tritonus / sekunda = slabe (< 0.05)") {
    CHECK(harmonicProximity(66, 60) < 0.05f);   // tritonus
    CHECK(harmonicProximity(61, 60) < 0.05f);   // m2
}

TEST_CASE("rozsah [0,1] a koncne hodnoty") {
    for (int t = 0; t < 128; ++t) {
        float v = harmonicProximity(t, 60);
        CHECK(v >= 0.f);
        CHECK(v <= 1.0f + 1e-4f);
        CHECK(std::isfinite(v));
    }
}
