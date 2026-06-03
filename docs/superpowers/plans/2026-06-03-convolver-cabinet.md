# Convolver (Cabinet/Body Sim) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Přidat Convolver DSP stage (direct FIR, mono IR, MIX, první v řetězci) pro subtilní simulaci těla nástroje, s IR ze dvou zdrojů: přibalené WAV + syntetický modální generátor kalibrovaný na fyziku klavírní desky (Chabassier/RR_9530).

**Architecture:** `Convolver : DspStage` (direct time-domain FIR, fixní max ring, RT-safe 2-slot IR swap, wet/dry MIX), zařazený jako první stage. IR vektor pochází buď z mono WAV loaderu, nebo z modálního generátoru (`Σ A_n·e^(-t/τ_n)·sin(2πf_n t)`, τ=2/f_ve(f), f_ve=2e-5f²+7e-2f). GUI dropdown přes IParamPage choice-rozšíření.

**Tech Stack:** C++20, doctest/CTest, biquady nepotřeba (čistý FIR).

**Spec:** `docs/superpowers/specs/2026-06-03-convolver-cabinet-design.md`

**Build pořadí:** T1–T4 v `ithaca_core` (+ testy); T5 GUI; plný build T7.

---

### Task 1: Mono IR WAV loader

**Files:** Create `engine/dsp/ir_wav.h`, `engine/dsp/ir_wav.cpp`; Modify `CMakeLists.txt`; Test `tests/test_ir_wav.cpp` + `tests/CMakeLists.txt`

- [ ] **Step 1: Header** `engine/dsp/ir_wav.h`:
```cpp
#pragma once
// engine/dsp/ir_wav.h — nacti mono IR z WAV (float32/int16), downmix→mono,
// resample na engine_sr (linearne), orizni na max_len. Pro Convolver.
#include <string>
#include <vector>
namespace ithaca::dsp {
// Vrati true + naplni `out` (mono float, delka <= max_len). false pri chybe.
bool loadIrWavMono(const std::string& path, float engine_sr, int max_len, std::vector<float>& out);
}
```

- [ ] **Step 2: Impl** `engine/dsp/ir_wav.cpp` (port z icr `convolver.cpp::readWavMono` + resample + cap):
```cpp
#include "dsp/ir_wav.h"
#include <cstring>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <algorithm>

namespace ithaca::dsp {
namespace {
bool readWavMono(const std::string& path, std::vector<float>& out, int& sr_out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    char riff[4]; f.read(riff,4); if (std::memcmp(riff,"RIFF",4)!=0) return false;
    uint32_t fsz; f.read(reinterpret_cast<char*>(&fsz),4);
    char wave[4]; f.read(wave,4); if (std::memcmp(wave,"WAVE",4)!=0) return false;
    uint16_t fmt=0, ch=0, bps=0; uint32_t sr=0, dsz=0;
    while (f.good()) {
        char id[4]; f.read(id,4); uint32_t csz; f.read(reinterpret_cast<char*>(&csz),4);
        if (std::memcmp(id,"fmt ",4)==0) {
            f.read(reinterpret_cast<char*>(&fmt),2); f.read(reinterpret_cast<char*>(&ch),2);
            f.read(reinterpret_cast<char*>(&sr),4); f.seekg(6,std::ios::cur);
            f.read(reinterpret_cast<char*>(&bps),2);
            if (csz>16) f.seekg(csz-16,std::ios::cur);
        } else if (std::memcmp(id,"data",4)==0) { dsz=csz; break; }
        else f.seekg(csz,std::ios::cur);
    }
    if (ch==0||sr==0||dsz==0) return false;
    sr_out=(int)sr;
    if (fmt==3 && bps==32) {
        int n=dsz/(4*ch); out.resize(n);
        if (ch==1) f.read(reinterpret_cast<char*>(out.data()), n*4);
        else { std::vector<float> b(n*ch); f.read(reinterpret_cast<char*>(b.data()),dsz);
               for(int i=0;i<n;++i){float s=0;for(int c=0;c<ch;++c)s+=b[i*ch+c];out[i]=s/ch;} }
    } else if (fmt==1 && bps==16) {
        int n=dsz/(2*ch); out.resize(n);
        std::vector<int16_t> b(n*ch); f.read(reinterpret_cast<char*>(b.data()),dsz);
        for(int i=0;i<n;++i){float s=0;for(int c=0;c<ch;++c)s+=b[i*ch+c]/32768.f;out[i]=s/ch;}
    } else return false;
    return true;
}
}

bool loadIrWavMono(const std::string& path, float engine_sr, int max_len, std::vector<float>& out) {
    std::vector<float> raw; int sr=0;
    if (!readWavMono(path, raw, sr) || raw.empty()) return false;
    if (engine_sr>0.f && sr>0 && std::fabs((float)sr-engine_sr)>1.f) {
        float ratio=engine_sr/(float)sr; int nl=(int)((float)raw.size()*ratio); if(nl<2)nl=2;
        std::vector<float> rs(nl);
        for(int i=0;i<nl;++i){ float sp=(float)i/ratio; int idx=(int)sp; float fr=sp-idx;
            if(idx+1<(int)raw.size()) rs[i]=raw[idx]*(1.f-fr)+raw[idx+1]*fr;
            else if(idx<(int)raw.size()) rs[i]=raw[idx]*(1.f-fr); else rs[i]=0.f; }
        raw.swap(rs);
    }
    if ((int)raw.size()>max_len) raw.resize(max_len);
    out.swap(raw);
    return true;
}
}
```

