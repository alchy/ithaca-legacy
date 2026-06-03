# Dynamic Enhancer (ex-BBE) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Nahradit statické 2-shelf BBE hybridním 3-pásmovým Enhancerem (PROCESS/CONTOUR/MID) s dynamikou boost-when-loud na PROCESS pásmu a group-delay zarovnáním; přejmenovat BBE → Enhancer všude.

**Architecture:** `Enhancer : DspStage` — 3-pásmový split (LP250 / BP250-3k / HP3k+cap11k), per-pásmové zpoždění (2,5/0,5/0 ms) pro group-delay, per-pásmový zisk (HIGH dynamicky škálovaný broadband peak-monitorem). Validační harness měří magnitudu + group delay proti Naganovovi.

**Tech Stack:** C++20, doctest/CTest, biquady (`dsp_math.h`).

**Spec:** `docs/superpowers/specs/2026-06-03-bbe-dynamic-design.md`

**Build pořadí:** T1–T2 v `ithaca_core` (+ `test_dsp`); GUI (persistence/app_context/main referují `bbe_*`) se opraví v T4 → do té doby `ithaca-gui` nebuildí (build jen uvedené targety). Plný build T6.

---

### Task 1: dsp_math helpery (LP/HP biquad + smoothstep)

**Files:** Modify `engine/dsp/dsp_math.h`; Test `tests/test_dsp.cpp` (přidat TEST_CASE)

- [ ] **Step 1: Failing test** — do `tests/test_dsp.cpp` přidej (na konec, před závěr):
```cpp
TEST_CASE("dsp_math: lowpass/highpass DC + smoothstep") {
    using namespace ithaca::dsp;
    auto lp = rbj_lowpass(1000.f, 0.707f, 48000.f);
    auto hp = rbj_highpass(1000.f, 0.707f, 48000.f);
    BiquadState sl{}, sh{};
    // DC: lowpass propustí ~1, highpass ~0 (po ustálení)
    float yl=0, yh=0;
    for (int i=0;i<4096;++i){ yl=biquad_tick(1.f,lp,sl); yh=biquad_tick(1.f,hp,sh); }
    CHECK(yl == doctest::Approx(1.f).epsilon(0.02));
    CHECK(std::fabs(yh) < 0.02f);
    CHECK(smoothstep(0.0f,0.2f,0.8f) == doctest::Approx(0.f));
    CHECK(smoothstep(1.0f,0.2f,0.8f) == doctest::Approx(1.f));
    CHECK(smoothstep(0.5f,0.2f,0.8f) > 0.f);
    CHECK(smoothstep(0.5f,0.2f,0.8f) < 1.f);
}
```

- [ ] **Step 2: Build → FAIL** (helpery neexistují):
```
cmake --build build --target test_dsp 2>&1 | tail -12
```
Expected: compile error `rbj_lowpass`/`rbj_highpass`/`smoothstep` undefined.

- [ ] **Step 3: Implementace** — do `engine/dsp/dsp_math.h` přidej (za `rbj_low_shelf`, před `gain_envelope_smooth`):
```cpp
// RBJ low-pass (Audio EQ Cookbook). fc [Hz], q (0.707 = Butterworth), sr.
inline BiquadCoeffs rbj_lowpass(float fc, float q, float sr) {
    float w0 = TAU * fc / sr, cosw = std::cos(w0), sinw = std::sin(w0);
    float al = sinw / (2.f * q);
    float a0 = 1.f + al, ia = 1.f / a0;
    BiquadCoeffs c;
    c.b0 = ((1.f - cosw) * 0.5f) * ia;
    c.b1 = ( 1.f - cosw)         * ia;
    c.b2 = ((1.f - cosw) * 0.5f) * ia;
    c.a1 = (-2.f * cosw)         * ia;
    c.a2 = ( 1.f - al)           * ia;
    return c;
}
// RBJ high-pass.
inline BiquadCoeffs rbj_highpass(float fc, float q, float sr) {
    float w0 = TAU * fc / sr, cosw = std::cos(w0), sinw = std::sin(w0);
    float al = sinw / (2.f * q);
    float a0 = 1.f + al, ia = 1.f / a0;
    BiquadCoeffs c;
    c.b0 = ((1.f + cosw) * 0.5f) * ia;
    c.b1 = (-(1.f + cosw))       * ia;
    c.b2 = ((1.f + cosw) * 0.5f) * ia;
    c.a1 = (-2.f * cosw)         * ia;
    c.a2 = ( 1.f - al)           * ia;
    return c;
}
// Hermite smoothstep: 0 pod edge0, 1 nad edge1, hladce mezi.
inline float smoothstep(float x, float edge0, float edge1) {
    if (edge1 <= edge0) return x < edge0 ? 0.f : 1.f;
    float t = (x - edge0) / (edge1 - edge0);
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    return t * t * (3.f - 2.f * t);
}
```

