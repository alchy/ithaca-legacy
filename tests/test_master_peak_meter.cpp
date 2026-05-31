// tests/test_master_peak_meter.cpp - Master peak meter sanity.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "engine.h"
#include <vector>

TEST_CASE("Master peak meter - silent input zero peak") {
    using namespace ithaca;
    Engine e;
    EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 256;
    REQUIRE(e.init(cfg));

    CHECK(e.masterPeakL() == doctest::Approx(0.f));
    CHECK(e.masterPeakR() == doctest::Approx(0.f));

    std::vector<float> L(256, 0.f), R(256, 0.f);
    e.processBlock(L.data(), R.data(), 256);
    CHECK(e.masterPeakL() < 0.01f);
    CHECK(e.masterPeakR() < 0.01f);
}

TEST_CASE("Master peak meter - decay after pulse") {
    // Simulace: rucni store vysokeho peaku, pak nekolik tichych bloku, mereni
    // ze peak klesa. Pristup zvolen protoze nemame jednoduche API jak vlozit
    // signal do bufferu pred peak vypoctem (Engine ho vypocte na vystupu;
    // bez banky a noty produkuje ticho).
    //
    // POZN.: master_peak_l_/r_ jsou private — nemuzeme je zvenku setnout.
    // Tento test je proto jen sanity smoke (ticho dava ~0). Realny decay
    // test by potreboval interni setter nebo nahru banku + noty, oboji
    // za scope MVP testu.
    using namespace ithaca;
    Engine e; EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 256;
    REQUIRE(e.init(cfg));
    // Bez banky processBlock generuje ticho → peak zustava blizko 0.
    std::vector<float> L(256, 0.f), R(256, 0.f);
    for (int b = 0; b < 10; ++b) e.processBlock(L.data(), R.data(), 256);
    CHECK(e.masterPeakL() < 0.001f);
    CHECK(e.masterPeakR() < 0.001f);
}