- [ ] **Step 3: CMake + test.** Přidej `engine/dsp/ir_wav.cpp` do `ithaca_core` v `CMakeLists.txt` (k ostatním `engine/dsp/`). `tests/test_ir_wav.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "dsp/ir_wav.h"
#include <vector>
using namespace ithaca::dsp;
TEST_CASE("loadIrWavMono: chybna cesta → false") {
    std::vector<float> ir;
    CHECK_FALSE(loadIrWavMono("/tmp/ithaca_nope_xyz.wav", 48000.f, 8192, ir));
}
```
(Plnohodnotný WAV test je v T7 smoke se skutečným IR; tady jen že loader linkuje a chyba nevadí.) Registruj v `tests/CMakeLists.txt`:
```cmake
add_executable(test_ir_wav test_ir_wav.cpp)
target_link_libraries(test_ir_wav PRIVATE ithaca_core doctest)
add_test(NAME test_ir_wav COMMAND test_ir_wav)
```

- [ ] **Step 4: build + test:**
```
cmake -S . -B build >/dev/null && cmake --build build --target ithaca_core test_ir_wav 2>&1 | tail -5 && ctest --test-dir build -R test_ir_wav --output-on-failure 2>&1 | tail -4
```

- [ ] **Step 5: commit:**
```bash
git add engine/dsp/ir_wav.h engine/dsp/ir_wav.cpp CMakeLists.txt tests/test_ir_wav.cpp tests/CMakeLists.txt
git commit -m "feat(dsp): mono IR WAV loader (port from icr) + resample/cap"
```

---

### Task 2: Synthetic modal IR generator

**Files:** Create `engine/dsp/ir_modal.h`, `engine/dsp/ir_modal.cpp`; Modify `CMakeLists.txt`; Test `tests/test_ir_modal.cpp` + CMake

- [ ] **Step 1: Header** `engine/dsp/ir_modal.h`:
```cpp
#pragma once
// engine/dsp/ir_modal.h — syntetic soundboard body IR (modalni model kalibrovany
// na Chabassier/RR_9530): suma tlumenych modu f_n s decay tau=2/f_ve(f_n).
#include <vector>
namespace ithaca::dsp {
enum class IrPreset { BodySoft, BodyBright };
// Modalni tlumeni desky (RR_9530): f_ve(f) = 2e-5 f^2 + 7e-2 f. Per-mod decay tau=2/f_ve.
float soundboardDampingFve(float f);
// Vygeneruj body IR (delka <= max_len) pro engine sr.
std::vector<float> generateModalIr(IrPreset preset, float sr, int max_len);
}
```

