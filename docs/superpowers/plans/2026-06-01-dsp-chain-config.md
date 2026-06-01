# Modular DSP Chain + CONFIG Switcher — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a real post-mix DSP chain (AGC → BBE → Limiter) behind a generic parameter-descriptor interface, and a GUI CONFIG selector that swaps the middle parameter panel to edit the selected stage (VOICE included).

**Architecture:** New `engine/dsp/` module. `IParamPage` (descriptor list + get/set/enable/meter) is the common GUI-facing interface; `DspStage : IParamPage` adds `prepare/reset/process`. `DspChain` owns AGC+BBE+Limiter in fixed order and runs enabled stages. Engine calls the chain after master gain, before the peak meter. GUI renders the selected `IParamPage` through one generic loop; `VoicePage` adapts existing voice controls to the same interface. Params stored as atomics; coefficients recomputed on the audio thread when a value changes (RT-safe).

**Tech Stack:** C++20, Dear ImGui 1.91 + GLFW (existing GUI), doctest + CTest. DSP modules ported from `/Users/j/Projects/icr/engine/dsp/`.

**Spec:** `docs/superpowers/specs/2026-06-01-dsp-chain-config-design.md`

**Build/test commands (run from repo root `/Users/j/Projects/ithaca-legacy`):**
- Build everything: `cmake --build build -j`
- Run all tests: `ctest --test-dir build`
- Run one test exe: `./build/tests/test_dsp` (doctest binary)
- GUI smoke: `( ./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca >/tmp/smoke.txt 2>&1 & echo $! >/tmp/pid ); sleep 3; kill $(cat /tmp/pid); tail -3 /tmp/smoke.txt`

**Conventions:** Czech comments are the norm. Namespace `ithaca` (DSP in `ithaca::dsp`). Commits end with the trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Do NOT commit `imgui.ini`. Work proceeds directly on `main` (the established workflow this session).

---

## File Structure

**New (engine, compiled into `ithaca_core`):**
- `engine/dsp/dsp_math.h` — ported math primitives (header-only).
- `engine/dsp/dsp_stage.h` — `Param`, `IParamPage`, `DspStage` (header-only interfaces).
- `engine/dsp/limiter.h` + `engine/dsp/limiter.cpp` — peak limiter stage.
- `engine/dsp/bbe.h` + `engine/dsp/bbe.cpp` — BBE shelving stage.
- `engine/dsp/agc.h` + `engine/dsp/agc.cpp` — AGC stage.
- `engine/dsp/dsp_chain.h` + `engine/dsp/dsp_chain.cpp` — owns + runs the three stages.

**New (GUI, compiled into `ithaca-gui`):**
- `app/gui/voice_page.h` — `VoicePage : IParamPage` (header-only) bridging engine voice params.
- `app/gui/panel_config.h` + `app/gui/panel_config.cpp` — CONFIG selector list (right column).

**New (tests):**
- `tests/test_dsp.cpp` — doctest unit tests for math + stages + chain.

**Modify:**
- `engine/engine.h` / `engine/engine.cpp` — chain member, prepare, processBlock insertion, `dspChain()` getter, re-prepare in `setBlockSize`.
- `app/gui/panel_params.h` / `app/gui/panel_params.cpp` — replace `renderParamsPanel` with generic `renderParamPage(AppContext&, ithaca::dsp::IParamPage&)`.
- `app/gui/widgets.h` — add `wdg::ToggleChip` (ON/OFF) helper.
- `app/gui/main.cpp` — page list + selection, render selected page in middle, CONFIG in right column.
- `app/gui/persistence.h` / `app/gui/persistence.cpp` — schema v4 DSP fields + migration.
- `app/gui/app_context.cpp` — apply persisted DSP params to chain at init.
- `CMakeLists.txt` — add dsp sources to `ithaca_core`, add `panel_config.cpp` to `ithaca-gui`, drop `panel_dsp.cpp`.
- `tests/CMakeLists.txt` — register `test_dsp`.

**Delete:**
- `app/gui/panel_dsp.h`, `app/gui/panel_dsp.cpp` (placeholder rack).

---

## Task 1: DSP math + interfaces + test scaffolding

**Files:**
- Create: `engine/dsp/dsp_math.h`
- Create: `engine/dsp/dsp_stage.h`
- Create: `tests/test_dsp.cpp`
- Modify: `tests/CMakeLists.txt` (after the `ithaca_tests` block, lines 2-4)

- [ ] **Step 1: Create `engine/dsp/dsp_math.h`** (ported from icr, namespace `ithaca::dsp`, only the helpers our stages need)

```cpp
#pragma once
// engine/dsp/dsp_math.h — sdilene DSP primitivy (stateless, inline, RT-safe).
// Portovano z icr/engine/dsp/dsp_math.h. Pouziva BBE, Limiter, AGC.
#include <cmath>
#include <algorithm>

namespace ithaca::dsp {

static constexpr float PI  = 3.14159265358979f;
static constexpr float TAU = 2.f * PI;

// dB -> linearni amplituda. 0 dB -> 1.0
inline float db_to_lin(float db) { return std::pow(10.f, db / 20.f); }
// linearni amplituda -> dB. 1.0 -> 0 dB
inline float lin_to_db(float lin) { return 20.f * std::log10((std::max)(lin, 1e-9f)); }

// Per-sample multiplikativni decay koeficient: exp(-1 / (tau_s * sr)).
inline float decay_coeff(float tau_seconds, float sample_rate) {
    return std::exp(-1.f / (std::max)(tau_seconds * sample_rate, 1.f));
}

// Biquad koeficienty (normalizovane, a0 = 1).
struct BiquadCoeffs { float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f; };
// Biquad stav (DF-II transposed, 2 delay elementy).
struct BiquadState { float s1 = 0.f, s2 = 0.f; };

// Jeden vzorek pres biquad (DF-II transposed). Aktualizuje stav in-place.
inline float biquad_tick(float x, const BiquadCoeffs& c, BiquadState& s) {
    float y = c.b0 * x + s.s1;
    s.s1    = c.b1 * x - c.a1 * y + s.s2;
    s.s2    = c.b2 * x - c.a2 * y;
    return y;
}

// RBJ high-shelf (Audio EQ Cookbook). fc=stred [Hz], gain_db boost/cut, sr.
inline BiquadCoeffs rbj_high_shelf(float fc, float gain_db, float sr) {
    float A = std::pow(10.f, gain_db / 40.f);
    float w0 = TAU * fc / sr, cosw = std::cos(w0), sinw = std::sin(w0);
    float al = sinw / 2.f * std::sqrt((A + 1.f/A) * (1.f/1.f - 1.f) + 2.f);
    float sqA2 = 2.f * std::sqrt(A) * al;
    float a0 = (A+1.f) - (A-1.f)*cosw + sqA2, ia = 1.f / a0;
    BiquadCoeffs c;
    c.b0 = ( A * ((A+1.f) + (A-1.f)*cosw + sqA2)) * ia;
    c.b1 = (-2.f*A * ((A-1.f) + (A+1.f)*cosw))    * ia;
    c.b2 = ( A * ((A+1.f) + (A-1.f)*cosw - sqA2)) * ia;
    c.a1 = ( 2.f * ((A-1.f) - (A+1.f)*cosw))      * ia;
    c.a2 = (       (A+1.f) - (A-1.f)*cosw - sqA2)  * ia;
    return c;
}
// RBJ low-shelf.
inline BiquadCoeffs rbj_low_shelf(float fc, float gain_db, float sr) {
    float A = std::pow(10.f, gain_db / 40.f);
    float w0 = TAU * fc / sr, cosw = std::cos(w0), sinw = std::sin(w0);
    float al = sinw / 2.f * std::sqrt((A + 1.f/A) * (1.f/1.f - 1.f) + 2.f);
    float sqA2 = 2.f * std::sqrt(A) * al;
    float a0 = (A+1.f) + (A-1.f)*cosw + sqA2, ia = 1.f / a0;
    BiquadCoeffs c;
    c.b0 = ( A * ((A+1.f) - (A-1.f)*cosw + sqA2)) * ia;
    c.b1 = ( 2.f*A * ((A-1.f) - (A+1.f)*cosw))    * ia;
    c.b2 = ( A * ((A+1.f) - (A-1.f)*cosw - sqA2)) * ia;
    c.a1 = (-2.f * ((A-1.f) + (A+1.f)*cosw))      * ia;
    c.a2 = (       (A+1.f) + (A-1.f)*cosw - sqA2)  * ia;
    return c;
}

// Vyhlazeni gain obalky: rychly attack (snizovani), pomaly release (zotaveni).
inline float gain_envelope_smooth(float current, float target,
                                  float attack_coeff, float release_coeff) {
    if (target < current) return attack_coeff  * current + (1.f - attack_coeff)  * target;
    else                  return release_coeff * current + (1.f - release_coeff) * target;
}

} // namespace ithaca::dsp
```