- [ ] **Step 4: Build + test → PASS:**
```
cmake --build build --target test_dsp 2>&1 | tail -4 && ctest --test-dir build -R "^test_dsp$" --output-on-failure 2>&1 | tail -4
```

- [ ] **Step 5: Commit:**
```bash
git add engine/dsp/dsp_math.h tests/test_dsp.cpp
git commit -m "feat(dsp): rbj_lowpass/highpass + smoothstep helpers"
```

---

### Task 2: Enhancer (rename BBE + hybridní 3-pásmová dynamická implementace)

**Files:**
- Create: `engine/dsp/enhancer.h`, `engine/dsp/enhancer.cpp`
- Delete: `engine/dsp/bbe.h`, `engine/dsp/bbe.cpp`
- Modify: `engine/dsp/dsp_chain.h`, `engine/engine.cpp:248` (komentář), `CMakeLists.txt:44`, `tests/test_dsp.cpp` (BBE testy → Enhancer)

**Build target:** `ithaca_core` + `test_dsp`.

- [ ] **Step 1: `engine/dsp/enhancer.h`:**
```cpp
#pragma once
// engine/dsp/enhancer.h — Enhancer (BBE-style Sonic Maximizer) jako DspStage.
// Hybrid: 3-pasmovy split (LOW/MID/HIGH) + per-pasmove group-delay zarovnani
// + dynamicky boost-when-loud na HIGH (PROCESS). Viz spec 2026-06-03.
#include "dsp/dsp_stage.h"
#include "dsp/dsp_math.h"
#include <atomic>
#include <vector>

namespace ithaca::dsp {

class Enhancer : public DspStage {
public:
    const char* name() const override { return "ENHANCER"; }
    void prepare(float sr, int max_block) override;
    void reset() override;

    int paramCount() const override { return 3; }
    const Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override { return p_[(size_t)i].load(std::memory_order_relaxed); }
    void set(int i, float v) override {
        v = (v < kParams[i].min) ? kParams[i].min : (v > kParams[i].max ? kParams[i].max : v);
        p_[(size_t)i].store(v, std::memory_order_relaxed);
    }
    bool hasEnable() const override { return enabled_.load(std::memory_order_relaxed); }  // viz pozn.
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool on) override { enabled_.store(on, std::memory_order_relaxed); }
    bool meter(float&, const char*&) const override { return false; }

    void process(float* L, float* R, int n) override;

private:
    static const Param kParams[3];   // 0=PROCESS(HIGH), 1=CONTOUR(LOW), 2=MID
    std::atomic<float> p_[3]{};      // dB
    std::atomic<bool>  enabled_{false};
    float sr_ = 48000.f;

    // Crossover koeficienty (sdilene pres kanaly): LR2 (Q=0.707). hp_exc_ = exciter HP.
    BiquadCoeffs lp_lo_{}, hp_lo_{}, lp_mid_{}, hp_hi_{}, lp_cap_{}, hp_exc_{};

    struct Chan {
        BiquadState s_lp_lo, s_hp_lo, s_lp_mid, s_hp_hi, s_lp_cap, s_hp_exc;
        std::vector<float> dl_low, dl_mid;   // delay linky (sized v prepare)
        int wl = 0, wm = 0;
    } ch_[2];
    int   delay_low_n_ = 0, delay_mid_n_ = 0;

    // Broadband peak level monitor (linkovany).
    float env_ = 0.f, atk_ = 0.f, rel_ = 0.f;

    void computeCoeffs_();
    static float pushDelay_(std::vector<float>& dl, int& w, int n, float x) {
        if (n <= 0) return x;
        float y = dl[(size_t)w];
        dl[(size_t)w] = x;
        w = (w + 1) % n;
        return y;
    }
};

} // namespace ithaca::dsp
```
> **Pozn. `hasEnable`:** musí vracet `true` (stránka má ON/OFF). Oprav výše uvedený překlep — `bool hasEnable() const override { return true; }`.

