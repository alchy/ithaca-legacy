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

// -- resetToDefaults --------------------------------------------------------
// Podklad pro RESET tlacitko v topbaru. Drive melo defaulty hardcodovane
// potreti (vedle GuiState defaultu a Param::def) a zapominalo na
// max_resonance_voices.

TEST_CASE("resetToDefaults vrati vsechny parametry stage na Param::def") {
    DspChain ch;  ch.prepare(48000.f, 512);
    auto& agc = ch.stage(1);
    agc.set(0, 0.42f);
    agc.set(1, 1234.f);
    agc.set(2, 0.9f);

    agc.resetToDefaults();

    for (int j = 0; j < agc.paramCount(); ++j)
        CHECK(agc.get(j) == doctest::Approx(agc.param(j).def));
}

TEST_CASE("resetToDefaults funguje pro kazdou stage chainu") {
    DspChain ch;  ch.prepare(48000.f, 512);
    for (int i = 0; i < ch.stageCount(); ++i) {
        auto& st = ch.stage(i);
        for (int j = 0; j < st.paramCount(); ++j)
            st.set(j, st.param(j).max);      // vychyl na maximum
        st.resetToDefaults();
        for (int j = 0; j < st.paramCount(); ++j)
            CHECK(st.get(j) == doctest::Approx(st.param(j).def));
    }
}

// -- Snapshot pole stranek (SAVE AS DEFAULT / RESET PARAMS na SYS) -----------
// Tohle je mechanismus, ktery uzivatelske defaulty poriduje i vraci. Musi jet
// pres IParamPage, ne pres DspStage — jinak by nepokryl MASTER a RESONANCE.

namespace {

// Minimalni stranka mimo DSP retezec: presne ten pripad, kvuli kteremu
// snapshotPages nesmi byt vazany na DspChain.
struct FakePage : ithaca::dsp::IParamPage {
    float a = 1.f, b = 2.f;
    bool  on = false;
    int   choice = -1;
    const char* name() const override { return "FAKE"; }
    int paramCount() const override { return 2; }
    const ithaca::dsp::Param& param(int i) const override { return kP[i]; }
    float get(int i) const override { return i == 0 ? a : b; }
    void  set(int i, float v) override { (i == 0 ? a : b) = v; }
    bool  hasEnable() const override { return true; }
    bool  enabled() const override { return on; }
    void  setEnabled(bool v) override { on = v; }
    bool  meter(float&, const char*&) const override { return false; }
    int   choiceCount() const override { return 3; }
    const char* choiceName(int) const override { return "X"; }
    int   currentChoice() const override { return choice; }
    void  selectChoice(int i) override { choice = i; }
    static constexpr ithaca::dsp::Param kP[2] = {
        {"alpha", "ALPHA", 0.f, 10.f, 1.f, "%.1f", false},
        {"beta",  "BETA",  0.f, 10.f, 2.f, "%.1f", false},
    };
};

} // namespace

TEST_CASE("snapshotPages/applyPagesState projdou i strankou mimo DspChain") {
    FakePage fp;
    fp.a = 7.5f; fp.b = 3.25f; fp.on = true; fp.choice = 2;
    ithaca::dsp::IParamPage* pages[] = { &fp };

    std::map<std::string, DspStageState> snap;
    snapshotPages(snap, pages, 1);

    REQUIRE(snap.count("FAKE") == 1);
    CHECK(snap.at("FAKE").params.at("alpha") == doctest::Approx(7.5f));
    CHECK(snap.at("FAKE").params.at("beta")  == doctest::Approx(3.25f));
    CHECK(snap.at("FAKE").enabled == true);
    CHECK(snap.at("FAKE").choice == 2);

    // Rozhaz vsechno a vrat ze snapshotu.
    fp.a = 0.f; fp.b = 0.f; fp.on = false; fp.choice = 0;
    applyPagesState(snap, pages, 1);

    CHECK(fp.a == doctest::Approx(7.5f));
    CHECK(fp.b == doctest::Approx(3.25f));
    CHECK(fp.on == true);
    CHECK(fp.choice == 2);
}

TEST_CASE("applyPagesState ignoruje stranku, ktera ve snapshotu neni") {
    FakePage fp;  fp.a = 5.f;
    ithaca::dsp::IParamPage* pages[] = { &fp };

    std::map<std::string, DspStageState> snap;
    snap["JINA_STRANKA"] = {true, -1, {{"alpha", 99.f}}};
    applyPagesState(snap, pages, 1);

    CHECK(fp.a == doctest::Approx(5.f));   // nedotcena
}

TEST_CASE("Snapshot cele GUI sady pokryje DSP retezec i stranky mimo nej") {
    DspChain ch;  ch.prepare(48000.f, 512);
    FakePage fp;
    ithaca::dsp::IParamPage* pages[] = {
        &fp, &ch.stage(0), &ch.stage(1), &ch.stage(2), &ch.stage(3),
    };

    std::map<std::string, DspStageState> snap;
    snapshotPages(snap, pages, 5);
    CHECK(snap.size() == 5);
    CHECK(snap.count("FAKE") == 1);
    CHECK(snap.count("AGC") == 1);

    // Zmena po snapshotu se musi dat vratit.
    ch.stage(1).set(0, 0.9f);
    applyPagesState(snap, pages, 5);
    CHECK(ch.stage(1).get(0) != doctest::Approx(0.9f));
}
