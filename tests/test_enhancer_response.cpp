#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "dsp/enhancer.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace ithaca::dsp;
static const float kPi = 3.14159265358979323846f;

// Ustaleny zisk [dB] sinu freq pres Enhancer pri dane amplitude.
static float gainDb(Enhancer& e, float freq, float amp) {
    const float sr = 48000.f; const int N = 24000;
    std::vector<float> L(N), R(N);
    for (int i=0;i<N;++i){ float s=amp*std::sin(2.f*kPi*freq*(float)i/sr); L[i]=s; R[i]=s; }
    for (int off=0; off<N; off+=512){ int n=std::min(512,N-off); e.process(L.data()+off,R.data()+off,n); }
    double in_sq=0,out_sq=0; int c=0;
    for (int i=N/2;i<N;++i){ float s=amp*std::sin(2.f*kPi*freq*(float)i/sr); in_sq+=(double)s*s; out_sq+=(double)L[i]*L[i]; ++c; }
    return 20.f*std::log10(std::sqrt(out_sq/c)/std::sqrt(in_sq/c));
}

TEST_CASE("Enhancer response: bypass / contour / dynamic process / exciter") {
    // bypass = identita
    { Enhancer e; e.prepare(48000.f,512); e.setEnabled(false); e.set(0,12.f);
      CHECK(std::fabs(gainDb(e,5000.f,0.5f)) < 0.5f); }

    // FLAT-AT-UNITY: enabled, vsechny parametry 0 → magnitudove ploche (all-pass
    // nemeni magnitudu). Strazi regresi comb-notche ve stredech.
    { Enhancer e; e.prepare(48000.f,512); e.setEnabled(true);  // process=contour=mid=0
      for (float f : {120.f, 500.f, 1000.f, 2000.f, 5000.f}) {
          e.reset(); float g = gainDb(e, f, 0.3f);
          MESSAGE("unity @"<<f<<"Hz = "<<g<<" dB");
          CHECK(std::fabs(g) < 1.5f);   // ploche ±1.5 dB (zadny notch/peak)
      } }

    // CONTOUR: bass roste, ~NEzávislé na úrovni
    { Enhancer e; e.prepare(48000.f,512); e.setEnabled(true); e.set(1,12.f);
      float lo = gainDb(e,100.f,0.02f); e.reset();
      float hi = gainDb(e,100.f,0.5f);
      MESSAGE("CONTOUR 100Hz low-lvl="<<lo<<" high-lvl="<<hi);
      CHECK(hi > 2.f);
      CHECK(std::fabs(hi-lo) < 2.f); }

    // PROCESS: HF roste a ZÁVISÍ na úrovni (boost-when-loud)
    { Enhancer e; e.prepare(48000.f,512); e.setEnabled(true); e.set(0,12.f);
      float lo = gainDb(e,6000.f,0.02f); e.reset();
      float hi = gainDb(e,6000.f,0.5f);
      MESSAGE("PROCESS 6kHz low-lvl="<<lo<<" high-lvl="<<hi);
      CHECK(hi > lo + 3.f); }

    // EXCITER: čistý 3 kHz sinus → output má NOVOU 2. harmonickou (6 kHz)
    auto bin = [](const std::vector<float>& y, float f, float sr){
        double re=0,im=0; int N=(int)y.size();
        for (int i=N/2;i<N;++i){ double w=2.0*kPi*f*i/sr; re+=y[(size_t)i]*std::cos(w); im+=y[(size_t)i]*std::sin(w); }
        return std::sqrt(re*re+im*im)/(N/2); };
    auto run3k = [&](float proc, float amp){
        Enhancer e; e.prepare(48000.f,512); e.setEnabled(true); e.set(0,proc);
        const int N=24000; std::vector<float> L(N),R(N);
        for(int i=0;i<N;++i){ float s=amp*std::sin(2.f*kPi*3000.f*(float)i/48000.f); L[i]=s;R[i]=s; }
        for(int o=0;o<N;o+=512){int n=std::min(512,N-o); e.process(L.data()+o,R.data()+o,n);}
        return bin(L, 6000.f, 48000.f); };
    {
        double h2_on  = run3k(12.f, 0.5f);
        double h2_off = run3k(0.f,  0.5f);
        MESSAGE("2nd harmonic @6kHz: process12="<<h2_on<<" process0="<<h2_off);
        CHECK(h2_on > h2_off * 2.0);
    }

    // CSV pro overlay / inspekci (dvě úrovně)
    { Enhancer e; e.prepare(48000.f,512); e.setEnabled(true); e.set(0,12.f); e.set(1,12.f);
      std::printf("freq,gain_db_low,gain_db_high\n");
      const float freqs[]={40,60,100,150,250,500,1000,2000,3000,4000,6000,8000,12000,16000};
      for (float f: freqs){ e.reset(); float gl=gainDb(e,f,0.02f); e.reset(); float gh=gainDb(e,f,0.5f);
        std::printf("%.0f,%.2f,%.2f\n", f, gl, gh); } }
}