- [ ] **Step 2: Impl** `engine/dsp/ir_modal.cpp`:
```cpp
#include "dsp/ir_modal.h"
#include <cmath>
#include <random>
#include <algorithm>
namespace ithaca::dsp {
namespace {
constexpr float kPi = 3.14159265358979f;
// spektralni obalka A(f): energie koncentrovana <~800 Hz (-12 dB/oct vyse),
// BodyBright pridava presence bump ~2.5 kHz.
float envelope(float f, IrPreset p) {
    const float lo = 800.f;
    float a = (f <= lo) ? 1.f : (lo/f)*(lo/f);   // -12 dB/oct nad 800 Hz
    if (p == IrPreset::BodyBright) {
        float g = std::exp(-std::pow((f - 2500.f)/1200.f, 2.f));
        a += 0.6f * g;
    }
    return a;
}
}
float soundboardDampingFve(float f) { return 2e-5f*f*f + 7e-2f*f; }

std::vector<float> generateModalIr(IrPreset preset, float sr, int max_len) {
    // Modalni frekvence: nizke anchory (RR_9530 mereni) + fill do ~5 kHz s mirne
    // rostoucim spacingem (vyssi hustota nahore = deska-like).
    std::vector<float> freqs = {27,42,63,86,121,164,210,260,289};
    const float fmax = 5000.f;
    for (float f = 340.f, df = 60.f; f < fmax; f += df, df *= 1.01f) freqs.push_back(f);

    int len = std::min(max_len, (int)(0.15f * sr));   // ~150 ms cap (transient body)
    if (len < 2) len = 2;
    std::vector<float> ir((size_t)len, 0.f);

    std::mt19937 rng(12345u);                          // fixni seed → reprodukovatelne
    std::uniform_real_distribution<float> phase(0.f, 2.f*kPi);
    for (float fn : freqs) {
        if (fn >= sr * 0.5f) continue;
        const float tau = 2.f / soundboardDampingFve(fn);   // s
        const float A   = envelope(fn, preset);
        const float phi = phase(rng);
        const float w   = 2.f * kPi * fn / sr;
        for (int t = 0; t < len; ++t)
            ir[(size_t)t] += A * std::exp(-(float)t/(tau*sr)) * std::sin(w*(float)t + phi);
    }
    // normalizace na unit peak
    float peak = 0.f; for (float v : ir) peak = std::max(peak, std::fabs(v));
    if (peak > 1e-9f) { const float g = 1.f/peak; for (auto& v : ir) v *= g; }
    return ir;
}
}
```

- [ ] **Step 3: CMake + test.** Přidej `engine/dsp/ir_modal.cpp` do `ithaca_core`. `tests/test_ir_modal.cpp` (verifikace proti fyzice):
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "dsp/ir_modal.h"
#include <cmath>
#include <vector>
using namespace ithaca::dsp;

TEST_CASE("damping law f_ve(f) = 2e-5 f^2 + 7e-2 f (RR_9530)") {
    CHECK(soundboardDampingFve(100.f) == doctest::Approx(2e-5f*10000.f + 7.f));   // 0.2+7=7.2
    CHECK(soundboardDampingFve(1000.f) == doctest::Approx(2e-5f*1e6f + 70.f));    // 20+70=90
    // decay tau=2/f_ve: nizke mody zni dele nez vysoke
    float tau100 = 2.f/soundboardDampingFve(100.f);
    float tau1k  = 2.f/soundboardDampingFve(1000.f);
    CHECK(tau100 > tau1k);
    CHECK(tau100 > 0.2f);    // ~0.28 s
    CHECK(tau1k  < 0.05f);   // ~0.022 s
}

TEST_CASE("generateModalIr: konecna, normalizovana, energie koncentrovana < 1 kHz") {
    auto ir = generateModalIr(IrPreset::BodySoft, 48000.f, 8192);
    REQUIRE(ir.size() > 100);
    CHECK((int)ir.size() <= 8192);
    float peak=0.f; for (float v:ir){ CHECK(std::isfinite(v)); peak=std::max(peak,std::fabs(v)); }
    CHECK(peak == doctest::Approx(1.f).epsilon(0.01));   // unit peak
    // spektralni obalka: energie v pasmu 100-800 Hz > pasmo 3-6 kHz (DFT biny)
    auto bandE = [&](float f0, float f1){ double e=0; int N=(int)ir.size();
        for (float f=f0; f<=f1; f+=50.f){ double re=0,im=0; double w=2.0*M_PI*f/48000.0;
            for (int i=0;i<N;++i){ re+=ir[(size_t)i]*std::cos(w*i); im+=ir[(size_t)i]*std::sin(w*i);} 
            e += re*re+im*im; } return e; };
    double lowE  = bandE(100.f, 800.f);
    double highE = bandE(3000.f, 6000.f);
    CHECK(lowE > highE);   // tělo koncentrovane v nizkych/strednich
}