- [ ] **Step 2: `engine/dsp/enhancer.cpp`:**
```cpp
#include "dsp/enhancer.h"
#include <algorithm>
#include <cmath>

namespace ithaca::dsp {

// Konstanty (laditelne; rohy dle schematu/mereni BBE).
namespace {
constexpr float kXoverLow  = 250.f;    // low/mid
constexpr float kXoverHigh = 3000.f;   // mid/high
constexpr float kHfCap     = 11000.f;  // horni mez HIGH (rolloff po ~10k)
constexpr float kQ         = 0.707f;
constexpr float kDelayLowMs = 2.5f;
constexpr float kDelayMidMs = 0.5f;
constexpr float kAtkMs = 5.f, kRelMs = 80.f;
constexpr float kScaleLo = 0.05f, kScaleHi = 0.35f;   // env→scale prahy (lin)
constexpr float kExcHp  = 4500.f;   // exciter high-pass (jen generované výšky)
constexpr float kExcite = 0.35f;    // max příměs exciteru (jemné)
constexpr float kSat    = 0.5f;     // síla asymetrické saturace (2. harmonická)
inline float db2lin(float db) { return std::pow(10.f, db / 20.f); }
}

const Param Enhancer::kParams[3] = {
    {"process", "PROCESS", 0.f, 12.f,  0.f, "%.1f dB", false},   // HIGH (dynamicky)
    {"contour", "CONTOUR", 0.f, 12.f,  0.f, "%.1f dB", false},   // LOW
    {"mid",     "MID",    -6.f,  6.f,  0.f, "%.1f dB", false},   // MID
};

void Enhancer::computeCoeffs_() {
    lp_lo_  = rbj_lowpass (kXoverLow,  kQ, sr_);
    hp_lo_  = rbj_highpass(kXoverLow,  kQ, sr_);
    lp_mid_ = rbj_lowpass (kXoverHigh, kQ, sr_);
    hp_hi_  = rbj_highpass(kXoverHigh, kQ, sr_);
    lp_cap_ = rbj_lowpass (kHfCap,     kQ, sr_);
    hp_exc_ = rbj_highpass(kExcHp,     kQ, sr_);
}

void Enhancer::prepare(float sr, int /*max_block*/) {
    sr_ = sr;
    computeCoeffs_();
    delay_low_n_ = (int)(kDelayLowMs * 0.001f * sr_ + 0.5f);
    delay_mid_n_ = (int)(kDelayMidMs * 0.001f * sr_ + 0.5f);
    for (auto& c : ch_) {
        c.dl_low.assign((size_t)std::max(1, delay_low_n_), 0.f);
        c.dl_mid.assign((size_t)std::max(1, delay_mid_n_), 0.f);
        c.wl = c.wm = 0;
    }
    atk_ = std::exp(-1.f / (kAtkMs * 0.001f * sr_));
    rel_ = std::exp(-1.f / (kRelMs * 0.001f * sr_));
    reset();
}

void Enhancer::reset() {
    for (auto& c : ch_) {
        c.s_lp_lo = c.s_hp_lo = c.s_lp_mid = c.s_hp_hi = c.s_lp_cap = c.s_hp_exc = BiquadState{};
        std::fill(c.dl_low.begin(), c.dl_low.end(), 0.f);
        std::fill(c.dl_mid.begin(), c.dl_mid.end(), 0.f);
        c.wl = c.wm = 0;
    }
    env_ = 0.f;
}

void Enhancer::process(float* L, float* R, int n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    const float gLow    = db2lin(p_[1].load(std::memory_order_relaxed));  // CONTOUR
    const float gMid    = db2lin(p_[2].load(std::memory_order_relaxed));  // MID
    const float procDb  = p_[0].load(std::memory_order_relaxed);          // PROCESS [dB]
    const float gHiMax  = db2lin(procDb);
    const float procNorm = procDb * (1.f / 12.f);                         // 0..1 (pro exciter)

    for (int i = 0; i < n; ++i) {
        float xL = L[i], xR = R[i];
        // broadband peak env (linkovany)
        float a = std::max(std::fabs(xL), std::fabs(xR));
        env_ = (a > env_) ? (atk_ * env_ + (1.f - atk_) * a)
                          : (rel_ * env_ + (1.f - rel_) * a);
        const float scale = smoothstep(env_, kScaleLo, kScaleHi);
        const float gHi   = 1.f + (gHiMax - 1.f) * scale;   // dynamicky boost-when-loud

        for (int chi = 0; chi < 2; ++chi) {
            Chan& c = ch_[chi];
            float x = (chi == 0) ? xL : xR;
            float low  = biquad_tick(x, lp_lo_, c.s_lp_lo);
            float rest = biquad_tick(x, hp_lo_, c.s_hp_lo);
            float mid  = biquad_tick(rest, lp_mid_, c.s_lp_mid);
            float high = biquad_tick(x, hp_hi_, c.s_hp_hi);
            high       = biquad_tick(high, lp_cap_, c.s_lp_cap);
            // harmonický exciter (2. harmonická), navázaný na PROCESS + dynamiku
            float exc  = high + kSat * high * std::fabs(high);     // jemná asymetrická saturace
            exc        = biquad_tick(exc, hp_exc_, c.s_hp_exc);    // ponech jen generované výšky
            const float excite_amt = kExcite * procNorm * scale;  // 0..kExcite (dynamické)
            float lowD = pushDelay_(c.dl_low, c.wl, delay_low_n_, low);
            float midD = pushDelay_(c.dl_mid, c.wm, delay_mid_n_, mid);
            float out  = lowD * gLow + midD * gMid + high * gHi + exc * excite_amt;
            if (chi == 0) L[i] = out; else R[i] = out;
        }
    }
}

} // namespace ithaca::dsp
```