- [ ] **Step 2: Create `engine/dsp/dsp_stage.h`** (interfaces)

```cpp
#pragma once
// engine/dsp/dsp_stage.h — genericke rozhrani DSP stage + parametricky deskriptor.
// IParamPage je GUI-facing (implementuje ho kazda stage I "VOICE" stranka v GUI).
// DspStage pridava audio-thread zpracovani. Viz spec 2026-06-01-dsp-chain-config.

namespace ithaca::dsp {

// Deskriptor jednoho parametru pro genericke GUI vykresleni + persistenci.
struct Param {
    const char* id;            // stabilni klic pro persistenci ("threshold_db")
    const char* label;         // UI eyebrow ("THRESHOLD")
    float       min, max, def;
    const char* fmt;           // DecoSlider format ("%.1f dB")
    bool        readonly = false;
};

// GUI-facing rozhrani. set() klampuje do [min,max].
struct IParamPage {
    virtual ~IParamPage() = default;
    virtual const char* name() const = 0;
    virtual int          paramCount() const = 0;
    virtual const Param& param(int i) const = 0;
    virtual float        get(int i) const = 0;
    virtual void         set(int i, float v) = 0;
    virtual bool         hasEnable() const = 0;   // VOICE: false
    virtual bool         enabled() const = 0;
    virtual void         setEnabled(bool on) = 0;
    // Volitelny read-only metr (limiter GR / AGC current gain).
    // Vraci false kdyz stranka metr nema.
    virtual bool         meter(float& value, const char*& label) const = 0;
};

// Audio-thread stage = param page + DSP. set()/setEnabled() z GUI threadu,
// process() z audio threadu; implementace drzi parametry atomicky.
struct DspStage : IParamPage {
    virtual void prepare(float sample_rate, int max_block) = 0;
    virtual void reset() = 0;
    virtual void process(float* L, float* R, int n) = 0;
};

} // namespace ithaca::dsp
```

- [ ] **Step 3: Create `tests/test_dsp.cpp`** with math sanity tests (failing until headers exist — they exist after steps 1-2, so this verifies compile + correctness)

```cpp
// tests/test_dsp.cpp — DSP math + stage + chain unit testy (doctest).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/dsp_math.h"
#include <cmath>

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
```

- [ ] **Step 4: Register `test_dsp` in `tests/CMakeLists.txt`** — insert after line 4 (the `ithaca_tests` `add_test`)

```cmake
add_executable(test_dsp test_dsp.cpp)
target_link_libraries(test_dsp PRIVATE ithaca_core doctest)
add_test(NAME test_dsp COMMAND test_dsp)
```

- [ ] **Step 5: Build + run**

Run: `cmake --build build -j && ctest --test-dir build -R test_dsp`
Expected: build succeeds; `test_dsp` passes (3 test cases).

- [ ] **Step 6: Commit**

```bash
git add engine/dsp/dsp_math.h engine/dsp/dsp_stage.h tests/test_dsp.cpp tests/CMakeLists.txt
git commit -m "feat(dsp): math primitives + IParamPage/DspStage interfaces + test scaffold

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Limiter stage

**Files:**
- Create: `engine/dsp/limiter.h`
- Create: `engine/dsp/limiter.cpp`
- Modify: `CMakeLists.txt` (add to `ithaca_core` source list, after line 41 `engine/engine.cpp`)
- Test: `tests/test_dsp.cpp` (append)

- [ ] **Step 1: Write failing tests** — append to `tests/test_dsp.cpp` (add `#include "dsp/limiter.h"` near the top includes)

```cpp
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
    // Po ustaleni (konec bloku) musi byt vystup <= prah (s malou tolerance).
    CHECK(std::abs(L[4799]) <= thr * 1.05f);
    float gr, dummy; const char* lbl;
    CHECK(lim.meter(gr, lbl) == true);
    CHECK(gr < 0.f);                 // limituje -> zaporna GR
    (void)dummy;
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
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: FAIL — `'limiter.h' file not found` / `dsp::Limiter` undefined.

- [ ] **Step 3: Create `engine/dsp/limiter.h`**

```cpp
#pragma once
// engine/dsp/limiter.h — stereo peak limiter jako DspStage.
// Algoritmus portovan z icr/engine/dsp/limiter/limiter.{h,cpp}: per-sample peak
// max(|L|,|R|), target = threshold/peak, vyhlazena obalka (fast attack / release),
// gain vzdy <= 1. Parametry atomicky; koeficienty se prepocitaji v process().
#include "dsp/dsp_stage.h"
#include "dsp/dsp_math.h"
#include <atomic>

namespace ithaca::dsp {

class Limiter : public DspStage {
public:
    const char* name() const override { return "LIMITER"; }
    void prepare(float sr, int /*max_block*/) override {
        sr_ = sr;
        atk_ = decay_coeff(0.001f, sr_);          // fixni 1 ms attack
        applyParams_(/*force=*/true);
        reset();
    }
    void reset() override { gain_ = 1.f; gr_db_.store(0.f, std::memory_order_relaxed); }

    int paramCount() const override { return 2; }
    const Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override {
        return (i == 0) ? thr_db_.load(std::memory_order_relaxed)
                        : rel_ms_.load(std::memory_order_relaxed);
    }
    void set(int i, float v) override {
        v = (v < kParams[i].min) ? kParams[i].min : (v > kParams[i].max ? kParams[i].max : v);
        if (i == 0) thr_db_.store(v, std::memory_order_relaxed);
        else        rel_ms_.store(v, std::memory_order_relaxed);
    }
    bool hasEnable() const override { return true; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool on) override { enabled_.store(on, std::memory_order_relaxed); }
    bool meter(float& value, const char*& label) const override {
        value = gr_db_.load(std::memory_order_relaxed); label = "GAIN REDUCTION"; return true;
    }

    void process(float* L, float* R, int n) override;

private:
    static const Param kParams[2];
    // GUI-nastavovane parametry (atomic).
    std::atomic<float> thr_db_{0.f};
    std::atomic<float> rel_ms_{200.f};
    std::atomic<bool>  enabled_{false};
    // Audio-thread odvozene koeficienty + stav.
    float sr_ = 48000.f, gain_ = 1.f, thr_lin_ = 1.f, atk_ = 0.f, rel_ = 0.f;
    float last_thr_db_ = 1e9f, last_rel_ms_ = -1.f;
    std::atomic<float> gr_db_{0.f};   // metr (psan audio, cten GUI)
    // Prepocita thr_lin_/rel_ z atomickych parametru kdyz se zmenily.
    void applyParams_(bool force);
};

} // namespace ithaca::dsp
```

- [ ] **Step 4: Create `engine/dsp/limiter.cpp`**

```cpp
#include "dsp/limiter.h"
#include <algorithm>
#include <cmath>