TEST_CASE("BodyBright má víc HF energie než BodySoft") {
    auto soft = generateModalIr(IrPreset::BodySoft, 48000.f, 8192);
    auto bright = generateModalIr(IrPreset::BodyBright, 48000.f, 8192);
    auto hf=[&](const std::vector<float>& ir){ double e=0;int N=(int)ir.size();
        for(float f=2000.f;f<=4000.f;f+=50.f){double re=0,im=0,w=2.0*M_PI*f/48000.0;
            for(int i=0;i<N;++i){re+=ir[(size_t)i]*std::cos(w*i);im+=ir[(size_t)i]*std::sin(w*i);}
            e+=re*re+im*im;} return e; };
    CHECK(hf(bright) > hf(soft));
}
```
Registruj `test_ir_modal` v `tests/CMakeLists.txt` (stejný vzor).

- [ ] **Step 4: build + test:**
```
cmake -S . -B build >/dev/null && cmake --build build --target ithaca_core test_ir_modal 2>&1 | tail -6 && ctest --test-dir build -R test_ir_modal --output-on-failure 2>&1 | tail -8
```
Expected: PASS. (Pokud „lowE>highE" selže, doladit `envelope()` rolloff — NE oslabit assert; cíl je energie koncentrovaná v nízkých/středních dle papírů.)

- [ ] **Step 5: commit:**
```bash
git add engine/dsp/ir_modal.h engine/dsp/ir_modal.cpp CMakeLists.txt tests/test_ir_modal.cpp tests/CMakeLists.txt
git commit -m "feat(dsp): synthetic modal soundboard IR generator (calibrated to Chabassier/RR_9530 f_ve)"
```

---

### Task 3: Convolver DSP stage

**Files:** Create `engine/dsp/convolver.h`, `engine/dsp/convolver.cpp`; Modify `CMakeLists.txt`; Test `tests/test_convolver.cpp` + CMake

- [ ] **Step 1: Header** `engine/dsp/convolver.h`:
```cpp
#pragma once
// engine/dsp/convolver.h — Convolver (cabinet/body sim) jako DspStage.
// Direct time-domain FIR, mono IR na L/R, wet/dry MIX. Fixni max ring; RT-safe
// 2-slot IR swap (atomicky publikovany aktivni slot). Viz spec 2026-06-03.
#include "dsp/dsp_stage.h"
#include <atomic>
#include <vector>

namespace ithaca::dsp {

class Convolver : public DspStage {
public:
    static constexpr int kMaxIr = 8192;   // ~170 ms @48k

    const char* name() const override { return "CONVOLVER"; }
    void prepare(float sr, int max_block) override;
    void reset() override;

    int paramCount() const override { return 1; }
    const Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override { (void)i; return mix_.load(std::memory_order_relaxed); }
    void set(int i, float v) override { (void)i;
        v = (v < 0.f) ? 0.f : (v > 1.f ? 1.f : v);
        mix_.store(v, std::memory_order_relaxed);
    }
    bool hasEnable() const override { return true; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool on) override { enabled_.store(on, std::memory_order_relaxed); }
    bool meter(float&, const char*&) const override { return false; }

    void process(float* L, float* R, int n) override;

    // IR swap (off-RT / GUI). Zkopiruje do neaktivniho slotu, normalizuje, publikuje.
    void setIR(const std::vector<float>& ir);
    int  irLength() const;

    // GUI IR dropdown (IParamPage choice rozsireni).
    int choiceCount() const override;
    const char* choiceName(int i) const override;
    int currentChoice() const override { return cur_choice_.load(std::memory_order_relaxed); }
    void selectChoice(int i) override;   // GUI vola; nacte/vygeneruje IR a setIR
    const char* choiceLabel() const override { return "IR"; }

private:
    static const Param kParams[1];
    std::atomic<float> mix_{0.25f};
    std::atomic<bool>  enabled_{false};
    float sr_ = 48000.f;

    std::vector<float> ir_[2];                 // 2-slot IR (ping-pong)
    std::atomic<int>   active_{0};             // ktery slot je aktivni
    std::vector<float> buf_l_, buf_r_;         // history ring (fixni kMaxIr)
    int                write_pos_ = 0;

    // IR volby (WAV soubory + modalni presety) — naplneno v prepare/scan.
    std::vector<std::string> choice_names_;
    std::atomic<int>   cur_choice_{0};
};

} // namespace ithaca::dsp
```

- [ ] **Step 2: IParamPage choice-rozšíření.** V `engine/dsp/dsp_stage.h` do `struct IParamPage` přidej (s defaulty → ostatní stage nezmění):
```cpp
    // Volitelny "selektor" (napr. IR list u Convolveru). Default: zadny.
    virtual int          choiceCount() const { return 0; }
    virtual const char*  choiceName(int /*i*/) const { return ""; }
    virtual int          currentChoice() const { return -1; }
    virtual void         selectChoice(int /*i*/) {}
    virtual const char*  choiceLabel() const { return ""; }
```

- [ ] **Step 3: Impl** `engine/dsp/convolver.cpp`:
```cpp
#include "dsp/convolver.h"
#include "dsp/ir_modal.h"
#include "dsp/ir_wav.h"
#include <algorithm>
#include <cmath>