- [ ] **Step 3: `engine/dsp/dsp_chain.h`** — nahraď BBE za Enhancer:
  - řádek 7 `#include "dsp/bbe.h"` → `#include "dsp/enhancer.h"`
  - komentáře ř. 2/4 „BBE" → „ENHANCER"
  - ř. 26 `BBE     bbe_;` → `Enhancer enhancer_;`
  - ř. 28 `{ &agc_, &bbe_, &lim_ }` → `{ &agc_, &enhancer_, &lim_ }`

- [ ] **Step 4: smaž staré + CMake:**
```bash
git rm engine/dsp/bbe.h engine/dsp/bbe.cpp
```
V `CMakeLists.txt` ř. 44 `engine/dsp/bbe.cpp` → `engine/dsp/enhancer.cpp`.
V `engine/engine.cpp:248` komentář `AGC -> BBE -> Limiter` → `AGC -> ENHANCER -> Limiter`.

- [ ] **Step 5: `tests/test_dsp.cpp`** — přepiš BBE testy (ř. ~83–118, 176–179) na Enhancer:
  - Zaměň `#include "dsp/bbe.h"` → `#include "dsp/enhancer.h"`.
  - „BBE: disabled = bit-identicky bypass" → `dsp::Enhancer e; e.prepare(48000.f,256); e.setEnabled(false); e.set(0,12.f); e.set(1,12.f);` proces 3 vzorky → identita. Přejmenuj na "Enhancer: disabled = bypass".
  - „BBE: oba parametry 0 dB" → "Enhancer: vsechny parametry 0 → pruchozi+aligned": `e.setEnabled(true)` s default (0/0/0); ověř, že výstup je konečný a ~unity magnitudy na DC (group-delay nemění DC úroveň). CHECK že výstup ≈ vstup po ustálení (DC propustí všemi pásmy součtem). (Tolerance volnější — crossover ripple.)
  - „BBE: bass boost zmeni DC uroven" → "Enhancer: CONTOUR boost zvedne DC": `e.set(1, 12.f)` (CONTOUR) → DC úroveň po ustálení > 1. (low pásmo propustí DC × gLow.)
  - „BBE: param round-trip + clamp" → Enhancer: `CHECK(e.paramCount()==3)`; `e.set(0,99.f); CHECK(e.get(0)==Approx(12.f))`; `e.set(2,-99.f); CHECK(e.get(2)==Approx(-6.f))`; `CHECK(std::string(e.name())=="ENHANCER")`.
  - „DspChain: poradi stage" ř. 179 `CHECK(...stage(1).name()=="BBE")` → `"ENHANCER"`.

