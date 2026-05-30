// tests/test_harmonic_proximity.cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "resonance/harmonic_proximity.h"

using namespace ithaca;

TEST_CASE("nota sama na sebe = 0 (resi se jinde, play-on-self)") {
    CHECK(harmonicProximity(60, 60) == doctest::Approx(0.f));
}

TEST_CASE("oktavy klesaji s vzdalenosti") {
    float p1 = harmonicProximity(60, 72);   // +1 oktava
    float p2 = harmonicProximity(60, 84);   // +2 oktavy
    float p3 = harmonicProximity(60, 96);   // +3 oktavy
    CHECK(p1 > p2);
    CHECK(p2 > p3);
    CHECK(p1 > 0.5f);
    CHECK(p3 > 0.f);
}

TEST_CASE("kvinta > kvarta > tercia") {
    float q5 = harmonicProximity(60, 67);   // kvinta
    float q4 = harmonicProximity(60, 65);   // kvarta
    float t3 = harmonicProximity(60, 64);   // velka tercia
    CHECK(q5 > q4);
    CHECK(q4 > t3);
}

TEST_CASE("tritonus / nahodne intervaly = ~0") {
    CHECK(harmonicProximity(60, 66) < 0.05f);   // tritonus
    CHECK(harmonicProximity(60, 61) < 0.05f);   // sekunda
}

TEST_CASE("symetrie: f(a,b) == f(b,a)") {
    for (int a : {21, 36, 60, 84, 108}) {
        for (int b : {24, 48, 67, 72, 96}) {
            CHECK(harmonicProximity(a, b) == doctest::Approx(harmonicProximity(b, a)));
        }
    }
}