namespace ithaca::dsp {

const Param Convolver::kParams[1] = {
    {"mix", "MIX", 0.f, 1.f, 0.25f, "%.2f", false},
};

void Convolver::prepare(float sr, int /*max_block*/) {
    sr_ = sr;
    buf_l_.assign(kMaxIr, 0.f);
    buf_r_.assign(kMaxIr, 0.f);
    write_pos_ = 0;
    // Vychozi nabidka IR: modalni presety (WAV se doplni v GUI scanu adresare).
    choice_names_ = {"Body soft (modal)", "Body bright (modal)"};
    // Inicialni IR = Body soft.
    setIR(generateModalIr(IrPreset::BodySoft, sr_, kMaxIr));
    cur_choice_.store(0, std::memory_order_relaxed);
}

void Convolver::reset() {
    std::fill(buf_l_.begin(), buf_l_.end(), 0.f);
    std::fill(buf_r_.begin(), buf_r_.end(), 0.f);
    write_pos_ = 0;
}

void Convolver::setIR(const std::vector<float>& ir) {
    const int inactive = 1 - active_.load(std::memory_order_relaxed);
    std::vector<float> tmp = ir;
    if ((int)tmp.size() > kMaxIr) tmp.resize(kMaxIr);
    // normalizace na unit peak (predikovatelny wet level)
    float peak = 0.f; for (float v : tmp) peak = std::max(peak, std::fabs(v));
    if (peak > 1e-9f) { const float g = 1.f/peak; for (auto& v : tmp) v *= g; }
    ir_[(size_t)inactive] = std::move(tmp);
    active_.store(inactive, std::memory_order_release);   // publikuj
}

int Convolver::irLength() const {
    return (int)ir_[(size_t)active_.load(std::memory_order_acquire)].size();
}

void Convolver::process(float* L, float* R, int n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    const auto& ir = ir_[(size_t)active_.load(std::memory_order_acquire)];
    const int M = (int)ir.size();
    if (M == 0) return;
    const float wet = mix_.load(std::memory_order_relaxed);
    const float dry = 1.f - wet;
    for (int i = 0; i < n; ++i) {
        buf_l_[(size_t)write_pos_] = L[i];
        buf_r_[(size_t)write_pos_] = R[i];
        float oL = 0.f, oR = 0.f;
        int rp = write_pos_;
        for (int k = 0; k < M; ++k) {
            oL += ir[(size_t)k] * buf_l_[(size_t)rp];
            oR += ir[(size_t)k] * buf_r_[(size_t)rp];
            if (--rp < 0) rp = kMaxIr - 1;
        }
        L[i] = dry * L[i] + wet * oL;
        R[i] = dry * R[i] + wet * oR;
        if (++write_pos_ >= kMaxIr) write_pos_ = 0;
    }
}

int Convolver::choiceCount() const { return (int)choice_names_.size(); }
const char* Convolver::choiceName(int i) const {
    return (i >= 0 && i < (int)choice_names_.size()) ? choice_names_[(size_t)i].c_str() : "";
}
void Convolver::selectChoice(int i) {
    if (i < 0 || i >= (int)choice_names_.size()) return;
    cur_choice_.store(i, std::memory_order_relaxed);
    if (i == 0)      setIR(generateModalIr(IrPreset::BodySoft,   sr_, kMaxIr));
    else if (i == 1) setIR(generateModalIr(IrPreset::BodyBright, sr_, kMaxIr));
    else {
        // i>=2 → WAV soubor (jmeno = cesta vlozena pri scanu adresare v GUI)
        std::vector<float> ir;
        if (loadIrWavMono(choice_names_[(size_t)i], sr_, kMaxIr, ir)) setIR(ir);
    }
}

} // namespace ithaca::dsp
```
> POZN.: history ring má délku `kMaxIr`; konvoluce čte `M ≤ kMaxIr` zpětných vzorků — wrap přes `kMaxIr`. Korektní i po swapu IR (ring fixní). RT: `process` alloc-free; `setIR` (alokace) běží off-RT/GUI; aktivní slot publikován atomicky, audio čte druhý slot až po dočtení (single-writer ping-pong).

- [ ] **Step 4: CMake + test.** Přidej `engine/dsp/convolver.cpp` do `ithaca_core`. `tests/test_convolver.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "dsp/convolver.h"
#include <vector>
using namespace ithaca::dsp;

