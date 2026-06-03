#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "dsp/convolver.h"
#include <string>
#include <vector>
#include <cmath>
using namespace ithaca::dsp;

TEST_CASE("Convolver: disabled = bypass") {
    Convolver c; c.prepare(48000.f, 256); c.setEnabled(false);
    float L[3]={0.1f,0.2f,0.3f}, R[3]={0.1f,0.2f,0.3f};
    c.process(L,R,3);
    CHECK(L[0]==doctest::Approx(0.1f)); CHECK(L[2]==doctest::Approx(0.3f));
}
TEST_CASE("Convolver: identity IR + MIX=1 → passthrough") {
    Convolver c; c.prepare(48000.f, 256); c.setEnabled(true); c.set(0, 1.f);
    c.setIR(std::vector<float>{1.f});
    float L[4]={0.5f,-0.3f,0.2f,0.1f}, R[4]={0.5f,-0.3f,0.2f,0.1f};
    c.process(L,R,4);
    CHECK(L[0]==doctest::Approx(0.5f)); CHECK(L[1]==doctest::Approx(-0.3f));
}
TEST_CASE("Convolver: impulz → IR (MIX=1)") {
    Convolver c; c.prepare(48000.f, 256); c.setEnabled(true); c.set(0, 1.f);
    c.setIR(std::vector<float>{0.5f, 0.25f, -0.5f});   // peak-normalizovano → {1, 0.5, -1}
    float L[4]={1.f,0.f,0.f,0.f}, R[4]={1.f,0.f,0.f,0.f};
    c.process(L,R,4);
    CHECK(L[0]==doctest::Approx(1.0f));
    CHECK(L[1]==doctest::Approx(0.5f));
    CHECK(L[2]==doctest::Approx(-1.0f));
    CHECK(L[3]==doctest::Approx(0.f));
}
TEST_CASE("Convolver: MIX=0 dry; choice list (modal presety)") {
    Convolver c; c.prepare(48000.f, 256); c.setEnabled(true); c.set(0, 0.f);
    float L[2]={0.7f,0.4f}, R[2]={0.7f,0.4f}; c.process(L,R,2);
    CHECK(L[0]==doctest::Approx(0.7f));
    CHECK(c.choiceCount() >= 2);
    CHECK(std::string(c.choiceLabel())=="IR");
    CHECK_NOTHROW(c.selectChoice(1));
    CHECK(c.irLength() > 0);
}
TEST_CASE("Convolver: paramCount 3 (MIX/DECAY/TONE) + defaults") {
    Convolver c; c.prepare(48000.f, 256);
    CHECK(c.paramCount() == 3);
    CHECK(c.get(0) == doctest::Approx(0.15f));   // MIX default
    CHECK(c.get(1) == doctest::Approx(0.50f));   // DECAY
    CHECK(c.get(2) == doctest::Approx(0.60f));   // TONE
    CHECK(std::string(c.param(1).label) == "DECAY");
    CHECK(std::string(c.param(2).label) == "TONE");
}
TEST_CASE("Convolver: nizsi DECAY zkrati IR energii (pozdni cast)") {
    auto tailEnergy = [](Convolver& c){
        // odezva na impulz, MIX=1 → IR; spocti energii pozdni casti (za 30 ms)
        std::vector<float> L(8192,0.f), R(8192,0.f); L[0]=1.f; R[0]=1.f;
        c.process(L.data(), R.data(), 8192);
        double e=0; for (int i=1440;i<8192;++i) e+=(double)L[i]*L[i]; return e; };
    Convolver hi; hi.prepare(48000.f, 8192); hi.setEnabled(true); hi.set(0,1.f); hi.set(1,0.9f);
    Convolver lo; lo.prepare(48000.f, 8192); lo.setEnabled(true); lo.set(0,1.f); lo.set(1,0.1f);
    CHECK(tailEnergy(lo) < tailEnergy(hi));   // kratsi decay → mensi pozdni energie
}
TEST_CASE("Convolver: nizsi TONE = tmavsi IR (mensi HF energie)") {
    auto hfE = [](Convolver& c){
        std::vector<float> L(8192,0.f), R(8192,0.f); L[0]=1.f; R[0]=1.f;
        c.process(L.data(), R.data(), 8192);
        double e=0; for (float f=6000.f;f<=10000.f;f+=200.f){ double re=0,im=0,w=2.0*M_PI*f/48000.0;
            for(int i=0;i<8192;++i){re+=L[(size_t)i]*std::cos(w*i);im+=L[(size_t)i]*std::sin(w*i);} e+=re*re+im*im;} return e; };
    Convolver br; br.prepare(48000.f, 8192); br.setEnabled(true); br.set(0,1.f); br.set(2,1.0f);  // bright
    Convolver dk; dk.prepare(48000.f, 8192); dk.setEnabled(true); dk.set(0,1.f); dk.set(2,0.0f);  // dark
    CHECK(hfE(dk) < hfE(br));
}