- [ ] **Step 6: reconfigure + build + test:**
```
cmake -S . -B build >/dev/null && cmake --build build --target ithaca_core test_dsp 2>&1 | tail -10
ctest --test-dir build -R "^test_dsp$" --output-on-failure 2>&1 | tail -6
```
Expected: build OK, test_dsp PASS.

- [ ] **Step 7: Commit:**
```bash
git add engine/dsp/enhancer.h engine/dsp/enhancer.cpp engine/dsp/dsp_chain.h engine/engine.cpp CMakeLists.txt tests/test_dsp.cpp
git commit -m "feat(dsp): Enhancer (ex-BBE) — hybrid 3-band + dynamic PROCESS boost-when-loud"
```

---

### Task 3: Validační harness `test_enhancer_response.cpp`

**Files:** Create `tests/test_enhancer_response.cpp`; Delete `tests/test_bbe_measure.cpp`; Modify `tests/CMakeLists.txt`

**Build target:** `test_enhancer_response`.

- [ ] **Step 1: Test** — `tests/test_enhancer_response.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "dsp/enhancer.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace ithaca::dsp;
static const float kPi = 3.14159265358979323846f;

// Ustaleny zisk [dB] sinu freq pres Enhancer pri dane amplitude (level).
static float gainDb(Enhancer& e, float freq, float amp) {
    const float sr = 48000.f; const int N = 24000;
    std::vector<float> L(N), R(N);
    for (int i=0;i<N;++i){ float s=amp*std::sin(2.f*kPi*freq*(float)i/sr); L[i]=s; R[i]=s; }
    for (int off=0; off<N; off+=512){ int n=std::min(512,N-off); e.process(L.data()+off,R.data()+off,n); }
    double in_sq=0,out_sq=0; int c=0;
    for (int i=N/2;i<N;++i){ float s=amp*std::sin(2.f*kPi*freq*(float)i/sr); in_sq+=(double)s*s; out_sq+=(double)L[i]*L[i]; ++c; }
    return 20.f*std::log10(std::sqrt(out_sq/c)/std::sqrt(in_sq/c));
}

TEST_CASE("Enhancer response: bypass / contour / dynamic process") {
    // bypass = identita
    { Enhancer e; e.prepare(48000.f,512); e.setEnabled(false); e.set(0,12.f);
      CHECK(std::fabs(gainDb(e,5000.f,0.5f)) < 0.5f); }

    // CONTOUR: bass roste, NEzávislé na úrovni
    { Enhancer e; e.prepare(48000.f,512); e.setEnabled(true); e.set(1,12.f);
      float lo = gainDb(e,100.f,0.02f); e.reset();
      float hi = gainDb(e,100.f,0.5f);
      MESSAGE("CONTOUR 100Hz low-lvl="<<lo<<" high-lvl="<<hi);
      CHECK(hi > 2.f);                       // bass boost přítomen
      CHECK(std::fabs(hi-lo) < 2.f); }       // ~nezávislé na úrovni

    // PROCESS: HF roste a ZÁVISÍ na úrovni (boost-when-loud)
    { Enhancer e; e.prepare(48000.f,512); e.setEnabled(true); e.set(0,12.f);
      float lo = gainDb(e,6000.f,0.02f); e.reset();
      float hi = gainDb(e,6000.f,0.5f);
      MESSAGE("PROCESS 6kHz low-lvl="<<lo<<" high-lvl="<<hi);
      CHECK(hi > lo + 3.f); }                // level-delta ≥ 3 dB (dynamika)

    // EXCITER: čistý 3 kHz sinus → output má NOVOU 2. harmonickou (6 kHz)
    auto bin = [](const std::vector<float>& y, float f, float sr){
        double re=0,im=0; int N=(int)y.size();
        for (int i=N/2;i<N;++i){ double w=2.0*kPi*f*i/sr; re+=y[(size_t)i]*std::cos(w); im+=y[(size_t)i]*std::sin(w); }
        return std::sqrt(re*re+im*im)/(N/2); };
    auto run3k = [&](float proc, float amp){
        Enhancer e; e.prepare(48000.f,512); e.setEnabled(true); e.set(0,proc);
        const int N=24000; std::vector<float> L(N),R(N);
        for(int i=0;i<N;++i){ float s=amp*std::sin(2.f*kPi*3000.f*i/48000.f); L[i]=s;R[i]=s; }
        for(int o=0;o<N;o+=512){int n=std::min(512,N-o); e.process(L.data()+o,R.data()+o,n);}
        return bin(L, 6000.f, 48000.f); };
    {
        double h2_on  = run3k(12.f, 0.5f);   // exciter aktivní, silný signál
        double h2_off = run3k(0.f,  0.5f);   // PROCESS=0 → žádný exciter
        MESSAGE("2nd harmonic @6kHz: process12="<<h2_on<<" process0="<<h2_off);
        CHECK(h2_on > h2_off * 2.0);         // exciter generuje novou harmonickou
    }

    // CSV pro overlay na Naganova (dvě úrovně)
    { Enhancer e; e.prepare(48000.f,512); e.setEnabled(true); e.set(0,12.f); e.set(1,12.f);
      std::printf("freq,gain_db_low,gain_db_high\n");
      const float freqs[]={40,60,100,150,250,500,1000,2000,3000,4000,6000,8000,12000,16000};
      for (float f: freqs){ e.reset(); float gl=gainDb(e,f,0.02f); e.reset(); float gh=gainDb(e,f,0.5f);
        std::printf("%.0f,%.2f,%.2f\n", f, gl, gh); } }
}
```
(Group delay test může přibýt později; tento harness pokrývá magnitudu + level-dependenci, což jsou klíčové Naganovovy charakteristiky. CSV jde do stdoutu testu.)