TEST_CASE("Convolver: disabled = bypass") {
    Convolver c; c.prepare(48000.f, 256); c.setEnabled(false);
    float L[3]={0.1f,0.2f,0.3f}, R[3]={0.1f,0.2f,0.3f};
    c.process(L,R,3);
    CHECK(L[0]==doctest::Approx(0.1f)); CHECK(L[2]==doctest::Approx(0.3f));
}
TEST_CASE("Convolver: identity IR + MIX=1 → passthrough") {
    Convolver c; c.prepare(48000.f, 256); c.setEnabled(true); c.set(0, 1.f);
    c.setIR(std::vector<float>{1.f});            // identity (1 tap), po normalizaci stale 1
    float L[4]={0.5f,-0.3f,0.2f,0.1f}, R[4]={0.5f,-0.3f,0.2f,0.1f};
    c.process(L,R,4);
    CHECK(L[0]==doctest::Approx(0.5f)); CHECK(L[1]==doctest::Approx(-0.3f));
}
TEST_CASE("Convolver: impulz → IR (MIX=1)") {
    Convolver c; c.prepare(48000.f, 256); c.setEnabled(true); c.set(0, 1.f);
    c.setIR(std::vector<float>{0.5f, 0.25f, -0.5f});   // normalizuje na peak 1 → {1, 0.5, -1}
    float L[4]={1.f,0.f,0.f,0.f}, R[4]={1.f,0.f,0.f,0.f};
    c.process(L,R,4);
    CHECK(L[0]==doctest::Approx(1.0f));    // ir[0]
    CHECK(L[1]==doctest::Approx(0.5f));    // ir[1]
    CHECK(L[2]==doctest::Approx(-1.0f));   // ir[2]
    CHECK(L[3]==doctest::Approx(0.f));
}
TEST_CASE("Convolver: MIX=0 = dry, choiceCount>=2 (modal presety)") {
    Convolver c; c.prepare(48000.f, 256); c.setEnabled(true); c.set(0, 0.f);
    float L[2]={0.7f,0.4f}, R[2]={0.7f,0.4f}; c.process(L,R,2);
    CHECK(L[0]==doctest::Approx(0.7f));
    CHECK(c.choiceCount() >= 2);
    CHECK(std::string(c.choiceLabel())=="IR");
    CHECK_NOTHROW(c.selectChoice(1));   // Body bright
    CHECK(c.irLength() > 0);
}
```
Registruj `test_convolver` v `tests/CMakeLists.txt`.

- [ ] **Step 5: build + test:**
```
cmake -S . -B build >/dev/null && cmake --build build --target ithaca_core test_convolver 2>&1 | tail -6 && ctest --test-dir build -R test_convolver --output-on-failure 2>&1 | tail -6
```

- [ ] **Step 6: commit:**
```bash
git add engine/dsp/convolver.h engine/dsp/convolver.cpp engine/dsp/dsp_stage.h CMakeLists.txt tests/test_convolver.cpp tests/CMakeLists.txt
git commit -m "feat(dsp): Convolver stage (direct FIR, MIX, fixed ring, 2-slot IR swap) + IParamPage choice ext"
```

---

### Task 4: Chain integration (Convolver first)

**Files:** Modify `engine/dsp/dsp_chain.h`, `engine/engine.cpp` (comment)

- [ ] **Step 1: `engine/dsp/dsp_chain.h`** — přidej Convolver jako PRVNÍ stage:
  - include `#include "dsp/convolver.h"`; komentář ř.2/4 „AGC -> ENHANCER -> Limiter" → „CONVOLVER -> AGC -> ENHANCER -> Limiter", stage indexy (0=CONVOLVER,1=AGC,2=ENHANCER,3=LIMITER).
  - člen `Convolver convolver_;` (před `agc_`).
  - `stageCount()` → 4.
  - `DspStage* stages_[4] = { &convolver_, &agc_, &enhancer_, &lim_ };`

- [ ] **Step 2: `engine/engine.cpp`** — komentář „AGC -> ENHANCER -> Limiter" (ř. ~248) → „CONVOLVER -> AGC -> ENHANCER -> Limiter".

- [ ] **Step 3: build core + DSP testy:**
```
cmake --build build --target ithaca_core test_dsp 2>&1 | tail -4 && ctest --test-dir build -R "^test_dsp$" --output-on-failure 2>&1 | tail -4
```
Expected: build OK. (test_dsp „DspChain order" testuje stage(1)==ENHANCER — teď stage(1) je AGC! Uprav ten test: `stage(0).name()=="CONVOLVER"`, `stage(1).name()=="AGC"`, `stage(2).name()=="ENHANCER"`, `stage(3).name()=="LIMITER"`; a `stageCount()==4`. Najdi v `tests/test_dsp.cpp` „DspChain: poradi stage" a aktualizuj.)
```bash
git add engine/dsp/dsp_chain.h engine/engine.cpp tests/test_dsp.cpp
git commit -m "feat(dsp): insert Convolver as first chain stage (CONVOLVER→AGC→ENHANCER→Limiter)"
```

---

### Task 5: GUI — Convolver page + IR dropdown + persistence

**Files:** Modify `app/gui/panel_params.cpp` (render choice combo), `app/gui/main.cpp` (pages), `app/gui/persistence.{h,cpp}`, `app/gui/app_context.cpp`

