// tests/test_pace.cpp — prepinani tempa prekreslovani.
//
// Regulator, ktery meni tempo panelu, se posuzuje hlavne podle dvou veci:
// jestli zrychli DOST RYCHLE (zpozdena odpoved na dotek je na pristroji
// nejvic znat) a jestli neprepina porad dokola (kazde prepnuti swapIntervalu
// muze stat jedno skubnuti).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "pace.h"

using ithaca::gui::PaceControl;

namespace {
constexpr float kDt = 1.f / 60.f;

int run(PaceControl& p, bool busy, float secs, int fast, int slow) {
    int last = fast;
    for (int i = 0; i < (int)(secs / kDt); ++i) last = p.step(busy, kDt, fast, slow);
    return last;
}
} // namespace

TEST_CASE("bez zpomalovani se tempo nemeni") {
    PaceControl p;
    CHECK(run(p, false, 60.f, 1, 0) == 1);      // slow = 0 → vypnuto
    CHECK(run(p, false, 60.f, 2, 2) == 2);      // slow == fast → nema co delat
    CHECK(run(p, false, 60.f, 2, 1) == 2);      // slow < fast → nesmysl, ignoruj
}

TEST_CASE("pri cinnosti se nezpomaluje nikdy") {
    PaceControl p;
    CHECK(run(p, true, 600.f, 1, 4) == 1);
}

TEST_CASE("v klidu se zpomali az po prodleve") {
    PaceControl p;
    // Tesne pred prahem jeste plne tempo.
    CHECK(run(p, false, PaceControl::kIdleHold - 0.2f, 1, 4) == 1);
    // Po prahu uz zpomalene.
    CHECK(run(p, false, 0.5f, 1, 4) == 4);
}

TEST_CASE("zrychleni je OKAMZITE, uz prvnim snimkem") {
    PaceControl p;
    REQUIRE(run(p, false, 10.f, 1, 4) == 4);    // zpomalene
    // Jediny snimek s cinnosti musi vratit plne tempo — na dotek se ceka
    // nejhur ze vseho.
    CHECK(p.step(true, kDt, 1, 4) == 1);
}

TEST_CASE("kratka pomlka mezi notami tempo neshodi") {
    PaceControl p;
    run(p, true, 5.f, 1, 4);
    // Vteřina ticha mezi frazemi — pod prahem, tempo se drzi.
    CHECK(run(p, false, 1.0f, 1, 4) == 1);
    CHECK(p.step(true, kDt, 1, 4) == 1);
}

TEST_CASE("po zrychleni se prodleva pocita znovu od nuly") {
    PaceControl p;
    run(p, false, PaceControl::kIdleHold - 0.2f, 1, 4);   // skoro u prahu
    p.step(true, kDt, 1, 4);                              // dotek
    // Kdyby se citac nevynuloval, zpomalilo by to hned po dvou desetinach.
    CHECK(run(p, false, 0.5f, 1, 4) == 1);
}

TEST_CASE("dlouhy vypadek se do prodlevy nepocita") {
    PaceControl p;
    // Jeden dvouvterinovy snimek (load banky, prepnuti okna) nesmi prodlevu
    // preskocit — jinak by panel po kazdem zadrhnuti skocil do usporneho tempa.
    p.step(false, 2.0f, 1, 4);
    CHECK(p.idleSeconds() == doctest::Approx(0.f));
}

TEST_CASE("nesmyslny fast se srovna") {
    PaceControl p;
    CHECK(p.step(true, kDt, 0, 4) == 1);
    CHECK(p.step(true, kDt, -3, 4) == 1);
}