namespace ithaca::dsp {

const Param Limiter::kParams[2] = {
    {"threshold_db", "THRESHOLD", -40.f, 0.f,   0.f,   "%.1f dB", false},
    {"release_ms",   "RELEASE",    10.f, 2000.f, 200.f, "%.0f ms", false},
};

void Limiter::applyParams_(bool force) {
    const float thr_db = thr_db_.load(std::memory_order_relaxed);
    const float rel_ms = rel_ms_.load(std::memory_order_relaxed);
    if (force || thr_db != last_thr_db_) { thr_lin_ = db_to_lin(thr_db); last_thr_db_ = thr_db; }
    if (force || rel_ms != last_rel_ms_) { rel_ = decay_coeff(rel_ms * 0.001f, sr_); last_rel_ms_ = rel_ms; }
}

void Limiter::process(float* L, float* R, int n) {
    applyParams_(/*force=*/false);
    for (int i = 0; i < n; ++i) {
        float peak = std::max(std::abs(L[i]), std::abs(R[i]));
        float target = (peak > thr_lin_ && peak > 1e-9f) ? thr_lin_ / peak : 1.f;
        gain_ = gain_envelope_smooth(gain_, target, atk_, rel_);
        gain_ = std::min(gain_, 1.f);
        L[i] *= gain_;
        R[i] *= gain_;
    }
    gr_db_.store(lin_to_db(std::max(gain_, 1e-9f)), std::memory_order_relaxed);
}

} // namespace ithaca::dsp
```

- [ ] **Step 5: Add source to `CMakeLists.txt`** — in the `ithaca_core` list, after `engine/engine.cpp` (line 41), add:

```cmake
    engine/dsp/limiter.cpp
```

- [ ] **Step 6: Build + run tests**

Run: `cmake --build build -j && ctest --test-dir build -R test_dsp`
Expected: PASS (all Limiter cases green).

- [ ] **Step 7: Commit**

```bash
git add engine/dsp/limiter.h engine/dsp/limiter.cpp tests/test_dsp.cpp CMakeLists.txt
git commit -m "feat(dsp): peak limiter stage (ported from icr)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: BBE stage

**Files:**
- Create: `engine/dsp/bbe.h`, `engine/dsp/bbe.cpp`
- Modify: `CMakeLists.txt` (add `engine/dsp/bbe.cpp`)
- Test: `tests/test_dsp.cpp` (append; add `#include "dsp/bbe.h"`)

- [ ] **Step 1: Write failing tests**

```cpp
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
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: FAIL — `'bbe.h' file not found`.

- [ ] **Step 3: Create `engine/dsp/bbe.h`**

```cpp
#pragma once
// engine/dsp/bbe.h — zjednoduseny BBE Sonic Maximizer jako DspStage.
// Dva RBJ shelving biquady na kanal: DEFINITION (high shelf 5 kHz, 0..12 dB),
// BASS (low shelf 180 Hz, 0..10 dB). Portovano z icr/engine/dsp/bbe.
#include "dsp/dsp_stage.h"
#include "dsp/dsp_math.h"
#include <atomic>

namespace ithaca::dsp {

class BBE : public DspStage {
public:
    const char* name() const override { return "BBE"; }
    void prepare(float sr, int /*max_block*/) override { sr_ = sr; applyParams_(true); reset(); }
    void reset() override { def_l_={}; def_r_={}; bass_l_={}; bass_r_={}; }

    int paramCount() const override { return 2; }
    const Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override {
        return (i==0) ? def_db_.load(std::memory_order_relaxed)
                      : bass_db_.load(std::memory_order_relaxed);
    }
    void set(int i, float v) override {
        v = (v < kParams[i].min) ? kParams[i].min : (v > kParams[i].max ? kParams[i].max : v);
        if (i==0) def_db_.store(v, std::memory_order_relaxed);
        else      bass_db_.store(v, std::memory_order_relaxed);
    }
    bool hasEnable() const override { return true; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool on) override { enabled_.store(on, std::memory_order_relaxed); }
    bool meter(float&, const char*&) const override { return false; }

    void process(float* L, float* R, int n) override;

private:
    static const Param kParams[2];
    std::atomic<float> def_db_{0.f};
    std::atomic<float> bass_db_{0.f};
    std::atomic<bool>  enabled_{false};
    float sr_ = 48000.f;
    float last_def_db_ = 1e9f, last_bass_db_ = 1e9f;
    BiquadCoeffs def_c_{}, bass_c_{};
    BiquadState  def_l_{}, def_r_{}, bass_l_{}, bass_r_{};
    void applyParams_(bool force);
};

} // namespace ithaca::dsp
```

- [ ] **Step 4: Create `engine/dsp/bbe.cpp`**

```cpp
#include "dsp/bbe.h"

namespace ithaca::dsp {

const Param BBE::kParams[2] = {
    {"definition", "DEFINITION", 0.f, 12.f, 0.f, "%.1f dB", false},
    {"bass",       "BASS",       0.f, 10.f, 0.f, "%.1f dB", false},
};

void BBE::applyParams_(bool force) {
    const float d = def_db_.load(std::memory_order_relaxed);
    const float b = bass_db_.load(std::memory_order_relaxed);
    if (force || d != last_def_db_)  { def_c_  = rbj_high_shelf(5000.f, d, sr_); last_def_db_  = d; }
    if (force || b != last_bass_db_) { bass_c_ = rbj_low_shelf (180.f,  b, sr_); last_bass_db_ = b; }
}

void BBE::process(float* L, float* R, int n) {
    applyParams_(/*force=*/false);
    for (int i = 0; i < n; ++i) {
        L[i] = biquad_tick(L[i], def_c_,  def_l_);
        R[i] = biquad_tick(R[i], def_c_,  def_r_);
        L[i] = biquad_tick(L[i], bass_c_, bass_l_);
        R[i] = biquad_tick(R[i], bass_c_, bass_r_);
    }
}

} // namespace ithaca::dsp
```

- [ ] **Step 5: Add `engine/dsp/bbe.cpp` to `CMakeLists.txt`** (`ithaca_core` list, after `engine/dsp/limiter.cpp`).

- [ ] **Step 6: Build + run**

Run: `cmake --build build -j && ctest --test-dir build -R test_dsp`
Expected: PASS (BBE cases green).

- [ ] **Step 7: Commit**

```bash
git add engine/dsp/bbe.h engine/dsp/bbe.cpp tests/test_dsp.cpp CMakeLists.txt
git commit -m "feat(dsp): BBE shelving stage (ported from icr)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: AGC stage

**Files:**
- Create: `engine/dsp/agc.h`, `engine/dsp/agc.cpp`
- Modify: `CMakeLists.txt` (add `engine/dsp/agc.cpp`)
- Test: `tests/test_dsp.cpp` (append; add `#include "dsp/agc.h"`)

- [ ] **Step 1: Write failing tests**