**Build target:** `ithaca-gui` + `test_persistence`.

- [ ] **Step 1: `panel_params.cpp`** — v `renderParamPage`, za smyčku sliderů (před meter blokem), přidej IR/choice combo:
```cpp
    if (page.choiceCount() > 0) {
        ImGui::Dummy({0, L::Dims::row_gap});
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
        ImGui::TextUnformatted(page.choiceLabel());
        ImGui::PopStyleColor();
        int cur = page.currentChoice();
        const char* curName = (cur >= 0) ? page.choiceName(cur) : "(none)";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - L::Dims::pad_panel);
        if (ImGui::BeginCombo("##choice", curName)) {
            for (int i = 0; i < page.choiceCount(); ++i)
                if (ImGui::Selectable(page.choiceName(i), i == cur) && i != cur)
                    page.selectChoice(i);
            ImGui::EndCombo();
        }
    }
```

- [ ] **Step 2: `main.cpp` pages** — Convolver je `stage(0)`. Rozšiř pole na 6 a přidej CONVOLVER (např. za RESONANCE; pořadí GUI je kosmetické, audio řetězec má convolver první):
```cpp
    ithaca::dsp::IParamPage* pages[6] = {
        &master_page,
        &resonance_page,
        &ctx.engine.dspChain().stage(0),   // CONVOLVER
        &ctx.engine.dspChain().stage(1),   // AGC
        &ctx.engine.dspChain().stage(2),   // ENHANCER
        &ctx.engine.dspChain().stage(3),   // LIMITER
    };
    if (ctx.state.config_page < 0 || ctx.state.config_page > 5) ctx.state.config_page = 0;
```
A `renderConfigPanel(ctx, pages, 6, ctx.state.config_page);` (změň 5→6).

- [ ] **Step 3: Persistence** — `persistence.h` GuiState přidej:
```cpp
    bool   convolver_enabled = false;
    float  convolver_mix     = 0.25f;
    int    convolver_choice  = 0;   // index IR volby (0=modal soft)
```
`persistence.cpp` load (defensive) + save (4 řádky se vzorem ostatních; pozor na čárky):
```cpp
        s.convolver_enabled = readB("convolver_enabled", s.convolver_enabled);
        s.convolver_mix     = readF("convolver_mix", s.convolver_mix);
        s.convolver_choice  = readI("convolver_choice", s.convolver_choice);
```
```cpp
        f << "  \"convolver_enabled\": " << (s.convolver_enabled ? "true":"false") << ",\n";
        f << "  \"convolver_mix\": "     << s.convolver_mix     << ",\n";
        f << "  \"convolver_choice\": "  << s.convolver_choice  << ",\n";
```

- [ ] **Step 4: `app_context.cpp` init** — po vytvoření chainu aplikuj convolver stav (stage(0)):
```cpp
    {
        auto& cv = engine.dspChain().stage(0);   // CONVOLVER
        cv.set(0, state.convolver_mix);
        if (state.convolver_choice > 0) cv.selectChoice(state.convolver_choice);
        cv.setEnabled(state.convolver_enabled);
    }
```
(Umísti k bloku, kde se aplikují ostatní DSP stage parametry.)

- [ ] **Step 5: main.cpp sync + debounce** — kde se synchronizují stage stavy do `ctx.state` (vedle enhancer/agc sync) přidej convolver, a do debounce `changed`:
```cpp
            auto& cv = ch.stage(0);
            ctx.state.convolver_enabled = cv.enabled();
            ctx.state.convolver_mix = cv.get(0);
            ctx.state.convolver_choice = cv.currentChoice();
```
```cpp
            last_saved.convolver_enabled != ctx.state.convolver_enabled ||
            last_saved.convolver_mix     != ctx.state.convolver_mix ||
            last_saved.convolver_choice  != ctx.state.convolver_choice ||
```

- [ ] **Step 6: build + test:**
```
cmake --build build --target ithaca-gui test_persistence 2>&1 | tail -10 && ctest --test-dir build -R test_persistence --output-on-failure 2>&1 | tail -4
```
Expected: build OK (no new warnings), test_persistence PASS.

- [ ] **Step 7: commit:**
```bash
git add app/gui/panel_params.cpp app/gui/main.cpp app/gui/persistence.h app/gui/persistence.cpp app/gui/app_context.cpp
git commit -m "feat(gui): CONVOLVER page (MIX + IR dropdown) + persistence (enabled/mix/choice)"
```

---

### Task 6: WAV IR assety + reference docs

**Files:** Create `banks/ir/` (port icr WAV), Modify `docs/reference/G-dsp.md`, `docs/reference/H-gui.md`