- [ ] **Step 2: CMake** — v `tests/CMakeLists.txt` odeber/nahraď registraci `test_bbe_measure` (pokud byla; nebyla registrovaná — jen smaž soubor) a přidej:
```cmake
add_executable(test_enhancer_response test_enhancer_response.cpp)
target_link_libraries(test_enhancer_response PRIVATE ithaca_core doctest)
add_test(NAME test_enhancer_response COMMAND test_enhancer_response)
```
```bash
git rm tests/test_bbe_measure.cpp
```

- [ ] **Step 3: build + test:**
```
cmake -S . -B build >/dev/null && cmake --build build --target test_enhancer_response 2>&1 | tail -6
ctest --test-dir build -R test_enhancer_response --output-on-failure 2>&1 | tail -20
```
Expected: PASS; v outputu CSV + MESSAGE řádky (level-delta @ 6 kHz ≥ 3 dB).
> Pokud level-delta < 3 dB nebo CONTOUR boost < 2 dB: dolaď konstanty v `enhancer.cpp` (`kScaleLo/kScaleHi`, crossover Q, rohy) — NE oslabuj assert. Zaznamenej, co jsi ladil.

- [ ] **Step 4: Commit:**
```bash
git add tests/test_enhancer_response.cpp tests/CMakeLists.txt
git commit -m "test(enhancer): response harness — bypass/contour/dynamic-process + CSV vs Naganov"
```

---

### Task 4: Persistence + app_context + main (rename + 3 parametry + migrace)

**Files:** Modify `app/gui/persistence.h`, `app/gui/persistence.cpp`, `app/gui/app_context.cpp`, `app/gui/main.cpp`

**Build target:** `ithaca-gui` + `test_persistence`.

- [ ] **Step 1: `persistence.h`** — ř. 30 nahraď:
```cpp
    bool  bbe_enabled = false;     float bbe_definition = 0.f; float bbe_bass = 0.f;
```
za:
```cpp
    bool  enhancer_enabled = false; float enhancer_process = 0.f; float enhancer_contour = 0.f; float enhancer_mid = 0.f;
```
A ř. 32 komentář config_page `2 = BBE` → `2 = AGC, 3 = ENHANCER` (sjednotit: 0=MASTER,1=RESONANCE,2=AGC,3=ENHANCER,4=LIMITER).