```cpp
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
    // gain >= floor -> output >= input*floor
    CHECK(std::abs(L[4799]) >= 5.f * 0.1f * 0.9f);
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
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: FAIL — `'agc.h' file not found`.

- [ ] **Step 3: Create `engine/dsp/agc.h`**

```cpp
#pragma once
// engine/dsp/agc.h — RMS-following downward AGC jako DspStage. Meri per-blok RMS
// a vyhlazene snizuje gain k target RMS; nikdy nezesiluje, nikdy pod floor.
// Algoritmus portovan z icr/engine/dsp/agc.h (AgcState/agc_process).
#include "dsp/dsp_stage.h"
#include "dsp/dsp_math.h"
#include <atomic>

namespace ithaca::dsp {

class AGC : public DspStage {
public:
    const char* name() const override { return "AGC"; }
    void prepare(float sr, int /*max_block*/) override {
        sr_ = sr;
        atk_ = 1.f - std::exp(-1.f / (5.f * 0.001f * sr_));   // fixni 5 ms attack
        applyParams_(true);
        reset();
    }
    void reset() override { gain_ = 1.f; cur_gain_.store(1.f, std::memory_order_relaxed); }

    int paramCount() const override { return 3; }
    const Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override {
        if (i==0) return target_.load(std::memory_order_relaxed);
        if (i==1) return release_ms_.load(std::memory_order_relaxed);
        return floor_.load(std::memory_order_relaxed);
    }
    void set(int i, float v) override {
        v = (v < kParams[i].min) ? kParams[i].min : (v > kParams[i].max ? kParams[i].max : v);
        if (i==0) target_.store(v, std::memory_order_relaxed);
        else if (i==1) release_ms_.store(v, std::memory_order_relaxed);
        else floor_.store(v, std::memory_order_relaxed);
    }
    bool hasEnable() const override { return true; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool on) override { enabled_.store(on, std::memory_order_relaxed); }
    bool meter(float& value, const char*& label) const override {
        value = cur_gain_.load(std::memory_order_relaxed); label = "CURRENT GAIN"; return true;
    }

    void process(float* L, float* R, int n) override;

private:
    static const Param kParams[3];
    std::atomic<float> target_{0.15f};
    std::atomic<float> release_ms_{200.f};
    std::atomic<float> floor_{0.05f};
    std::atomic<bool>  enabled_{false};
    float sr_ = 48000.f, gain_ = 1.f, atk_ = 0.f, rel_ = 0.f;
    float last_rel_ms_ = -1.f;
    std::atomic<float> cur_gain_{1.f};
    void applyParams_(bool force);
};

} // namespace ithaca::dsp
```

- [ ] **Step 4: Create `engine/dsp/agc.cpp`**

```cpp
#include "dsp/agc.h"
#include <cmath>

namespace ithaca::dsp {

const Param AGC::kParams[3] = {
    {"target_rms", "TARGET",     0.01f, 0.5f,   0.15f, "%.3f",    false},
    {"release_ms", "RELEASE",    10.f,  2000.f, 200.f, "%.0f ms", false},
    {"gain_floor", "GAIN FLOOR", 0.f,   1.f,    0.05f, "%.2f",    false},
};

void AGC::applyParams_(bool force) {
    const float rel_ms = release_ms_.load(std::memory_order_relaxed);
    if (force || rel_ms != last_rel_ms_) {
        rel_ = 1.f - std::exp(-1.f / (rel_ms * 0.001f * sr_));
        last_rel_ms_ = rel_ms;
    }
}

void AGC::process(float* L, float* R, int n) {
    applyParams_(/*force=*/false);
    const float target_rms = target_.load(std::memory_order_relaxed);
    const float gain_floor = floor_.load(std::memory_order_relaxed);

    // Per-blok RMS pres oba kanaly.
    float sum_sq = 0.f;
    for (int i = 0; i < n; ++i) sum_sq += L[i]*L[i] + R[i]*R[i];
    float rms = std::sqrt(sum_sq / (float)(n * 2));

    float target_gain = 1.f;
    if (rms > 1e-6f) {
        target_gain = target_rms / rms;
        if (target_gain > 1.f) target_gain = 1.f;
        if (target_gain < gain_floor) target_gain = gain_floor;
    }
    // Rychly attack (snizovani), pomaly release (zotaveni).
    const float coeff = (target_gain < gain_) ? atk_ : rel_;
    for (int i = 0; i < n; ++i) {
        gain_ += (target_gain - gain_) * coeff;
        L[i] *= gain_;
        R[i] *= gain_;
    }
    cur_gain_.store(gain_, std::memory_order_relaxed);
}

} // namespace ithaca::dsp
```

- [ ] **Step 5: Add `engine/dsp/agc.cpp` to `CMakeLists.txt`** (`ithaca_core`, after `engine/dsp/bbe.cpp`).

- [ ] **Step 6: Build + run**

Run: `cmake --build build -j && ctest --test-dir build -R test_dsp`
Expected: PASS (AGC cases green).

- [ ] **Step 7: Commit**

```bash
git add engine/dsp/agc.h engine/dsp/agc.cpp tests/test_dsp.cpp CMakeLists.txt
git commit -m "feat(dsp): RMS AGC stage (ported from icr)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: DspChain

**Files:**
- Create: `engine/dsp/dsp_chain.h`, `engine/dsp/dsp_chain.cpp`
- Modify: `CMakeLists.txt` (add `engine/dsp/dsp_chain.cpp`)
- Test: `tests/test_dsp.cpp` (append; add `#include "dsp/dsp_chain.h"`)

- [ ] **Step 1: Write failing tests**

```cpp
TEST_CASE("DspChain: vsechny stage disabled = identita") {
    dsp::DspChain ch; ch.prepare(48000.f, 256);
    CHECK(ch.stageCount()==3);
    float L[4]={0.3f,-0.4f,0.5f,-0.6f}, R[4]={0.1f,0.2f,-0.2f,0.4f};
    float L0[4]; for(int i=0;i<4;++i) L0[i]=L[i];
    ch.process(L,R,4);
    for(int i=0;i<4;++i) CHECK(L[i]==L0[i]);
}

TEST_CASE("DspChain: poradi stage je AGC, BBE, LIMITER") {
    dsp::DspChain ch; ch.prepare(48000.f, 256);
    CHECK(std::string(ch.stage(0).name())=="AGC");
    CHECK(std::string(ch.stage(1).name())=="BBE");
    CHECK(std::string(ch.stage(2).name())=="LIMITER");
}

TEST_CASE("DspChain: enabled limiter omezi spicku") {
    dsp::DspChain ch; ch.prepare(48000.f, 4800);
    ch.stage(2).setEnabled(true);       // LIMITER
    ch.stage(2).set(0, -12.f);          // threshold
    ch.stage(2).set(1, 50.f);
    float L[4800], R[4800];
    for(int i=0;i<4800;++i){ L[i]=0.9f; R[i]=0.9f; }
    ch.process(L,R,4800);
    CHECK(std::abs(L[4799]) <= dsp::db_to_lin(-12.f) * 1.05f);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: FAIL — `'dsp_chain.h' file not found`.

- [ ] **Step 3: Create `engine/dsp/dsp_chain.h`**

```cpp
#pragma once
// engine/dsp/dsp_chain.h — pevny modularni DSP retezec: AGC -> BBE -> Limiter.
// process() spusti jen enabled() stage (disabled = no-op passthrough).
// stage(i) vraci referenci pro GUI (i=0 AGC, 1 BBE, 2 LIMITER).
#include "dsp/dsp_stage.h"
#include "dsp/agc.h"
#include "dsp/bbe.h"
#include "dsp/limiter.h"

