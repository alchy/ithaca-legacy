// tests/test_dsp.cpp — DSP math + stage + chain unit testy (doctest).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/dsp_math.h"
#include "dsp/limiter.h"
#include "dsp/bbe.h"
#include "dsp/agc.h"
#include <cmath>
#include <string>

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

TEST_CASE("BBE: disabled = bit-identicky bypass") {
    dsp::BBE bbe; bbe.prepare(48000.f, 256);
    bbe.setEnabled(false);
    bbe.set(0, 12.f); bbe.set(1, 10.f);
    float L[3]={0.4f,-0.5f,0.6f}, R[3]={0.1f,0.2f,-0.3f};
    float L0[3]; for(int i=0;i<3;++i) L0[i]=L[i];
    bbe.process(L,R,3);
    for(int i=0;i<3;++i) CHECK(L[i]==L0[i]);
}

TEST_CASE("BBE: oba parametry 0 dB = temer pruhledny na DC") {
    dsp::BBE bbe; bbe.prepare(48000.f, 256);
    bbe.setEnabled(true);                 // definition=0, bass=0 (default)
    float L[256], R[256];
    for(int i=0;i<256;++i){ L[i]=0.5f; R[i]=0.5f; }
    bbe.process(L,R,256);
    CHECK(L[255]==doctest::Approx(0.5f).epsilon(0.03));
}

TEST_CASE("BBE: bass boost zmeni DC uroven") {
    dsp::BBE bbe; bbe.prepare(48000.f, 256);
    bbe.setEnabled(true);
    bbe.set(1, 10.f);                     // BASS +10 dB (low shelf -> boost DC)
    float L[2048], R[2048];
    for(int i=0;i<2048;++i){ L[i]=0.2f; R[i]=0.2f; }
    bbe.process(L,R,2048);
    CHECK(std::abs(L[2047]) > 0.2f * 1.5f);   // DC zesilen low-shelfem
}

TEST_CASE("BBE: param round-trip + clamp") {
    dsp::BBE bbe; bbe.prepare(48000.f, 256);
    CHECK(bbe.paramCount()==2);
    bbe.set(0, 99.f);  CHECK(bbe.get(0)==doctest::Approx(12.f));
    bbe.set(1, -5.f);  CHECK(bbe.get(1)==doctest::Approx(0.f));
    CHECK(std::string(bbe.name())=="BBE");
    float v; const char* l; CHECK(bbe.meter(v,l)==false);
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