- [ ] **Step 2: `persistence.cpp` load** (ř. ~127–129) nahraď za migrující čtení:
```cpp
        // Enhancer (ex-BBE): cti enhancer_*, fallback na stare bbe_* (migrace).
        s.enhancer_enabled = readB("enhancer_enabled", readB("bbe_enabled", s.enhancer_enabled));
        s.enhancer_process = readF("enhancer_process", readF("bbe_definition", s.enhancer_process));
        s.enhancer_contour = readF("enhancer_contour", readF("bbe_bass", s.enhancer_contour));
        s.enhancer_mid     = readF("enhancer_mid", s.enhancer_mid);
```

- [ ] **Step 3: `persistence.cpp` save** (ř. ~177–179) nahraď:
```cpp
        f << "  \"bbe_enabled\": "        << (s.bbe_enabled ? "true" : "false") << ",\n";
        f << "  \"bbe_definition\": "     << s.bbe_definition       << ",\n";
        f << "  \"bbe_bass\": "           << s.bbe_bass             << ",\n";
```
za:
```cpp
        f << "  \"enhancer_enabled\": "   << (s.enhancer_enabled ? "true" : "false") << ",\n";
        f << "  \"enhancer_process\": "   << s.enhancer_process     << ",\n";
        f << "  \"enhancer_contour\": "   << s.enhancer_contour     << ",\n";
        f << "  \"enhancer_mid\": "       << s.enhancer_mid         << ",\n";
```

- [ ] **Step 4: `app_context.cpp`** (ř. ~78–82) nahraď mapování stage:
```cpp
        auto& agc = ch.stage(0); auto& bbe = ch.stage(1); auto& lim = ch.stage(2);
        agc.set(0, state.agc_target); agc.set(1, state.agc_release_ms); agc.set(2, state.agc_floor);
        agc.setEnabled(state.agc_enabled);
        bbe.set(0, state.bbe_definition); bbe.set(1, state.bbe_bass);
        bbe.setEnabled(state.bbe_enabled);
```
za (zachovej AGC řádky beze změny; jen Enhancer):
```cpp
        auto& agc = ch.stage(0); auto& enh = ch.stage(1); auto& lim = ch.stage(2);
        agc.set(0, state.agc_target); agc.set(1, state.agc_release_ms); agc.set(2, state.agc_floor);
        agc.setEnabled(state.agc_enabled);
        enh.set(0, state.enhancer_process); enh.set(1, state.enhancer_contour); enh.set(2, state.enhancer_mid);
        enh.setEnabled(state.enhancer_enabled);
```
A komentář ř. 74 `BBE[definition,bass]` → `ENHANCER[process,contour,mid]`. (`lim` se používá dál beze změny.)

- [ ] **Step 5: `main.cpp`** — sync z stage (ř. ~256–260):
```cpp
            auto& agc = ch.stage(0); auto& bbe = ch.stage(1); auto& lim = ch.stage(2);
            ...
            ctx.state.bbe_enabled = bbe.enabled();
            ctx.state.bbe_definition = bbe.get(0); ctx.state.bbe_bass = bbe.get(1);
```
nahraď (zachovej AGC/LIMITER sync):
```cpp
            auto& agc = ch.stage(0); auto& enh = ch.stage(1); auto& lim = ch.stage(2);
            ...
            ctx.state.enhancer_enabled = enh.enabled();
            ctx.state.enhancer_process = enh.get(0); ctx.state.enhancer_contour = enh.get(1); ctx.state.enhancer_mid = enh.get(2);
```
Komentář ř. 189 `// BBE` → `// ENHANCER`.
Debounce (ř. ~290–292) nahraď:
```cpp
            last_saved.bbe_enabled         != ctx.state.bbe_enabled ||
            last_saved.bbe_definition      != ctx.state.bbe_definition ||
            last_saved.bbe_bass            != ctx.state.bbe_bass ||
```
za:
```cpp
            last_saved.enhancer_enabled    != ctx.state.enhancer_enabled ||
            last_saved.enhancer_process    != ctx.state.enhancer_process ||
            last_saved.enhancer_contour    != ctx.state.enhancer_contour ||
            last_saved.enhancer_mid        != ctx.state.enhancer_mid ||
```