namespace ithaca::dsp {

class DspChain {
public:
    void prepare(float sample_rate, int max_block) {
        for (auto* s : stages_) s->prepare(sample_rate, max_block);
    }
    void reset() { for (auto* s : stages_) s->reset(); }
    void process(float* L, float* R, int n) {
        for (auto* s : stages_) if (s->enabled()) s->process(L, R, n);
    }
    int stageCount() const { return 3; }
    DspStage& stage(int i) { return *stages_[i]; }

private:
    AGC     agc_;
    BBE     bbe_;
    Limiter lim_;
    DspStage* stages_[3] = { &agc_, &bbe_, &lim_ };
};

} // namespace ithaca::dsp
```

- [ ] **Step 4: Create `engine/dsp/dsp_chain.cpp`** (translation unit anchor; keeps a .o so future non-inline additions have a home and the symbol set is stable)

```cpp
#include "dsp/dsp_chain.h"
// DspChain je momentalne plne inline v hlavicce. Tento soubor existuje jako
// kotva prekladove jednotky (a misto pro budouci non-inline rozsireni, napr.
// pridani Convolveru). Zamerne prazdny krome include.
namespace ithaca::dsp { /* nic */ }
```

- [ ] **Step 5: Add `engine/dsp/dsp_chain.cpp` to `CMakeLists.txt`** (`ithaca_core`, after `engine/dsp/agc.cpp`).

- [ ] **Step 6: Build + run all DSP tests**

Run: `cmake --build build -j && ctest --test-dir build -R test_dsp`
Expected: PASS (math + Limiter + BBE + AGC + DspChain).

- [ ] **Step 7: Commit**

```bash
git add engine/dsp/dsp_chain.h engine/dsp/dsp_chain.cpp tests/test_dsp.cpp CMakeLists.txt
git commit -m "feat(dsp): DspChain (AGC->BBE->Limiter), runs enabled stages

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Wire DspChain into the engine

**Files:**
- Modify: `engine/engine.h` (add include, member, getter)
- Modify: `engine/engine.cpp` (prepare in `init`, process in `processBlock`, re-prepare in `setBlockSize`)
- Test: `tests/test_dsp.cpp` (append a wiring test; add `#include "engine.h"`)

- [ ] **Step 1: Write failing test** — verifies the engine exposes a reachable, prepared chain

```cpp
TEST_CASE("Engine vystavuje DSP chain se 3 stage") {
    ithaca::EngineConfig cfg;          // default 48k / 256, zadna banka
    ithaca::Engine eng;
    REQUIRE(eng.init(cfg));
    auto& ch = eng.dspChain();
    CHECK(ch.stageCount() == 3);
    CHECK(std::string(ch.stage(2).name()) == "LIMITER");
    ch.stage(2).setEnabled(true);
    CHECK(ch.stage(2).enabled());
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: FAIL — `eng.dspChain()` undefined.

- [ ] **Step 3: Add include + member + getter to `engine/engine.h`**

After the existing include block (after line 15 `#include "resonance/resonance_engine.h"`), add:
```cpp
#include "dsp/dsp_chain.h"
```
In the public section, after `masterPeakR()` (line 130), add:
```cpp
    // -- DSP chain (GUI ovlada stage; audio thread vola process v processBlock) --
    dsp::DspChain& dspChain() noexcept { return dsp_; }
```
In the private members, after `master_gain_` (line 148), add:
```cpp
    dsp::DspChain                     dsp_;
```

- [ ] **Step 4: Prepare the chain in `Engine::init`** — in `engine/engine.cpp`, after `master_gain_.store(...)` (line 46) and before `initialized_ = true;`:

```cpp
    dsp_.prepare((float)cfg.sample_rate, cfg.block_size);
```

- [ ] **Step 5: Insert chain processing in `Engine::processBlock`** — in `engine/engine.cpp`, the master-gain block is currently:
```cpp
    // 3. Master gain post-mix.
    float g = master_gain_.load(std::memory_order_relaxed);
    if (std::fabs(g - 1.f) > 0.001f)
        for (int i = 0; i < n_samples; ++i) { out_l[i] *= g; out_r[i] *= g; }
```
Immediately after that loop (before `// 4. Master peak meter`), add:
```cpp
    // 3b. DSP chain (AGC -> BBE -> Limiter). Disabled stage = no-op.
    dsp_.process(out_l, out_r, n_samples);
```

- [ ] **Step 6: Re-prepare on block-size change** — find `Engine::setBlockSize` (around line 234). After it commits the new `cfg_.block_size` (and before it returns), add a re-prepare so limiter/AGC/biquad coeffs track the rate (sample rate is unchanged but keep the contract explicit):
```cpp
    dsp_.prepare((float)cfg_.sample_rate, cfg_.block_size);
```
(Place this just before the `return` of the new block size. If `setBlockSize` early-returns on an invalid size before changing cfg_, do not add it on that path.)

- [ ] **Step 7: Build + run**

Run: `cmake --build build -j && ctest --test-dir build`
Expected: PASS — all tests incl. new wiring test; existing 25 still green (total 26+ cases across test_dsp).

- [ ] **Step 8: Smoke test the engine path (audio still plays, no crash)**

Run: `( ./build/ithaca-cli --help >/tmp/cli.txt 2>&1 ); echo "cli ok"; ( ./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca >/tmp/smoke.txt 2>&1 & echo $! >/tmp/pid ); sleep 3; kill $(cat /tmp/pid) 2>/dev/null && echo "gui ran ok"`
Expected: `gui ran ok` (bank loads, audio + MIDI start; DSP chain present but all stages disabled → sound identical to before).

- [ ] **Step 9: Commit**

```bash
git add engine/engine.h engine/engine.cpp tests/test_dsp.cpp
git commit -m "feat(engine): run DSP chain after master gain (disabled by default)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Persistence — schema v4 with DSP fields + migration

**Files:**
- Modify: `app/gui/persistence.h` (GuiState fields, default schema_version → 4)
- Modify: `app/gui/persistence.cpp` (loadState: accept v3+v4, defensive reads; saveState: write fields)

- [ ] **Step 1: Add fields to `GuiState` in `app/gui/persistence.h`** — change `schema_version` default to 4 and add DSP fields. Replace the struct's `schema_version` line and append before `window_x`:

```cpp
    int         schema_version    = 4;
```
and after `max_resonance_voices` (line 25), before the window fields, add:
```cpp
    // -- DSP chain (defaulty = zadna zmena chovani: vsechny stage vyplé) --
    bool  agc_enabled = false;     float agc_target = 0.15f;  float agc_release_ms = 200.f;  float agc_floor = 0.05f;
    bool  bbe_enabled = false;     float bbe_definition = 0.f; float bbe_bass = 0.f;
    bool  limiter_enabled = false; float limiter_threshold_db = 0.f; float limiter_release_ms = 200.f;
    int   config_page = 0;         // 0 = VOICE, 1 = AGC, 2 = BBE, 3 = LIMITER
```

- [ ] **Step 2: Accept v3+v4 and read DSP keys defensively in `loadState`** (`app/gui/persistence.cpp`)

Change the version guard (currently `if (s.schema_version != 3) return std::nullopt;`) to:
```cpp
        if (s.schema_version != 3 && s.schema_version != 4) return std::nullopt;