- [ ] **Step 1: Port WAV IR.**
```bash
mkdir -p banks/ir
cp ../icr/banks/soundboard/pl-grand-04071611-soundboard.wav banks/ir/grand-soundboard-a.wav
cp ../icr/banks/soundboard/pl-grand-04072006-soundboard.wav banks/ir/grand-soundboard-b.wav
```
(Pozn.: GUI scan adresáře `banks/ir/` a doplnění do `choice_names_` jako WAV volby je rozšíření — pro v1 stačí modální presety; přibalené WAV slouží jako budoucí volby/ukázka. Pokud chceš WAV hned v dropdownu, přidej do `Convolver::prepare` scan `banks/ir/*.wav` → `choice_names_`. Pro v1 ponech modální presety + WAV jako asset.)

- [ ] **Step 2: G-dsp.md** — přidej **Convolver** sekci: direct FIR, mono IR na L/R, MIX, první stage; IR zdroje (WAV loader + modální generátor `Σ A_n e^(-t/τ_n) sin`, τ=2/f_ve, f_ve=2e-5f²+7e-2f, obálka <1kHz, presety); 2-slot atomický swap + fixní ring; reference RR_9530/hal/Teng.

- [ ] **Step 3: H-gui.md** — CONFIG stránky: přidat CONVOLVER (MIX + IR dropdown); `config_page` 0..5; persistence `convolver_*`; IParamPage choice-rozšíření.

- [ ] **Step 4: commit:**
```bash
git add banks/ir docs/reference/G-dsp.md docs/reference/H-gui.md
git commit -m "feat(assets+docs): port soundboard IR WAVs + Convolver reference docs"
```

---

### Task 7: Full build, tests, smoke, finish

- [ ] **Step 1: Full build + suita:**
```
cmake --build build 2>&1 | tail -6 && ctest --test-dir build --output-on-failure 2>&1 | tail -12
```
Expected: build OK, všechny testy PASS (vč. test_ir_wav/test_ir_modal/test_convolver).

- [ ] **Step 2: Smoke (uživatel, `./build/ithaca-gui`):**
  - CONFIG → CONVOLVER: MIX slider + ON/OFF + IR dropdown (Body soft / Body bright).
  - Zapni, hraj klavír → subtilní „tělo"/teplo; MIX nahoru = víc; přepni Body soft/bright → slyšitelný rozdíl charakteru. Toggle ON/OFF mění barvu, ne dramaticky.
  - DSP LOAD dlaždice ukáže náklad konvoluce (~80–150 ms IR); při velmi dlouhé IR + malém bufferu může vyskočit.
  - Restart → MIX/IR/enabled persistované.
  - (Volitelně) ověř, že vysoké tóny nejsou „přebité" (Teng caveat) — default MIX 0,25 by měl být v pohodě.

- [ ] **Step 3: Finish branch** — REQUIRED SUB-SKILL: superpowers:finishing-a-development-branch (merge → main, push).

---

## Self-Review
- **Spec coverage:** direct FIR convolver → T3; mono IR L/R + MIX + first-in-chain → T3+T4; WAV loader → T1; modal generator (f_ve, envelope, presets) → T2; fixed ring + 2-slot swap → T3; GUI MIX+IR dropdown + persistence → T5; assets+docs → T6; validation (impulse/identity/mix + modal decay-vs-f/envelope) → T2+T3; finish → T7. ✓
- **Type consistency:** `Convolver`, `name()="CONVOLVER"`, `setIR`/`irLength`/`choiceCount/Name/currentChoice/selectChoice/choiceLabel`, `generateModalIr(IrPreset,sr,max)`, `soundboardDampingFve`, `loadIrWavMono`, `convolver_enabled/mix/choice`, `kMaxIr=8192`, stage(0)=CONVOLVER konzistentní napříč tasky. IParamPage choice-virtuals přidány v T3 Step2 (default 0 → ostatní stage beze změny). ✓
- **Placeholder scan:** T6 Step1 nechává WAV-dropdown scan jako volitelné rozšíření (explicitně, s návodem) — v1 jede na modálních presetech; není to placeholder logiky. Ostatní kroky mají konkrétní kód.
- **Build-green pořadí:** T1/T2/T3 core+testy; T4 chain (uprav test_dsp order); T5 gui; plný build T7. ✓
- **RT-safety:** `process` alloc-free; IR alokace v `setIR` off-RT; 2-slot atomický swap + fixní ring; verifikováno review v T7/finish.
- **Pozn. CPU:** direct konvoluce O(IR_len)/vzorek na master sběrnici; DSP LOAD metr to ukáže. Dlouhé IR → vyšší load (proto cap 8192 + subtilní default).