- [ ] **Step 6: build + test:**
```
cmake --build build --target ithaca-gui test_persistence 2>&1 | tail -10
ctest --test-dir build -R test_persistence --output-on-failure 2>&1 | tail -4
```
Expected: build OK (no new warnings); test_persistence PASS. Grep ověř: `grep -rn "bbe" app/gui | grep -v "//"` → nic.
> test_persistence asseruje round-trip; pokud testuje `bbe_*`, aktualizuj na `enhancer_*` (process/contour/mid) — viz `tests/test_persistence.cpp`.

- [ ] **Step 7: Commit:**
```bash
git add app/gui/persistence.h app/gui/persistence.cpp app/gui/app_context.cpp app/gui/main.cpp tests/test_persistence.cpp
git commit -m "feat(gui): rename BBE→Enhancer state (process/contour/mid) + bbe_* migration"
```

---

### Task 5: Reference dokumentace

**Files:** Modify `docs/reference/G-dsp.md`, `docs/reference/H-gui.md`

- [ ] **Step 1:** G-dsp — BBE sekce → **Enhancer**: hybridní 3-pásmová topologie (LOW/MID/HIGH crossovery 250/3000 Hz, HF cap 11k), per-pásmové delaye 2,5/0,5/0 ms, dynamický PROCESS (boost-when-loud peak monitor), params PROCESS/CONTOUR/MID. Odkaz na spec + reference (Lumin schéma, Naganov).
- [ ] **Step 2:** H-gui — CONFIG stránky „BBE" → „ENHANCER" (3 params); persistence pole `enhancer_*`; `config_page` mapování.
- [ ] **Step 3: Commit:**
```bash
git add docs/reference/
git commit -m "docs(reference): BBE → Enhancer (hybrid 3-band dynamic)"
```

---

### Task 6: Full build, tests, smoke, finish

- [ ] **Step 1: Full build + suita:**
```
cmake --build build 2>&1 | tail -6 && ctest --test-dir build --output-on-failure 2>&1 | tail -12
```
Expected: build OK, všechny testy PASS (vč. test_enhancer_response).

- [ ] **Step 2: Smoke (uživatel, `./build/ithaca-gui`):**
  - CONFIG: stránka „ENHANCER" se 3 slidery PROCESS/CONTOUR/MID + ON/OFF.
  - PROCESS nahoru + hraj akord silně → výrazné zjasnění; hraj tiše → boost mizí (dynamika). Toggle ON/OFF teď **slyšitelně** mění charakter.
  - CONTOUR → basy (konstantní); MID → středy.
  - Restart app → hodnoty se zachovaly (migrace ze starých bbe_* při prvním načtení).
  - (Volitelně) zkopíruj CSV z `test_enhancer_response` do gnuplot/sheet a overlayuj na Naganovovy grafy.

- [ ] **Step 3: Finish branch** — REQUIRED SUB-SKILL: superpowers:finishing-a-development-branch (merge `feat/enhancer-dynamic` → main, push).

---

## Self-Review
- **Spec coverage:** 3-band hybrid topologie → T2; dsp_math helpery → T1; dynamický PROCESS boost-when-loud (peak monitor) → T2; per-band delay group-align → T2; rename BBE→Enhancer (engine/dsp_chain/CMake/test_dsp) → T2; persistence enhancer_* + migrace → T4; app_context/main 3 params → T4; validační harness + CSV → T3; docs → T5; finish → T6. ✓
- **Type consistency:** `Enhancer`, `name()="ENHANCER"`, `paramCount()=3`, params PROCESS/CONTOUR/MID (idx 0/1/2), `enhancer_enabled/process/contour/mid`, `rbj_lowpass/highpass/smoothstep` konzistentní napříč tasky. dsp_chain `enhancer_`. ✓
- **Placeholder scan:** opraveno — `hasEnable()` má vracet `true` (poznámka u kódu); ladění konstant v T3 má konkrétní cíle (≥3 dB delta), ne placeholder.
- **Build-green pořadí:** T1/T2 core+test_dsp; T3 test target; T4 gui+test_persistence; plný build T6. GUI nebuildí mezi T2–T4 (staví se jen uvedené targety). ✓
- **Pozn. testovatelnost:** T1/T2/T3/T4 deterministicky (doctest); charakter + Naganov overlay = smoke + CSV (T6).
