// tests/test_gui_dsp_state.cpp - zrcadleni mezi DspChain a GuiState::dsp.
// Tohle je vrstva, ktera drive byla rucne vypsana na dvou mistech (initFromState
// + zrcadleni v render loopu) a pri pridani parametru se na ni zapominalo.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../app/gui/dsp_state.h"
#include "dsp/dsp_chain.h"

using namespace ithaca::gui;
using ithaca::dsp::DspChain;

TEST_CASE("Zrcadleni DspChain -> GuiState pokryje vsechny stage a parametry") {
    DspChain ch;  ch.prepare(48000.f, 512);
    GuiState s;
    dspStateFromChain(s, ch);

    REQUIRE((int)s.dsp.size() == ch.stageCount());
    for (int i = 0; i < ch.stageCount(); ++i) {
        auto& page = ch.stage(i);
        REQUIRE(s.dsp.count(page.name()) == 1);
        const auto& st = s.dsp.at(page.name());
        CHECK((int)st.params.size() == page.paramCount());
        // Klice musi byt presne Param::id — na tom stoji cela persistence.
        for (int j = 0; j < page.paramCount(); ++j)
            CHECK(st.params.count(page.param(j).id) == 1);
    }
}

TEST_CASE("Zrcadleni prenese hodnoty i enabled flag") {
    DspChain ch;  ch.prepare(48000.f, 512);
    ch.stage(1).set(0, 0.25f);        // AGC target_rms
    ch.stage(1).setEnabled(true);
    ch.stage(3).set(0, -6.f);         // LIMITER threshold_db
    ch.stage(3).setEnabled(false);

    GuiState s;
    dspStateFromChain(s, ch);

    CHECK(s.dsp.at("AGC").enabled == true);
    CHECK(s.dsp.at("AGC").params.at("target_rms") == doctest::Approx(0.25f));
    CHECK(s.dsp.at("LIMITER").enabled == false);
    CHECK(s.dsp.at("LIMITER").params.at("threshold_db") == doctest::Approx(-6.f));
}

TEST_CASE("Convolver choice (volic IR) projde zrcadlenim") {
    DspChain ch;  ch.prepare(48000.f, 512);
    auto& cv = ch.stage(0);
    REQUIRE(cv.choiceCount() > 1);
    cv.selectChoice(1);

    GuiState s;
    dspStateFromChain(s, ch);
    CHECK(s.dsp.at("CONVOLVER").choice == 1);

    // Stage bez volice musi mit choice = -1 (jinak by se do JSONu psal nesmysl).
    CHECK(s.dsp.at("AGC").choice == -1);
}

TEST_CASE("Round-trip: apply(mirror(chain)) obnovi stejny stav") {
    DspChain src;  src.prepare(48000.f, 512);
    src.stage(0).setEnabled(true);
    src.stage(0).set(0, 0.4f);        // CONVOLVER mix
    src.stage(0).selectChoice(1);
    src.stage(1).set(2, 0.3f);        // AGC gain_floor
    src.stage(2).setEnabled(true);
    src.stage(2).set(1, 4.f);         // ENHANCER contour
    src.stage(3).set(1, 500.f);       // LIMITER release_ms

    GuiState s;
    dspStateFromChain(s, src);

    DspChain dst;  dst.prepare(48000.f, 512);
    applyDspStateToChain(s, dst);

    for (int i = 0; i < src.stageCount(); ++i) {
        CHECK(dst.stage(i).enabled() == src.stage(i).enabled());
        CHECK(dst.stage(i).currentChoice() == src.stage(i).currentChoice());
        for (int j = 0; j < src.stage(i).paramCount(); ++j)
            CHECK(dst.stage(i).get(j) == doctest::Approx(src.stage(i).get(j)));
    }
}

TEST_CASE("apply ignoruje neznamou stage i neznamy parametr (stary state.json)") {
    GuiState s;
    s.dsp["NEEXISTUJICI_STAGE"] = {true, -1, {{"foo", 1.f}}};
    s.dsp["AGC"] = {true, -1, {{"target_rms", 0.3f}, {"zruseny_param", 9.f}}};

    DspChain ch;  ch.prepare(48000.f, 512);
    applyDspStateToChain(s, ch);   // nesmi spadnout

    CHECK(ch.stage(1).enabled() == true);
    CHECK(ch.stage(1).get(0) == doctest::Approx(0.3f));
}

TEST_CASE("apply nechá chybejici parametr na jeho soucasne hodnote") {
    DspChain ch;  ch.prepare(48000.f, 512);
    const float before = ch.stage(1).get(1);   // AGC release_ms

    GuiState s;
    s.dsp["AGC"] = {false, -1, {{"target_rms", 0.2f}}};   // release_ms chybi
    applyDspStateToChain(s, ch);

    CHECK(ch.stage(1).get(0) == doctest::Approx(0.2f));
    CHECK(ch.stage(1).get(1) == doctest::Approx(before));
}
