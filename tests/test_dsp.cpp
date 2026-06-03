// tests/test_dsp.cpp — DSP math + stage + chain unit testy (doctest).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/dsp_math.h"
#include "dsp/limiter.h"
#include "dsp/enhancer.h"
#include "dsp/agc.h"
#include "dsp/dsp_chain.h"
#include "engine.h"
#include <cmath>
#include <string>
#include <vector>

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

TEST_CASE("Limiter: disabled = bit-identicky bypass") {
    dsp::Limiter lim; lim.prepare(48000.f, 512);
    lim.setEnabled(false);
    float L[4] = {0.9f, -0.8f, 0.5f, -0.2f}, R[4] = {0.7f, 0.6f, -0.9f, 0.1f};
    float L0[4]; for (int i=0;i<4;++i) L0[i]=L[i];
    lim.process(L, R, 4);
    for (int i=0;i<4;++i) CHECK(L[i] == L0[i]);
}

TEST_CASE("Limiter: signal nad prahem je omezen na ~threshold") {
    dsp::Limiter lim; lim.prepare(48000.f, 4800);
    lim.setEnabled(true);
    lim.set(0, -12.f);   // THRESHOLD -12 dB  (lin ~0.251)
    lim.set(1, 50.f);    // RELEASE 50 ms
    const float thr = dsp::db_to_lin(-12.f);
    float L[4800], R[4800];
    for (int i=0;i<4800;++i){ L[i]=0.8f; R[i]=0.8f; }
    lim.process(L, R, 4800);
    CHECK(std::abs(L[4799]) <= thr * 1.05f);
    float gr; const char* lbl;
    CHECK(lim.meter(gr, lbl) == true);
    CHECK(gr < 0.f);                 // limituje -> zaporna GR
}

TEST_CASE("Limiter: signal pod prahem projde beze zmeny") {
    dsp::Limiter lim; lim.prepare(48000.f, 256);
    lim.setEnabled(true);
    lim.set(0, -6.f);    // prah ~0.501
    float L[256], R[256];
    for (int i=0;i<256;++i){ L[i]=0.1f; R[i]=0.1f; }
    lim.process(L, R, 256);
    for (int i=0;i<256;++i) CHECK(L[i] == doctest::Approx(0.1f));
}

TEST_CASE("Limiter: set klampuje, param round-trip, enable toggle") {
    dsp::Limiter lim; lim.prepare(48000.f, 256);
    CHECK(lim.paramCount() == 2);
    lim.set(0, 999.f); CHECK(lim.get(0) == doctest::Approx(0.f));    // max 0 dB
    lim.set(0, -999.f); CHECK(lim.get(0) == doctest::Approx(-40.f)); // min -40
    lim.setEnabled(true);  CHECK(lim.enabled());
    lim.setEnabled(false); CHECK(!lim.enabled());
    CHECK(std::string(lim.name()) == "LIMITER");
    CHECK(lim.hasEnable());
}

TEST_CASE("Enhancer: disabled = bypass") {
    dsp::Enhancer e; e.prepare(48000.f, 256);
    e.setEnabled(false); e.set(0, 12.f); e.set(1, 12.f);
    float L[3]={0.1f,0.2f,0.3f}, R[3]={0.1f,0.2f,0.3f};
    e.process(L,R,3);
    CHECK(L[0]==doctest::Approx(0.1f)); CHECK(L[2]==doctest::Approx(0.3f));
}
TEST_CASE("Enhancer: vsechny parametry 0 → pruchozi (DC ~unity po ustaleni)") {
    dsp::Enhancer e; e.prepare(48000.f, 256);
    e.setEnabled(true);                  // process=contour=mid=0 (default)
    std::vector<float> L(8192,1.f), R(8192,1.f);
    e.process(L.data(),R.data(),8192);
    CHECK(L[8191]==doctest::Approx(1.f).epsilon(0.1));   // DC projde ~unity (volna tolerance kvuli crossover)
}
TEST_CASE("Enhancer: CONTOUR boost zvedne DC uroven") {
    dsp::Enhancer e; e.prepare(48000.f, 256);
    e.setEnabled(true); e.set(1, 12.f);  // CONTOUR +12 dB (low band na DC)
    std::vector<float> L(8192,1.f), R(8192,1.f);
    e.process(L.data(),R.data(),8192);
    CHECK(L[8191] > 1.5f);               // low pasmo × ~4 → DC nahoru
}
TEST_CASE("Enhancer: param round-trip + clamp") {
    dsp::Enhancer e; e.prepare(48000.f, 256);
    CHECK(e.paramCount()==3);
    e.set(0, 99.f);  CHECK(e.get(0)==doctest::Approx(12.f));
    e.set(2, -99.f); CHECK(e.get(2)==doctest::Approx(-6.f));
    CHECK(std::string(e.name())=="ENHANCER");
    float v; const char* l; CHECK(e.meter(v,l)==false);
}

TEST_CASE("AGC: hlasity vstup je utlumen k target RMS") {
    dsp::AGC agc; agc.prepare(48000.f, 4800);
    agc.setEnabled(true);
    agc.set(0, 0.15f);    // TARGET RMS
    agc.set(1, 50.f);     // RELEASE ms (rychlejsi ustaleni v testu)
    float L[4800], R[4800];
    for(int i=0;i<4800;++i){ L[i]=0.5f; R[i]=0.5f; }   // RMS 0.5 >> target
    agc.process(L,R,4800);
    CHECK(std::abs(L[4799]) < 0.5f);                   // utlumeno
    CHECK(std::abs(L[4799]) == doctest::Approx(0.15f).epsilon(0.25)); // ~ target
}