```
After the `window_h` read (line 118), add defensive DSP reads (missing keys → struct defaults, so v3 files load cleanly):
```cpp
        auto readF = [&](const char* k, float dv){ std::string v = findValue(json, k); return v.empty() ? dv : std::stof(v); };
        auto readB = [&](const char* k, bool dv){ std::string v = findValue(json, k); return v.empty() ? dv : (v == "true" || v == "1"); };
        auto readI = [&](const char* k, int dv){ std::string v = findValue(json, k); return v.empty() ? dv : std::stoi(v); };
        s.agc_enabled        = readB("agc_enabled", s.agc_enabled);
        s.agc_target         = readF("agc_target", s.agc_target);
        s.agc_release_ms     = readF("agc_release_ms", s.agc_release_ms);
        s.agc_floor          = readF("agc_floor", s.agc_floor);
        s.bbe_enabled        = readB("bbe_enabled", s.bbe_enabled);
        s.bbe_definition     = readF("bbe_definition", s.bbe_definition);
        s.bbe_bass           = readF("bbe_bass", s.bbe_bass);
        s.limiter_enabled    = readB("limiter_enabled", s.limiter_enabled);
        s.limiter_threshold_db = readF("limiter_threshold_db", s.limiter_threshold_db);
        s.limiter_release_ms = readF("limiter_release_ms", s.limiter_release_ms);
        s.config_page        = readI("config_page", s.config_page);
```
(Place these inside the existing `try { ... }` block, after the window reads.)

- [ ] **Step 3: Write DSP fields in `saveState`** (`app/gui/persistence.cpp`) — the current block ends with `window_h` written WITHOUT a trailing comma (line 149: `... window_h << "\n";`). Add a comma to `window_h` and append the DSP lines before the closing `}`:

```cpp
        f << "  \"window_h\": " << s.window_h << ",\n";
        f << "  \"agc_enabled\": "        << (s.agc_enabled ? "true" : "false") << ",\n";
        f << "  \"agc_target\": "         << s.agc_target          << ",\n";
        f << "  \"agc_release_ms\": "     << s.agc_release_ms       << ",\n";
        f << "  \"agc_floor\": "          << s.agc_floor            << ",\n";
        f << "  \"bbe_enabled\": "        << (s.bbe_enabled ? "true" : "false") << ",\n";
        f << "  \"bbe_definition\": "     << s.bbe_definition       << ",\n";
        f << "  \"bbe_bass\": "           << s.bbe_bass             << ",\n";
        f << "  \"limiter_enabled\": "    << (s.limiter_enabled ? "true" : "false") << ",\n";
        f << "  \"limiter_threshold_db\": " << s.limiter_threshold_db << ",\n";
        f << "  \"limiter_release_ms\": " << s.limiter_release_ms   << ",\n";
        f << "  \"config_page\": "        << s.config_page          << "\n";
```
(Note: `schema_version` is written from `s.schema_version`, which now defaults to 4. The literal `"\n"` — no trailing comma — must now be on the LAST line, `config_page`.)

- [ ] **Step 4: Build**

Run: `cmake --build build -j 2>&1 | tail -3`
Expected: `Built target ithaca-gui`, no errors.

- [ ] **Step 5: Verify round-trip + migration by inspection** (persistence has no unit-test harness in this repo — it lives in the `ithaca-gui` target, not `ithaca_core`; verify by file inspection, matching the existing convention)

Run:
```bash
STATE="/Users/j/Library/Application Support/ithaca-legacy/state.json"
cp "$STATE" /tmp/state_backup.json 2>/dev/null || true
( ./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca >/tmp/smoke.txt 2>&1 & echo $! >/tmp/pid ); sleep 3; kill $(cat /tmp/pid) 2>/dev/null
grep -E "schema_version|limiter_enabled|config_page" "$STATE"
```
Expected: file now shows `"schema_version": 4`, `"limiter_enabled": false`, `"config_page": 0`. The pre-existing v3 fields (bank_path, window_*) are preserved (NOT reset to defaults) — confirming migration didn't discard state.

- [ ] **Step 6: Commit**

```bash
git add app/gui/persistence.h app/gui/persistence.cpp
git commit -m "feat(gui): persist DSP params + config page (schema v4, v3-compatible)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: VoicePage adapter

**Files:**
- Create: `app/gui/voice_page.h`

VoicePage exposes the existing voice controls through `IParamPage` so the GUI renders it like any stage. It reads/writes `ctx.state` and calls existing engine setters. Header-only (holds an `AppContext&`).

- [ ] **Step 1: Create `app/gui/voice_page.h`**

```cpp
#pragma once
// app/gui/voice_page.h — "VOICE" stranka pro CONFIG prepinac. Neni soucasti
// audio chainu; je to IParamPage adapter nad existujicimi engine settery +
// GuiState. hasEnable()=false (vzdy on). MAX RESONANCE je readonly (init-only).
#include "dsp/dsp_stage.h"
#include "app_context.h"
#include <cmath>

namespace ithaca::gui {

class VoicePage : public ithaca::dsp::IParamPage {
public:
    explicit VoicePage(AppContext& ctx) : ctx_(ctx) {}

    const char* name() const override { return "VOICE"; }
    int paramCount() const override { return 5; }
    const ithaca::dsp::Param& param(int i) const override { return kParams[i]; }

    float get(int i) const override {
        switch (i) {
            case 0: return ctx_.state.master_gain_db;
            case 1: return ctx_.state.resonance_strength;
            case 2: return ctx_.state.release_ms;
            case 3: return ctx_.state.excite_decay_ms;
            default: return (float)ctx_.state.max_resonance_voices;
        }
    }
    void set(int i, float v) override {
        const auto& p = kParams[i];
        v = (v < p.min) ? p.min : (v > p.max ? p.max : v);
        switch (i) {
            case 0: ctx_.state.master_gain_db = v;
                    ctx_.engine.setMasterGain(std::pow(10.f, v / 20.f)); break;
            case 1: ctx_.state.resonance_strength = v;
                    ctx_.engine.setResonanceStrength(v); break;
            case 2: ctx_.state.release_ms = v;
                    ctx_.engine.setReleaseMs(v); break;
            case 3: ctx_.state.excite_decay_ms = v;
                    ctx_.engine.setExciteDecayMs(v); break;
            default: break;   // MAX RESONANCE readonly
        }
    }
    bool hasEnable() const override { return false; }
    bool enabled() const override { return true; }
    void setEnabled(bool) override {}
    bool meter(float&, const char*&) const override { return false; }

private:
    AppContext& ctx_;
    static constexpr ithaca::dsp::Param kParams[5] = {
        {"master_db",  "MASTER",        -60.f, 6.f,    0.f,    "%.1f dB", false},
        {"resonance",  "RESONANCE",       0.f, 1.f,    0.5f,   "%.2f",    false},
        {"release_ms", "RELEASE",        50.f, 2000.f, 200.f,  "%.0f ms", false},
        {"excite_ms",  "EXCITE DECAY",  500.f, 30000.f,5000.f, "%.0f ms", false},
        {"max_res",    "MAX RESONANCE",   1.f, 64.f,   32.f,   "%.0f",    true},
    };
};

} // namespace ithaca::gui
```

- [ ] **Step 2: Compile check** (no consumer yet; compile the GUI target to ensure the header is well-formed by temporarily including it — instead, just verify it parses by building after Task 10 wires it). For now, a static parse check:

Run: `c++ -std=c++20 -fsyntax-only -I app/gui -I engine -I third-party/imgui app/gui/voice_page.h 2>&1 | head`
Expected: no output beyond possible ImGui include warnings; no hard errors about `VoicePage`. (If include resolution is awkward standalone, defer the real check to Task 10's build — note that and proceed.)

- [ ] **Step 3: Commit**

```bash
git add app/gui/voice_page.h
git commit -m "feat(gui): VoicePage IParamPage adapter for VOICE config page

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: CONFIG selector panel

**Files:**
- Create: `app/gui/panel_config.h`, `app/gui/panel_config.cpp`

Renders the right-column list of pages with an LED (enabled) + name; clicking selects.

- [ ] **Step 1: Create `app/gui/panel_config.h`**

```cpp
#pragma once
// app/gui/panel_config.h - CONFIG selektor (pravy sloupec, misto DSP RACK).
// Seznam IParamPage* (VOICE + DSP stage); LED = enabled(); klik nastavi selected.
#include "dsp/dsp_stage.h"

namespace ithaca::gui {
struct AppContext;
// pages: pole IParamPage*, n jejich pocet, selected in/out (index vybrane stranky).
void renderConfigPanel(AppContext& ctx, ithaca::dsp::IParamPage** pages, int n, int& selected);
}
```

- [ ] **Step 2: Create `app/gui/panel_config.cpp`**

```cpp
// app/gui/panel_config.cpp - viz panel_config.h.
#include "panel_config.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "layout.h"
#include "imgui.h"
#include <string>

namespace ithaca::gui {

void renderConfigPanel(AppContext& ctx, ithaca::dsp::IParamPage** pages, int n, int& selected) {
    (void)ctx;
    using theme::Colors;
    namespace L = ithaca::gui::layout;
    ImGui::Dummy({0, 4}); ImGui::Indent(L::Dims::pad_panel);
    wdg::Eyebrow("CONFIG", Colors::silver2);
    ImGui::Dummy({0, L::Dims::row_gap});

    auto* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < n; ++i) {
        ithaca::dsp::IParamPage* p = pages[i];
        const bool sel = (i == selected);
        const bool on  = p->enabled();   // VOICE vraci true
        ImVec2 pos = ImGui::GetCursorScreenPos();
        // LED kolecko (zlate = on, tlumene = off).
        dl->AddCircleFilled({pos.x + 4.f, pos.y + 9.f}, 3.5f,
                            on ? Colors::gold : Colors::line);
        // Klikatelny radek pres celou sirku.
        const float w = ImGui::GetContentRegionAvail().x - L::Dims::pad_panel;
        ImGui::SetCursorScreenPos({pos.x + 16.f, pos.y});
        ImU32 col = sel ? Colors::gold : Colors::ink;
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(col));
        ImGui::TextUnformatted(p->name());
        ImGui::PopStyleColor();
        // Selectable pres cely radek (vizualne uz vykresleno; tohle resi klik).
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton((std::string("##cfg_") + p->name()).c_str(),
                                   {w, 20.f}))
            selected = i;
        ImGui::Dummy({0, L::Dims::row_gap_s});
    }
    ImGui::Unindent(L::Dims::pad_panel);
}

} // namespace ithaca::gui
```

- [ ] **Step 3: Build check deferred to Task 10** (panel_config.cpp must be in CMake + called; that happens in Task 10). For now just commit the files.

- [ ] **Step 4: Commit**

```bash
git add app/gui/panel_config.h app/gui/panel_config.cpp
git commit -m "feat(gui): CONFIG selector panel (LED + clickable page list)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: Generic param renderer + main wiring + delete DSP rack

**Files:**
- Modify: `app/gui/widgets.h` (add `ToggleChip`)
- Modify: `app/gui/panel_params.h`, `app/gui/panel_params.cpp` (→ `renderParamPage`)
- Modify: `app/gui/main.cpp` (page list, selection, render selected page + CONFIG)
- Modify: `app/gui/app_context.cpp` (apply persisted DSP params to chain at init)
- Modify: `CMakeLists.txt` (swap `panel_dsp.cpp` → `panel_config.cpp` in `ithaca-gui`)
- Delete: `app/gui/panel_dsp.h`, `app/gui/panel_dsp.cpp`

- [ ] **Step 1: Add `ToggleChip` to `app/gui/widgets.h`** — insert after the `Lamp` function (around line 42):

```cpp
// ON/OFF prepinac (chip). Vraci true kdyz uzivatel kliknul (volajici prepne stav).
inline bool ToggleChip(const char* id, bool on) {
    if (Fonts::eyebrow) ImGui::PushFont(Fonts::eyebrow);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(on ? Colors::gold : Colors::muted));
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colors::v(Colors::line_soft));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Colors::v(Colors::line));
    char buf[32]; std::snprintf(buf, sizeof(buf), "%s  %s##%s",
                                on ? "\xE2\x97\x8F" : "\xE2\x97\x8B",
                                on ? "ON" : "OFF", id);   // ● ON / ○ OFF
    bool clicked = ImGui::Button(buf);
    ImGui::PopStyleColor(4);
    if (Fonts::eyebrow) ImGui::PopFont();
    return clicked;
}
```

- [ ] **Step 2: Change `app/gui/panel_params.h`** to the generic signature

```cpp
// app/gui/panel_params.h - genericky renderer parametricke stranky (IParamPage).
#pragma once
#include "dsp/dsp_stage.h"
namespace ithaca::gui {
struct AppContext;
void renderParamPage(AppContext& ctx, ithaca::dsp::IParamPage& page);
}
```

- [ ] **Step 3: Rewrite `app/gui/panel_params.cpp`** as the generic renderer

```cpp
// app/gui/panel_params.cpp - genericky renderer libovolne IParamPage (VOICE i DSP
// stage). Nadpis = page.name(); volitelny ON/OFF toggle; smycka DecoSlideru pres
// parametry (readonly -> disabled); volitelny metr na konci.
#include "panel_params.h"
#include "app_context.h"
#include "theme.h"
#include "widgets.h"
#include "layout.h"
#include "imgui.h"
#include <cstdio>

namespace ithaca::gui {

void renderParamPage(AppContext& ctx, ithaca::dsp::IParamPage& page) {
    (void)ctx;
    using theme::Colors;
    namespace L = ithaca::gui::layout;
    ImGui::Dummy({0, 4}); ImGui::Indent(L::Dims::pad_panel);
    wdg::Eyebrow(page.name(), Colors::silver2);
    ImGui::Dummy({0, L::Dims::row_gap});

    if (page.hasEnable()) {
        if (wdg::ToggleChip(page.name(), page.enabled()))
            page.setEnabled(!page.enabled());
        ImGui::Dummy({0, L::Dims::row_gap});
    }

    for (int i = 0; i < page.paramCount(); ++i) {
        const auto& p = page.param(i);
        float v = page.get(i);
        const ImU32 accent = (i == 0) ? Colors::gold : Colors::silver2;
        if (wdg::DecoSlider(p.label, &v, p.min, p.max, p.fmt, accent, /*enabled=*/!p.readonly))
            page.set(i, v);
        ImGui::Dummy({0, L::Dims::row_gap});
    }

    float mv; const char* ml;
    if (page.meter(mv, ml)) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.2f", mv);
        wdg::Eyebrow(ml);
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::ink));
        ImGui::TextUnformatted(buf);
        ImGui::PopStyleColor();
    }
    ImGui::Unindent(L::Dims::pad_panel);
}

} // namespace ithaca::gui
```

- [ ] **Step 4: Wire pages + CONFIG into `app/gui/main.cpp`**

