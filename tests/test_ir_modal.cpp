#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "dsp/ir_modal.h"
#include <cmath>
#include <vector>
using namespace ithaca::dsp;

TEST_CASE("damping law f_ve(f) = 2e-5 f^2 + 7e-2 f (RR_9530)") {
    CHECK(soundboardDampingFve(100.f) == doctest::Approx(2e-5f*10000.f + 7.f));   // 7.2
    CHECK(soundboardDampingFve(1000.f) == doctest::Approx(2e-5f*1e6f + 70.f));    // 90
    float tau100 = 2.f/soundboardDampingFve(100.f);
    float tau1k  = 2.f/soundboardDampingFve(1000.f);
    CHECK(tau100 > tau1k);
    CHECK(tau100 > 0.2f);
    CHECK(tau1k  < 0.05f);
}
TEST_CASE("generateModalIr: konecna, normalizovana, energie < 1 kHz > 3-6 kHz") {
    auto ir = generateModalIr(IrPreset::BodySoft, 48000.f, 8192);
    REQUIRE(ir.size() > 100);
    CHECK((int)ir.size() <= 8192);
    float peak=0.f; for (float v:ir){ CHECK(std::isfinite(v)); peak=std::max(peak,std::fabs(v)); }
    CHECK(peak == doctest::Approx(1.f).epsilon(0.01));
    auto bandE = [&](float f0, float f1){ double e=0; int N=(int)ir.size();
        for (float f=f0; f<=f1; f+=50.f){ double re=0,im=0,w=2.0*M_PI*f/48000.0;
            for (int i=0;i<N;++i){ re+=ir[(size_t)i]*std::cos(w*i); im+=ir[(size_t)i]*std::sin(w*i);}
            e += re*re+im*im; } return e; };
    CHECK(bandE(100.f,800.f) > bandE(3000.f,6000.f));
}
TEST_CASE("BodyBright má víc HF energie než BodySoft") {
    auto soft   = generateModalIr(IrPreset::BodySoft,   48000.f, 8192);
    auto bright = generateModalIr(IrPreset::BodyBright, 48000.f, 8192);
    auto hf=[&](const std::vector<float>& ir){ double e=0;int N=(int)ir.size();
        for(float f=2000.f;f<=4000.f;f+=50.f){double re=0,im=0,w=2.0*M_PI*f/48000.0;
            for(int i=0;i<N;++i){re+=ir[(size_t)i]*std::cos(w*i);im+=ir[(size_t)i]*std::sin(w*i);}
            e+=re*re+im*im;} return e; };
    CHECK(hf(bright) > hf(soft));
}