TEST_CASE("AGC: tichy vstup neni zesilen (gain <= 1)") {
    dsp::AGC agc; agc.prepare(48000.f, 1024);
    agc.setEnabled(true);
    agc.set(0, 0.15f);
    float L[1024], R[1024];
    for(int i=0;i<1024;++i){ L[i]=0.02f; R[i]=0.02f; } // RMS 0.02 < target
    agc.process(L,R,1024);
    CHECK(std::abs(L[1023]) <= 0.02f + 1e-4f);         // nikdy nezesili
}

TEST_CASE("AGC: gain neklesne pod floor") {
    dsp::AGC agc; agc.prepare(48000.f, 4800);
    agc.setEnabled(true);
    agc.set(0, 0.15f);
    agc.set(2, 0.1f);     // GAIN FLOOR 0.1
    agc.set(1, 20.f);
    float L[4800], R[4800];
    for(int i=0;i<4800;++i){ L[i]=5.f; R[i]=5.f; }     // target_gain ~0.03 < floor
    agc.process(L,R,4800);
    CHECK(std::abs(L[4799]) >= 5.f * 0.1f * 0.9f);     // gain >= floor
    float g; const char* l; CHECK(agc.meter(g,l)==true); CHECK(g <= 1.f);
}

TEST_CASE("AGC: disabled bypass + param round-trip") {
    dsp::AGC agc; agc.prepare(48000.f, 64);
    agc.setEnabled(false);
    float L[2]={0.9f,-0.9f}, R[2]={0.9f,-0.9f};
    agc.process(L,R,2);
    CHECK(L[0]==0.9f); CHECK(L[1]==-0.9f);
    CHECK(agc.paramCount()==3);
    agc.set(0, 99.f); CHECK(agc.get(0)==doctest::Approx(0.5f));   // TARGET max 0.5
    CHECK(std::string(agc.name())=="AGC");
}

TEST_CASE("DspChain: vsechny stage disabled = identita") {
    dsp::DspChain ch; ch.prepare(48000.f, 256);
    CHECK(ch.stageCount()==4);
    float L[4]={0.3f,-0.4f,0.5f,-0.6f}, R[4]={0.1f,0.2f,-0.2f,0.4f};
    float L0[4]; for(int i=0;i<4;++i) L0[i]=L[i];
    ch.process(L,R,4);
    for(int i=0;i<4;++i) CHECK(L[i]==L0[i]);
}

TEST_CASE("DspChain: poradi stage je CONVOLVER, AGC, ENHANCER, LIMITER") {
    dsp::DspChain ch; ch.prepare(48000.f, 256);
    CHECK(ch.stageCount() == 4);
    CHECK(std::string(ch.stage(0).name())=="CONVOLVER");
    CHECK(std::string(ch.stage(1).name())=="AGC");
    CHECK(std::string(ch.stage(2).name())=="ENHANCER");
    CHECK(std::string(ch.stage(3).name())=="LIMITER");
}

TEST_CASE("DspChain: enabled limiter omezi spicku") {
    dsp::DspChain ch; ch.prepare(48000.f, 4800);
    ch.stage(3).setEnabled(true);       // LIMITER
    ch.stage(3).set(0, -12.f);          // threshold
    ch.stage(3).set(1, 50.f);
    float L[4800], R[4800];
    for(int i=0;i<4800;++i){ L[i]=0.9f; R[i]=0.9f; }
    ch.process(L,R,4800);
    CHECK(std::abs(L[4799]) <= dsp::db_to_lin(-12.f) * 1.05f);
}

TEST_CASE("Engine vystavuje DSP chain se 3 stage") {
    ithaca::EngineConfig cfg;          // default 48k / 256, zadna banka
    ithaca::Engine eng;
    REQUIRE(eng.init(cfg));
    auto& ch = eng.dspChain();
    CHECK(ch.stageCount() == 4);
    CHECK(std::string(ch.stage(3).name()) == "LIMITER");
    ch.stage(3).setEnabled(true);
    CHECK(ch.stage(3).enabled());
}

TEST_CASE("Engine ma oddelene main + resonance stream pooly") {
    ithaca::EngineConfig cfg;
    ithaca::Engine eng;
    REQUIRE(eng.init(cfg));
    CHECK(eng.mainRingsTotal()      == cfg.num_rings);
    CHECK(eng.resonanceRingsTotal() == cfg.resonance_num_rings);
    CHECK(eng.mainRingsTotal() != eng.resonanceRingsTotal());   // dva ruzne pooly
    CHECK(eng.mainStreamUnderrunRecent(1000.f) == false);
    CHECK(eng.resonanceStreamUnderrunRecent(1000.f) == false);
}

TEST_CASE("dsp_math: lowpass/highpass DC + smoothstep") {
    using namespace ithaca::dsp;
    auto lp = rbj_lowpass(1000.f, 0.707f, 48000.f);
    auto hp = rbj_highpass(1000.f, 0.707f, 48000.f);
    BiquadState sl{}, sh{};
    float yl=0, yh=0;
    for (int i=0;i<4096;++i){ yl=biquad_tick(1.f,lp,sl); yh=biquad_tick(1.f,hp,sh); }
    CHECK(yl == doctest::Approx(1.f).epsilon(0.02));   // lowpass passes DC
    CHECK(std::fabs(yh) < 0.02f);                       // highpass blocks DC
    CHECK(smoothstep(0.0f,0.2f,0.8f) == doctest::Approx(0.f));
    CHECK(smoothstep(1.0f,0.2f,0.8f) == doctest::Approx(1.f));
    CHECK(smoothstep(0.5f,0.2f,0.8f) > 0.f);
    CHECK(smoothstep(0.5f,0.2f,0.8f) < 1.f);
}