(a) Replace the includes `#include "panel_params.h"` / `#include "panel_dsp.h"` block — remove the `panel_dsp.h` line and add `panel_config.h` + `voice_page.h`:
```cpp
#include "panel_params.h"
#include "panel_config.h"
#include "voice_page.h"
```
(b) After `ctx.initFromState(st)` succeeds (after the `AppContext ctx;` init block around line 155) and before the render loop, build the page list:
```cpp
    // CONFIG stranky: VOICE (engine voice params) + 3 DSP stage z chainu.
    VoicePage voice_page(ctx);
    ithaca::dsp::IParamPage* pages[4] = {
        &voice_page,
        &ctx.engine.dspChain().stage(0),   // AGC
        &ctx.engine.dspChain().stage(1),   // BBE
        &ctx.engine.dspChain().stage(2),   // LIMITER
    };
    if (ctx.state.config_page < 0 || ctx.state.config_page > 3) ctx.state.config_page = 0;
```
(c) In the main-row layout, replace the VOICE child body and the DSP child:
Current:
```cpp
        ImGui::BeginChild("##voice", {content_w-COL1-COL3, main_h}, false); renderParamsPanel(ctx); ImGui::EndChild();
        ImGui::SameLine(0,0);
        ImGui::BeginChild("##dsp",   {COL3, main_h}, false); renderDspRack(ctx);     ImGui::EndChild();
```
Replace with:
```cpp
        ImGui::BeginChild("##voice", {content_w-COL1-COL3, main_h}, false);
            renderParamPage(ctx, *pages[ctx.state.config_page]);
        ImGui::EndChild();
        ImGui::SameLine(0,0);
        ImGui::BeginChild("##config", {COL3, main_h}, false);
            renderConfigPanel(ctx, pages, 4, ctx.state.config_page);
        ImGui::EndChild();
```

- [ ] **Step 5: Apply persisted DSP params to the chain in `app/gui/app_context.cpp`** — in `initFromState`, after `engine.init(cfg)` succeeds (after line 64, before the bank load), add:

```cpp
    // Aplikuj perzistovane DSP parametry na chain (poradi parametru = Param tabulky
    // jednotlivych stage: AGC[target,release,floor], BBE[definition,bass],
    // LIMITER[threshold_db,release_ms]).
    {
        auto& ch = engine.dspChain();
        auto& agc = ch.stage(0); auto& bbe = ch.stage(1); auto& lim = ch.stage(2);
        agc.set(0, state.agc_target); agc.set(1, state.agc_release_ms); agc.set(2, state.agc_floor);
        agc.setEnabled(state.agc_enabled);
        bbe.set(0, state.bbe_definition); bbe.set(1, state.bbe_bass);
        bbe.setEnabled(state.bbe_enabled);
        lim.set(0, state.limiter_threshold_db); lim.set(1, state.limiter_release_ms);
        lim.setEnabled(state.limiter_enabled);
    }
```

- [ ] **Step 6: Persist DSP changes during runtime** — in `app/gui/main.cpp`, the debounced change-detector (around lines 229-237) compares `last_saved` fields. Add the DSP fields so edits trigger a save. Extend the `bool changed =` expression with:
```cpp
            last_saved.agc_enabled         != ctx.state.agc_enabled ||
            last_saved.agc_target          != ctx.state.agc_target ||
            last_saved.agc_release_ms      != ctx.state.agc_release_ms ||
            last_saved.agc_floor           != ctx.state.agc_floor ||
            last_saved.bbe_enabled         != ctx.state.bbe_enabled ||
            last_saved.bbe_definition      != ctx.state.bbe_definition ||
            last_saved.bbe_bass            != ctx.state.bbe_bass ||
            last_saved.limiter_enabled     != ctx.state.limiter_enabled ||
            last_saved.limiter_threshold_db != ctx.state.limiter_threshold_db ||
            last_saved.limiter_release_ms  != ctx.state.limiter_release_ms ||
            last_saved.config_page         != ctx.state.config_page ||
```
**Important:** the DSP stages store their own values; `ctx.state` is only updated for VOICE params (VoicePage writes `ctx.state`). For DSP stages, the slider writes go to the stage, NOT `ctx.state`. So after rendering, sync the stage values back into `ctx.state` each frame so persistence sees them. After the `renderParamPage`/`renderConfigPanel` calls (after the main-row `EndChild`s), add a sync block:
```cpp
        // Zrcadli aktualni DSP stage hodnoty do ctx.state (pro persistenci).
        {
            auto& ch = ctx.engine.dspChain();
            auto& agc = ch.stage(0); auto& bbe = ch.stage(1); auto& lim = ch.stage(2);
            ctx.state.agc_enabled = agc.enabled();
            ctx.state.agc_target = agc.get(0); ctx.state.agc_release_ms = agc.get(1); ctx.state.agc_floor = agc.get(2);
            ctx.state.bbe_enabled = bbe.enabled();
            ctx.state.bbe_definition = bbe.get(0); ctx.state.bbe_bass = bbe.get(1);
            ctx.state.limiter_enabled = lim.enabled();
            ctx.state.limiter_threshold_db = lim.get(0); ctx.state.limiter_release_ms = lim.get(1);
        }
```

- [ ] **Step 7: Update `CMakeLists.txt`** — in the `ithaca-gui` `add_executable` list (lines 67-78), replace `app/gui/panel_dsp.cpp` with `app/gui/panel_config.cpp`.

- [ ] **Step 8: Delete the placeholder rack**

```bash
git rm app/gui/panel_dsp.h app/gui/panel_dsp.cpp
```

- [ ] **Step 9: Build**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: `Built target ithaca-gui`, no errors (no remaining references to `renderDspRack`/`renderParamsPanel`/`panel_dsp.h`).

- [ ] **Step 10: Smoke test the full feature**

Run: `( ./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca >/tmp/smoke.txt 2>&1 & echo $! >/tmp/pid ); sleep 3; kill $(cat /tmp/pid) 2>/dev/null && echo "ran ok"; tail -3 /tmp/smoke.txt`
Expected: `ran ok`. Manual check (user): right column shows CONFIG list VOICE/AGC/BBE/LIMITER with LEDs; clicking an item swaps the middle panel's title + sliders; toggling ON/OFF lights the LED; values persist across restart (verify `state.json`).

- [ ] **Step 11: Run full test suite (no regressions)**

Run: `ctest --test-dir build`
Expected: 100% pass.

- [ ] **Step 12: Commit**

```bash
git add app/gui/widgets.h app/gui/panel_params.h app/gui/panel_params.cpp app/gui/main.cpp app/gui/app_context.cpp CMakeLists.txt
git commit -m "feat(gui): CONFIG selector swaps generic param panel; remove DSP rack mockup

VOICE + AGC/BBE/Limiter rendered through one generic IParamPage loop; CONFIG
list (right column) selects the active page. DSP values applied at init and
mirrored back to GuiState for persistence.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (after all tasks)

- [ ] `cmake --build build -j` — clean build, no warnings about removed symbols.
- [ ] `ctest --test-dir build` — 100% pass (existing 25 + new test_dsp cases).
- [ ] GUI smoke run loads a bank, audio + MIDI start.
- [ ] Manual GUI check: CONFIG list switches the middle panel; enable toggles work; a limiter set to a low threshold audibly limits; values survive restart; with all stages off the sound is unchanged from before the feature.
- [ ] `grep -rn "renderDspRack\|renderParamsPanel\|panel_dsp" app/ engine/` → zero matches.
- [ ] `git status` clean except `imgui.ini` (never committed).

---

## Notes for the implementer

- **RT-safety:** never call `prepare()`/`reset()` from the audio thread while it is processing; they run at init / block-size change only. `set()`/`setEnabled()` from the GUI thread are atomic stores; `process()` reads atomics and recomputes coefficients locally. Do not add locks.
- **Param index order is load-bearing** for persistence apply (Task 10 Step 5) and the `i==0 ⇒ gold accent` rule. If you reorder a stage's `kParams`, update the apply/sync blocks.
- **Convolver is out of scope.** Do not add it. The chain + interfaces are built so it slots in later as a 4th stage with a matching `kParams` table and one extra entry in the GUI `pages[]` array.
- **DecoSlider** already supports the read-only mode (`enabled=false`) used by MAX RESONANCE — see `app/gui/widgets.h`.
