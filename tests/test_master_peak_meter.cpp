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

TEST_CASE("Master peak meter - real signal feed + decay") {
    // Vyuzivame faktu, ze Engine::processBlock je buffer-additivni (ne-zeruje
    // vstup) a bez banky/voicu/rezonance se vstup nesahne. Master_gain=1.0
    // → multiplikator preskocime (|g-1|<0.001). Peak meter pak cte z bufferu
    // vstupni hodnotu.
    using namespace ithaca;
    Engine e; EngineConfig cfg;
    cfg.sample_rate = 48000; cfg.block_size = 256;
    cfg.master_gain = 1.f;
    REQUIRE(e.init(cfg));

    // 1) Pre-fill bufferu konstantou 0.5 → peak by mel byt presne 0.5 po
    //    prvnim bloku (engine nepricte nic, master_gain je 1.0, |abs| dava 0.5).
    std::vector<float> L(256, 0.5f), R(256, 0.5f);
    e.processBlock(L.data(), R.data(), 256);
    CHECK(e.masterPeakL() == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(e.masterPeakR() == doctest::Approx(0.5f).epsilon(0.01));

    // 2) Po 10 tichych blocich peak klesa pres decay. Decay per blok je
    //    exp(-256/(0.1*48000)) ≈ 0.948; po 10 blocich ≈ 0.583.
    //    Ocekavany peak: 0.5 × 0.583 ≈ 0.291.
    for (int i = 0; i < 10; ++i) {
        std::vector<float> sL(256, 0.f), sR(256, 0.f);
        e.processBlock(sL.data(), sR.data(), 256);
    }
    CHECK(e.masterPeakL() < 0.45f);   // klesl pod puvodni 0.5
    CHECK(e.masterPeakL() > 0.05f);   // ale jeste neutichl uplne
    CHECK(e.masterPeakR() < 0.45f);
    CHECK(e.masterPeakR() > 0.05f);
}

TEST_CASE("Master peak meter - bank_loading silences output") {
    // Followup #1 vedlejsi: kdyz reloadBank pousti audio thread do "silence
    // mode" pres bank_loading_, processBlock musi vystup vynulovat a peak
    // metr srazit na 0. Nemuzeme spustit reloadBank na non-init engine,
    // ale muzeme overit ze bez triggeru se klasicke chovani drzi (regresni
    // ochrana proti nahodnemu zapnuti flagu).
    using namespace ithaca;
    Engine e; EngineConfig cfg;
    cfg.sample_rate = 48000; cfg.block_size = 256;
    cfg.master_gain = 1.f;
    REQUIRE(e.init(cfg));
    std::vector<float> L(256, 0.7f), R(256, 0.7f);
    e.processBlock(L.data(), R.data(), 256);
    // Bez bank_loading_ vstup projde a peak je nenulovy.
    CHECK(e.masterPeakL() > 0.5f);
}
