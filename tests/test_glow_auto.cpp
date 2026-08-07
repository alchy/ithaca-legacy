// tests/test_glow_auto.cpp — regulator dosahu zare vlny.
//
// Regulator, ktery meni vzhled za behu, je nebezpecna vec: kdyz kmita, je to
// videt, a kdyz kmita v takt s hranim, je to videt nejvic. Testuje se proto
// hlavne to, co delat NEMA — reagovat na spicku, kmitat kolem prahu a vracet
// se prilis rychle.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "glow_auto.h"

using ithaca::gui::GlowAuto;

namespace {
constexpr float kDt = 1.f / 60.f;

// Odsimuluje `secs` vterin pri konstantni periode snimku.
void run(GlowAuto& g, float frame_ms, float want, float budget, float secs) {
    for (int i = 0; i < (int)(secs / kDt); ++i) g.step(frame_ms, want, budget, kDt);
}
} // namespace

TEST_CASE("bez rozpoctu se nic nemeni") {
    GlowAuto g;
    g.reset(1.f);
    run(g, 500.f, 1.f, 0.f, 60.f);          // katastrofalni perioda, rozpocet 0
    CHECK(g.scale() == doctest::Approx(1.f));
}

TEST_CASE("v pohode se dosah nesnizuje") {
    GlowAuto g;
    g.reset(1.f);
    run(g, 16.7f, 1.f, 25.f, 120.f);
    CHECK(g.scale() == doctest::Approx(1.f));
}

TEST_CASE("kratka spicka dosah nesnizi") {
    GlowAuto g;
    g.reset(1.f);
    run(g, 16.7f, 1.f, 25.f, 10.f);
    // Pul vteriny vypadku — presne to, co delaji planovac nebo load banky.
    run(g, 200.f, 1.f, 25.f, 0.5f);
    run(g, 16.7f, 1.f, 25.f, 10.f);
    CHECK(g.scale() == doctest::Approx(1.f));
}

TEST_CASE("trvale prekroceni dosah snizi") {
    GlowAuto g;
    g.reset(1.f);
    run(g, 40.f, 1.f, 25.f, 20.f);
    CHECK(g.scale() < 1.f);
}

TEST_CASE("automatika zar nikdy nezhasne uplne") {
    GlowAuto g;
    g.reset(1.f);
    run(g, 5000.f, 1.f, 25.f, 600.f);       // deset minut naprosteho zoufalstvi
    CHECK(g.scale() > 0.f);
}

TEST_CASE("navrat nahoru je pomalejsi nez pokles") {
    GlowAuto down;
    down.reset(1.f);
    run(down, 40.f, 1.f, 25.f, 20.f);
    const float after_down = down.scale();
    REQUIRE(after_down < 1.f);

    // Stejne dlouhy klid nesmi vratit vic, nez kolik ubralo stejne dlouhe
    // prekroceni — jinak by regulator kolem prahu kmital.
    GlowAuto up = down;
    run(up, 8.f, 1.f, 25.f, 20.f);
    CHECK(up.scale() <= 1.f);
    CHECK(up.scale() >= after_down);
}

TEST_CASE("po dlouhem klidu se dosah vrati na prani uzivatele") {
    GlowAuto g;
    g.reset(1.f);
    run(g, 40.f, 1.f, 25.f, 30.f);
    REQUIRE(g.scale() < 1.f);
    run(g, 8.f, 1.f, 25.f, 300.f);
    CHECK(g.scale() == doctest::Approx(1.f));
}

TEST_CASE("mezi prahy regulator nic nedela") {
    GlowAuto g;
    g.reset(1.f);
    run(g, 40.f, 1.f, 25.f, 20.f);
    const float settled = g.scale();
    // Pasmo necinnosti: pod rozpoctem, ale ne pod jeho polovinou.
    run(g, 20.f, 1.f, 25.f, 300.f);
    CHECK(g.scale() == doctest::Approx(settled));
}

TEST_CASE("snizeni parametru uzivatelem plati okamzite") {
    GlowAuto g;
    g.reset(4.f);
    g.step(16.7f, 0.5f, 25.f, kDt);        // uzivatel stahl na 0.5
    CHECK(g.scale() <= 0.5f);
}

TEST_CASE("hola cara je legitimni prani") {
    GlowAuto g;
    g.reset(0.f);
    run(g, 16.7f, 0.f, 25.f, 60.f);
    CHECK(g.scale() == doctest::Approx(0.f));
}
