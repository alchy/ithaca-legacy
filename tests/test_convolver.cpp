#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "dsp/convolver.h"
#include <cstdint>
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
TEST_CASE("Convolver: paramCount 4 (MIX/DECAY/TONE/SIZE) + defaults") {
    Convolver c; c.prepare(48000.f, 256);
    CHECK(c.paramCount() == 4);
    CHECK(c.get(0) == doctest::Approx(0.15f));   // MIX default
    CHECK(c.get(1) == doctest::Approx(0.50f));   // DECAY
    CHECK(c.get(2) == doctest::Approx(0.60f));   // TONE
    CHECK(c.get(3) == doctest::Approx(0.50f));   // SIZE
    CHECK(std::string(c.param(1).label) == "DECAY");
    CHECK(std::string(c.param(2).label) == "TONE");
    CHECK(std::string(c.param(3).label) == "SIZE");
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
TEST_CASE("Convolver: SIZE posouva telo (vetsi → nizsi spektralni teziste)") {
    CHECK([]{ Convolver c; c.prepare(48000.f,8192); return c.paramCount(); }() == 4);
    auto centroid = [](Convolver& c){
        std::vector<float> L(8192,0.f), R(8192,0.f); L[0]=1.f; R[0]=1.f;
        c.process(L.data(), R.data(), 8192);
        double num=0, den=0;
        for (float f=100.f; f<=8000.f; f+=100.f){ double re=0,im=0,w=2.0*M_PI*f/48000.0;
            for(int i=0;i<8192;++i){re+=L[(size_t)i]*std::cos(w*i);im+=L[(size_t)i]*std::sin(w*i);}
            double mag=std::sqrt(re*re+im*im); num+=f*mag; den+=mag; }
        return den>0? num/den : 0.0; };
    Convolver big;   big.prepare(48000.f,8192);   big.setEnabled(true);   big.set(0,1.f);   big.set(3,1.0f);  // velke telo
    Convolver small; small.prepare(48000.f,8192); small.setEnabled(true); small.set(0,1.f); small.set(3,0.0f); // male telo
    CHECK(centroid(big) < centroid(small));   // vetsi telo → nizsi teziste
}
TEST_CASE("Convolver: split (head+tail) ekvivalence s referencni FIR konvoluci") {
    // Hot loop je rozdelen na dva flat passy bez wraparound branche
    // (head: rp od write_pos dolu k 0; tail: rp od kMaxIr-1 dolu).
    // Tento test overuje, ze nove rozlozeni produkuje stejny vystup jako
    // primy ne-ring FIR y[i] = sum_k ir[k]*x[i-k] s nulami vlevo.
    // PRIME = kMaxIr + 100 -> write_pos pristane v "head" oblasti
    // a M=256 presahne do "tail" oblasti -> oba passy aktivni.
    constexpr int M_test = 256;
    constexpr int PRIME = 8192 + 100;
    constexpr int TEST  = 64;
    constexpr int TOTAL = PRIME + TEST;

    auto rng = [](uint32_t& s) {
        s = s * 1664525u + 1013904223u;
        return ((float)(s >> 8) / (float)0xFFFFFF) * 2.f - 1.f;   // U(-1, 1)
    };

    // Vstup
    std::vector<float> in_l(TOTAL), in_r(TOTAL);
    { uint32_t s = 1234567;
      for (int i = 0; i < TOTAL; ++i) { in_l[(size_t)i] = rng(s); in_r[(size_t)i] = rng(s); } }

    // IR uz peak-normalizovana (setIR ji znova nenormalizuje, peak == 1).
    std::vector<float> ir(M_test);
    { uint32_t s = 42;
      for (int i = 0; i < M_test; ++i) ir[(size_t)i] = rng(s) * std::exp(-3.f * (float)i / (float)M_test);
      float peak = 0.f; for (float v : ir) peak = std::max(peak, std::fabs(v));
      REQUIRE(peak > 0.f);
      for (auto& v : ir) v /= peak; }

    Convolver c;
    c.prepare(48000.f, 256);
    c.setEnabled(true);
    c.set(0, 1.f);    // MIX=1 -> pure wet (dry=0)
    c.setIR(ir);

    std::vector<float> L = in_l, R = in_r;
    c.process(L.data(), R.data(), TOTAL);

    // Referencni primy FIR (zero-padded vlevo, matches initial ring of nuly).
    auto ref = [&](const std::vector<float>& x) {
        std::vector<float> y((size_t)TOTAL, 0.f);
        for (int i = 0; i < TOTAL; ++i) {
            const int kmax = (i < M_test - 1) ? i + 1 : M_test;
            float acc = 0.f;
            for (int k = 0; k < kmax; ++k) acc += ir[(size_t)k] * x[(size_t)(i - k)];
            y[(size_t)i] = acc;
        }
        return y;
    };
    const auto rL = ref(in_l);
    const auto rR = ref(in_r);

    // Test posledni TEST vzorky: write_pos prozarazen po PRIME % kMaxIr = 100,
    // takze pro vsechny tyto i je headN < M_test -> tail pass aktivni.
    // Tolerance: kompilator muze prehazet poradi akumulace (lane-parallel
    // reduction po vektorizaci) -> max chyba ~ M*eps*max(|x|) ~ 256*1e-7 ~ 3e-5
    // skalovane velikosti konvolvovaneho signalu (mensi nez |ir|_1 ~ 30).
    double maxErr = 0.0;
    for (int i = PRIME; i < TOTAL; ++i) {
        maxErr = std::max(maxErr, (double)std::fabs(L[(size_t)i] - rL[(size_t)i]));
        maxErr = std::max(maxErr, (double)std::fabs(R[(size_t)i] - rR[(size_t)i]));
    }
    INFO("max abs error proti referencni FIR konvoluci = " << maxErr);
    CHECK(maxErr < 1e-3);
}

TEST_CASE("trimmedIrLength: energeticka zaruka + hranice") {
    // Silne doznivajici IR → orez zkrati delku, ale zahodi <= drop_frac energie.
    const int n = 2000;
    std::vector<float> ir((size_t)n);
    for (int i = 0; i < n; ++i) ir[(size_t)i] = std::exp(-(float)i / 50.f);  // tau=50

    const double drop = 1e-6;
    int L = trimmedIrLength(ir, drop);
    CHECK(L < n);            // neco se orezalo
    CHECK(L >= 1);

    double total = 0.0, dropped = 0.0;
    for (int i = 0; i < n; ++i) total   += (double)ir[(size_t)i] * ir[(size_t)i];
    for (int i = L; i < n; ++i) dropped += (double)ir[(size_t)i] * ir[(size_t)i];
    INFO("L=" << L << " dropped/total=" << (dropped/total));
    CHECK(dropped <= drop * total);     // ZARUKA: zahozena energie pod prahem
}

TEST_CASE("trimmedIrLength: konstantni IR se neorezava; ticho/hranice") {
    std::vector<float> flat(1000, 1.0f);
    CHECK(trimmedIrLength(flat, 1e-6) == 1000);   // zadny doznivajici ocas → bez orezu

    std::vector<float> zero(500, 0.0f);
    CHECK(trimmedIrLength(zero, 1e-6) == 1);       // ticho → 1

    std::vector<float> one(1, 0.5f);
    CHECK(trimmedIrLength(one, 1e-6) == 1);
}

TEST_CASE("Convolver: rebuildIr orezava runtime IR (nizsi DECAY = kratsi)") {
    Convolver c;
    c.prepare(48000.f, 256);          // postavi modal IR pres rebuildIr (orezany)
    const int full = c.irLength();
    CHECK(full > 0);
    CHECK(full <= Convolver::kMaxIr);
    c.set(1, 0.05f);                  // velmi nizky DECAY → kratky doznivajici ocas
    const int shortL = c.irLength();
    INFO("IR delka: default=" << full << " nizky DECAY=" << shortL);
    CHECK(shortL < full);            // kratsi IR = levnejsi konvoluce
    CHECK(shortL >= 1);
}
