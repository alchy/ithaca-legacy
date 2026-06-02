# Resonance velocity-layer selection + Gain + Config reorg — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sympatická rezonance vybírá velocity vrstvu podle uživatelského dB cíle (nearest peak RMS) místo natvrdo nejtišší, s dB gainem a enable toggle; CONFIG se přeskupí na MASTER + RESONANCE + AGC/BBE/LIMITER.

**Architecture:** Výběr vrstvy = čistá funkce `nearestSlotByRms`. `ResonanceEngine` zahodí `strength` (0..1) za `gain_lin` + `layer_target_db` + `enabled` (atomiky). Engine façada cachuje min/max peak RMS banky pro dynamický rozsah GUI slideru. GUI `VoicePage` → `MasterPage` + `ResonancePage`.

**Tech Stack:** C++20, Dear ImGui, miniaudio, doctest/CTest.

**Spec:** `docs/superpowers/specs/2026-06-02-resonance-velocity-layer-design.md`

**Build pozn.:** Refaktor je cross-cutting (ruší `resonance_strength`). Tasky jsou seřazené tak, aby každý buildil **svůj target** zeleně; plný build všech targetů je až v posledním tasku. Stavějte target uvedený v každém tasku, ne `cmake --build build` celé, dokud nedojdete na konec.

---

### Task 1: Čistá funkce `nearestSlotByRms`

**Files:**
- Create: `engine/resonance/resonance_layer_select.h`, `engine/resonance/resonance_layer_select.cpp`
- Create: `tests/test_resonance_layer_select.cpp`
- Modify: `CMakeLists.txt` (přidat .cpp do `ithaca_core`), `tests/CMakeLists.txt` (nový test)

- [ ] **Step 1: Hlavička** — `engine/resonance/resonance_layer_select.h`:

```cpp
#pragma once
// engine/resonance/resonance_layer_select.h
// Vyber velocity slotu pro sympatickou rezonanci: vrati index slotu, jehoz
// rms_db je nejbliz cilove hodnote target_db. Pri shode vzdalenosti nizsi index.
// Prazdne slots → -1. Cista funkce (testovatelna bez enginu).
#include "sample/sample_types.h"

namespace ithaca {
int nearestSlotByRms(const NoteSlots& ns, float target_db);
}
```

- [ ] **Step 2: Failing test** — `tests/test_resonance_layer_select.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "resonance/resonance_layer_select.h"
using namespace ithaca;

static NoteSlots makeNote(std::initializer_list<float> rms) {
    NoteSlots ns; ns.recorded = true;
    for (float v : rms) { VelocitySlot s; s.rms_db = v; ns.slots.push_back(s); }
    return ns;
}

TEST_CASE("nearestSlotByRms vybere nejblizsi rms_db") {
    NoteSlots ns = makeNote({-40.f, -25.f, -10.f});
    CHECK(nearestSlotByRms(ns, -25.f) == 1);
    CHECK(nearestSlotByRms(ns, -38.f) == 0);   // bliz -40
    CHECK(nearestSlotByRms(ns, -5.f)  == 2);   // bliz -10
    CHECK(nearestSlotByRms(ns, -1.f)  == 2);   // nad max → krajni
    CHECK(nearestSlotByRms(ns, -100.f)== 0);   // pod min → krajni
}
TEST_CASE("shoda vzdalenosti → nizsi index") {
    NoteSlots ns = makeNote({-30.f, -20.f});   // target -25 je presne mezi
    CHECK(nearestSlotByRms(ns, -25.f) == 0);
}
TEST_CASE("prazdne / jednoslotove") {
    NoteSlots empty; empty.recorded = true;
    CHECK(nearestSlotByRms(empty, -25.f) == -1);
    NoteSlots one = makeNote({-12.f});
    CHECK(nearestSlotByRms(one, -25.f) == 0);
    CHECK(nearestSlotByRms(one, 0.f)   == 0);
}
```

- [ ] **Step 3: Registrace v CMake.** V `CMakeLists.txt` za řádek `engine/resonance/harmonic_proximity.cpp` (ř. 39) přidej:
```cmake
    engine/resonance/resonance_layer_select.cpp
```
V `tests/CMakeLists.txt` přidej (vedle ostatních test targetů):
```cmake
add_executable(test_resonance_layer_select test_resonance_layer_select.cpp)
target_link_libraries(test_resonance_layer_select PRIVATE ithaca_core doctest)
add_test(NAME test_resonance_layer_select COMMAND test_resonance_layer_select)
```

- [ ] **Step 4: Reconfigure + build → FAIL** (chybí .cpp impl / linker error):
```
cmake -S . -B build >/dev/null && cmake --build build --target test_resonance_layer_select 2>&1 | tail -15
```
Expected: link/compile error (`nearestSlotByRms` nedefinováno).

- [ ] **Step 5: Implementace** — `engine/resonance/resonance_layer_select.cpp`:

```cpp
// engine/resonance/resonance_layer_select.cpp — viz .h
#include "resonance/resonance_layer_select.h"
#include <cmath>

namespace ithaca {
int nearestSlotByRms(const NoteSlots& ns, float target_db) {
    int best = -1;
    float bestd = 1e30f;
    for (size_t i = 0; i < ns.slots.size(); ++i) {
        const float d = std::fabs(ns.slots[i].rms_db - target_db);
        if (d < bestd) { bestd = d; best = (int)i; }   // strict < → nizsi index pri shode
    }
    return best;
}
}
```

- [ ] **Step 6: Build + test → PASS:**
```
cmake --build build --target test_resonance_layer_select 2>&1 | tail -3 && ctest --test-dir build -R test_resonance_layer_select --output-on-failure 2>&1 | tail -4
```
Expected: PASS.

- [ ] **Step 7: Commit:**
```bash
git add engine/resonance/resonance_layer_select.h engine/resonance/resonance_layer_select.cpp tests/test_resonance_layer_select.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(resonance): nearestSlotByRms — pure velocity-layer selector by target dB"
```

---

### Task 2: ResonanceEngine + Engine façade — gain/layer/enabled místo strength

**Files:**
- Modify: `engine/resonance/resonance_engine.h`, `engine/resonance/resonance_engine.cpp`
- Modify: `engine/engine.h`, `engine/engine.cpp`
- Modify: `tests/test_resonance_engine.cpp`, `tests/test_engine_diagnostics.cpp`, `tests/test_resonance_stream_sr.cpp`

**Build target:** `ithaca_core` + engine testy (NE celý build — GUI/CLI se opraví v Task 3–5).

- [ ] **Step 1: ResonanceEngine.h — nahradit strength API.** V `engine/resonance/resonance_engine.h`:

Nahraď deklarace (ř. ~67–69):
```cpp
    // Sila rezonance (0..1). Realtime-safe; cte se v onPlayedNoteOn.
    void  setStrength(float s);
    float strength() const;
```
za:
```cpp
    // Linearni gain rezonance (z dB). Realtime-safe; cte se v onPlayedNoteOn.
    void  setGainDb(float db);
    float gainLinear() const;
    // Cilove dB pro vyber velocity vrstvy (nearestSlotByRms). Realtime-safe.
    void  setLayerTargetDb(float db);
    float layerTargetDb() const;
    // Zapnuti/vypnuti sympaticke rezonance (onPlayedNoteOn early-return kdyz off).
    void  setEnabled(bool on);
    bool  enabled() const;
```
Nahraď member (ř. ~125):
```cpp
    std::atomic<float>  strength_{0.5f};
```
za:
```cpp
    std::atomic<float>  gain_lin_{0.251f};       // ~ -12 dB default
    std::atomic<float>  layer_target_db_{-30.f};
    std::atomic<bool>   enabled_{true};
```

- [ ] **Step 2: ResonanceEngine.cpp — settery + onPlayedNoteOn.** V `engine/resonance/resonance_engine.cpp`:

Přidej include nahoře:
```cpp
#include "resonance/resonance_layer_select.h"
#include <cmath>
```
Nahraď `setStrength`/`strength` (ř. ~55–63):
```cpp
void ResonanceEngine::setGainDb(float db) {
    gain_lin_.store(std::pow(10.f, db / 20.f), std::memory_order_relaxed);
}
float ResonanceEngine::gainLinear() const {
    return gain_lin_.load(std::memory_order_relaxed);
}
void ResonanceEngine::setLayerTargetDb(float db) {
    layer_target_db_.store(db, std::memory_order_relaxed);
}
float ResonanceEngine::layerTargetDb() const {
    return layer_target_db_.load(std::memory_order_relaxed);
}
void ResonanceEngine::setEnabled(bool on) {
    enabled_.store(on, std::memory_order_relaxed);
}
bool ResonanceEngine::enabled() const {
    return enabled_.load(std::memory_order_relaxed);
}
```
V `onPlayedNoteOn` nahraď úvod (ř. ~89–93):
```cpp
    const float str = strength_.load(std::memory_order_relaxed);
    const float vel_norm = (float)velocity / 127.f;
    if (vel_norm <= 0.f || str <= 0.f) return;
```
za:
```cpp
    if (!enabled_.load(std::memory_order_relaxed)) return;
    const float gain = gain_lin_.load(std::memory_order_relaxed);
    const float vel_norm = (float)velocity / 127.f;
    if (vel_norm <= 0.f) return;
```
Nahraď výpočet excite (ř. ~111):
```cpp
        const float excite = vel_norm * harm * str;
```
za:
```cpp
        const float excite = vel_norm * harm * gain;
```
Nahraď výběr slotu (ř. ~130–138):
```cpp
        const NoteSlots& ns = bank.notes[N];
        if (!ns.recorded || ns.slots.empty() ||
            ns.slots[0].variants.empty() ||
            ns.slots[0].variants[0].mics.empty()) {
            continue;  // nahravka pro N chybi — rezonance neni z ceho hrat
        }
        const SampleAsset& a = ns.slots[0].variants[0];
        const MicLayer*    m = &a.mics[0];
```
za:
```cpp
        const NoteSlots& ns = bank.notes[N];
        if (!ns.recorded) continue;
        const int si = nearestSlotByRms(ns, layer_target_db_.load(std::memory_order_relaxed));
        if (si < 0) continue;
        const VelocitySlot& vs = ns.slots[(size_t)si];
        if (vs.variants.empty() || vs.variants[0].mics.empty()) continue;  // nahravka chybi
        const SampleAsset& a = vs.variants[0];
        const MicLayer*    m = &a.mics[0];
```

- [ ] **Step 3: Engine.h — cfg pole + settery + getters + members.** V `engine/engine.h`:

V `EngineConfig` nahraď (ř. ~47):
```cpp
    float resonance_strength    = 0.5f;   // 0..1, expose pres CLI
```
za:
```cpp
    bool  resonance_enabled     = true;
    float resonance_gain_db     = -12.f;  // dB; expose pres CLI/GUI
    float resonance_layer_db    = -30.f;  // dB cil pro vyber velocity vrstvy
```
Nahraď setter (ř. ~137):
```cpp
    void setResonanceStrength(float s) noexcept;   // wrap resonance_->setStrength
```
za:
```cpp
    void setResonanceGainDb(float db) noexcept;
    void setResonanceLayerDb(float db) noexcept;
    void setResonanceEnabled(bool on) noexcept;
    // Min/max peak RMS [dB] napric nactenou bankou (pro dynamicky rozsah GUI
    // slideru "Resonance Layer"). Bez banky default -60 / 0.
    float bankPeakRmsMinDb() const noexcept { return bank_peak_rms_min_db_; }
    float bankPeakRmsMaxDb() const noexcept { return bank_peak_rms_max_db_; }
```
Do private členů (vedle `bank_`) přidej:
```cpp
    float bank_peak_rms_min_db_ = -60.f;
    float bank_peak_rms_max_db_ =   0.f;
```

- [ ] **Step 4: Engine.cpp — init, settery, loadBank cache.** V `engine/engine.cpp`:

Nahraď v `init` (ř. ~43):
```cpp
    resonance_->setStrength(cfg.resonance_strength);
```
za:
```cpp
    resonance_->setGainDb(cfg.resonance_gain_db);
    resonance_->setLayerTargetDb(cfg.resonance_layer_db);
    resonance_->setEnabled(cfg.resonance_enabled);
```
Nahraď `setResonanceStrength` (ř. ~365–367):
```cpp
void Engine::setResonanceStrength(float s) noexcept {
    // ResonanceEngine::setStrength si sama clampuje 0..1 a uklada atomic.
    if (resonance_) resonance_->setStrength(s);
}
```
za:
```cpp
void Engine::setResonanceGainDb(float db) noexcept {
    if (resonance_) resonance_->setGainDb(db);
}
void Engine::setResonanceLayerDb(float db) noexcept {
    if (resonance_) resonance_->setLayerTargetDb(db);
}
void Engine::setResonanceEnabled(bool on) noexcept {
    if (resonance_) resonance_->setEnabled(on);
}
```
V `Engine::loadBank`, **na konci po úspěšném načtení** (po tom, co je `bank_` naplněná, před `return true;`), přidej scan:
```cpp
    // Cache min/max peak RMS napric vsemi velocity sloty (pro GUI slider rozsah).
    {
        float mn = 1e30f, mx = -1e30f;
        for (int n = 0; n < 128; ++n)
            for (const auto& s : bank_.notes[n].slots) {
                if (s.rms_db < mn) mn = s.rms_db;
                if (s.rms_db > mx) mx = s.rms_db;
            }
        if (mn <= mx) { bank_peak_rms_min_db_ = mn; bank_peak_rms_max_db_ = mx; }
    }
```

- [ ] **Step 5: Update test_resonance_engine.cpp.** Nahraď VŠECHNY výskyty `res.setStrength(1.f);` (ř. 86,138,183,214) za:
```cpp
    res.setEnabled(true); res.setGainDb(0.f);   // gain_lin = 1.0 (jako drive strength 1.0)
```

- [ ] **Step 6: Update test_engine_diagnostics.cpp.** Nahraď `cfg.resonance_strength = 0.5f;` (ř. 47) za `cfg.resonance_gain_db = -12.f;`. Nahraď řádky se `setResonanceStrength` (ř. 53,62,63) — `CHECK_NOTHROW(e.setResonanceStrength(0.8f));` → `CHECK_NOTHROW(e.setResonanceGainDb(-6.f));`, `CHECK_NOTHROW(e.setResonanceStrength(-1.f));` → `CHECK_NOTHROW(e.setResonanceEnabled(false));`, `CHECK_NOTHROW(e.setResonanceStrength(2.f));` → `CHECK_NOTHROW(e.setResonanceLayerDb(-25.f));`. Uprav komentář na ř. 60 (zmínka o setStrength) na nové settery.

- [ ] **Step 7: Update test_resonance_stream_sr.cpp.** Nahraď `cfg.resonance_strength  = 0.5f;` (ř. 85,175) za `cfg.resonance_gain_db = 0.f;` (gain_lin=1.0). Uprav komentář ř. 85 (`excite = 1.0*0.70*0.5 = 0.35`) na `excite = 1.0*0.70*1.0 = 0.70`. POZN.: pokud test závisí na konkrétní hodnotě excite/počtu underrunů, ověř po buildu, že stále prochází; fixtura má 1 slot na notu → výběr vrstvy beze změny (nearest = slot 0).

- [ ] **Step 8: Reconfigure + build core + engine testy → PASS:**
```
cmake -S . -B build >/dev/null && cmake --build build --target ithaca_core test_resonance_engine test_engine_diagnostics test_resonance_stream_sr 2>&1 | tail -8
ctest --test-dir build -R "test_resonance_engine|test_engine_diagnostics|test_resonance_stream_sr|test_resonance_layer_select" --output-on-failure 2>&1 | tail -8
```
Expected: build OK, všechny uvedené testy PASS.

- [ ] **Step 9: Commit:**
```bash
git add engine/ tests/test_resonance_engine.cpp tests/test_engine_diagnostics.cpp tests/test_resonance_stream_sr.cpp
git commit -m "feat(resonance): replace strength(0..1) with gain dB + layer-target dB + enable; bank RMS range cache"
```

---

### Task 3: CLI flagy `--resonance` (gain) + `--resonance-layer`

**Files:**
- Modify: `app/cli/main.cpp`

**Build target:** `ithaca-cli`.

- [ ] **Step 1: Lokální proměnné + parsing.** V `app/cli/main.cpp` nahraď deklaraci (ř. ~87) `float resonance_strength = 0.5f;` za:
```cpp
    float resonance_gain_db  = -12.f;  // faze 5/8: gain sympaticke rezonance [dB]
    float resonance_layer_db = -30.f;  // faze 8: cilove dB pro vyber velocity vrstvy
```
Nahraď blok parsování `--resonance` (ř. ~121–123, clamp 0..1) za:
```cpp
            resonance_gain_db = (float)std::atof(argv[++i]);
            if (resonance_gain_db >  0.f)  resonance_gain_db = 0.f;
            if (resonance_gain_db < -60.f) resonance_gain_db = -60.f;
        } else if (std::strcmp(argv[i], "--resonance-layer") == 0 && i + 1 < argc) {
            resonance_layer_db = (float)std::atof(argv[++i]);
```
(Pozn.: ujisti se, že nový `else if` navazuje na existující `else if` řetěz — zkontroluj okolní strukturu a vlož konzistentně. Pokud původní větev nekončila `}`, uprav tak, aby nový `else if` byl validní.)

- [ ] **Step 2: cfg přiřazení.** Nahraď obě místa `cfg.resonance_strength = resonance_strength;` (ř. ~163, ~252) za:
```cpp
        cfg.resonance_gain_db  = resonance_gain_db;
        cfg.resonance_layer_db = resonance_layer_db;
```

- [ ] **Step 3: Help text.** Najdi help/usage výpis (kde se vypisují flagy jako `--resonance`) a uprav popis `--resonance` na „<dB> gain sympatické rezonance (default -12)" a přidej řádek `--resonance-layer <dB>  cilove dB pro vyber velocity vrstvy (default -30)`. (Pokud `--resonance` v helpu není, jen přidej oba.)

- [ ] **Step 4: Build → PASS:**
```
cmake --build build --target ithaca-cli 2>&1 | tail -6
```
Expected: success, no new warnings.

- [ ] **Step 5: Commit:**
```bash
git add app/cli/main.cpp
git commit -m "feat(cli): --resonance now dB gain + new --resonance-layer dB"
```

---

### Task 4: Persistence — resonance_enabled/gain_db/layer_db

**Files:**
- Modify: `app/gui/persistence.h`, `app/gui/persistence.cpp`
- Modify: `tests/test_persistence.cpp`

**Build target:** `test_persistence`.

- [ ] **Step 1: GuiState.** V `app/gui/persistence.h` nahraď `float resonance_strength  = 0.5f;` (ř. 22) za:
```cpp
    bool  resonance_enabled  = true;
    float resonance_gain_db  = -12.f;
    float resonance_layer_db = -30.f;
```

- [ ] **Step 2: loadState.** V `app/gui/persistence.cpp` odstraň required read (ř. 111):
```cpp
        s.resonance_strength    = std::stof(findValue(json, "resonance_strength"));
```
a do defensive sekce (vedle `readF/readB/readI`, za `s.config_page = readI(...)`) přidej:
```cpp
        s.resonance_enabled  = readB("resonance_enabled", s.resonance_enabled);
        s.resonance_gain_db  = readF("resonance_gain_db", s.resonance_gain_db);
        s.resonance_layer_db = readF("resonance_layer_db", s.resonance_layer_db);
```

- [ ] **Step 3: saveState.** Odstraň řádek (ř. ~161) `f << "  \"resonance_strength\": " << s.resonance_strength << ",\n";` a přidej (v bloku ostatních klíčů, všechny s `,\n` — žádný z nich není poslední):
```cpp
        f << "  \"resonance_enabled\": "  << (s.resonance_enabled ? "true" : "false") << ",\n";
        f << "  \"resonance_gain_db\": "  << s.resonance_gain_db  << ",\n";
        f << "  \"resonance_layer_db\": " << s.resonance_layer_db << ",\n";
```

- [ ] **Step 4: Update test_persistence.cpp.** Nahraď `s.resonance_strength  = 0.7f;` (ř. 16) za:
```cpp
    s.resonance_enabled  = false;
    s.resonance_gain_db  = -9.5f;
    s.resonance_layer_db = -22.f;
```
a `CHECK(loaded->resonance_strength == doctest::Approx(s.resonance_strength));` (ř. 30) za:
```cpp
    CHECK(loaded->resonance_enabled  == s.resonance_enabled);
    CHECK(loaded->resonance_gain_db  == doctest::Approx(s.resonance_gain_db));
    CHECK(loaded->resonance_layer_db == doctest::Approx(s.resonance_layer_db));
```

- [ ] **Step 5: Build + test → PASS:**
```
cmake --build build --target test_persistence 2>&1 | tail -4 && ctest --test-dir build -R test_persistence --output-on-failure 2>&1 | tail -4
```
Expected: PASS.

- [ ] **Step 6: Commit:**
```bash
git add app/gui/persistence.h app/gui/persistence.cpp tests/test_persistence.cpp
git commit -m "feat(gui): persist resonance_enabled/gain_db/layer_db (drop resonance_strength)"
```

---

### Task 5: GUI — MasterPage + ResonancePage + reorg

**Files:**
- Create: `app/gui/master_page.h`, `app/gui/resonance_page.h`
- Delete: `app/gui/voice_page.h`
- Modify: `app/gui/main.cpp`, `app/gui/app_context.cpp`, `app/gui/panel_topbar.cpp`

**Build target:** `ithaca-gui`.

- [ ] **Step 1: MasterPage** — `app/gui/master_page.h`:

```cpp
#pragma once
// app/gui/master_page.h — "MASTER" CONFIG stranka: MASTER gain + RELEASE.
// IParamPage adapter nad engine settery + GuiState. hasEnable()=false.
#include "dsp/dsp_stage.h"
#include "app_context.h"
#include <cmath>

namespace ithaca::gui {

class MasterPage : public ithaca::dsp::IParamPage {
public:
    explicit MasterPage(AppContext& ctx) : ctx_(ctx) {}
    const char* name() const override { return "MASTER"; }
    int paramCount() const override { return 2; }
    const ithaca::dsp::Param& param(int i) const override { return kParams[i]; }
    float get(int i) const override {
        return i == 0 ? ctx_.state.master_gain_db : ctx_.state.release_ms;
    }
    void set(int i, float v) override {
        const auto& p = kParams[i];
        v = (v < p.min) ? p.min : (v > p.max ? p.max : v);
        if (i == 0) { ctx_.state.master_gain_db = v;
                      ctx_.engine.setMasterGain(std::pow(10.f, v / 20.f)); }
        else        { ctx_.state.release_ms = v; ctx_.engine.setReleaseMs(v); }
    }
    bool hasEnable() const override { return false; }
    bool enabled() const override { return true; }
    void setEnabled(bool) override {}
    bool meter(float&, const char*&) const override { return false; }
private:
    AppContext& ctx_;
    static constexpr ithaca::dsp::Param kParams[2] = {
        {"master_db",  "MASTER",  -60.f, 6.f,    0.f,   "%.1f dB", false},
        {"release_ms", "RELEASE",  50.f, 2000.f, 200.f, "%.0f ms", false},
    };
};

} // namespace ithaca::gui
```

- [ ] **Step 2: ResonancePage** — `app/gui/resonance_page.h` (dynamický rozsah LAYER přes `mutable` Param, enable toggle):

```cpp
#pragma once
// app/gui/resonance_page.h — "RESONANCE" CONFIG stranka: Resonance Layer (dB,
// dynamicky rozsah z banky) + Resonance Gain (dB) + Excite Decay + Max Resonance.
// hasEnable()=true → enabled mapuje na state.resonance_enabled + engine.
#include "dsp/dsp_stage.h"
#include "app_context.h"
#include <algorithm>

namespace ithaca::gui {

class ResonancePage : public ithaca::dsp::IParamPage {
public:
    explicit ResonancePage(AppContext& ctx) : ctx_(ctx) {}
    const char* name() const override { return "RESONANCE"; }
    int paramCount() const override { return 4; }

    const ithaca::dsp::Param& param(int i) const override {
        if (i == 0) {   // RESONANCE LAYER — dynamicky rozsah z banky
            float lo = ctx_.engine.bankPeakRmsMinDb();
            float hi = ctx_.engine.bankPeakRmsMaxDb();
            if (hi <= lo) hi = lo + 1.f;   // degenerace (1 vrstva / bez banky)
            layer_param_.min = lo; layer_param_.max = hi;
        }
        return i == 0 ? layer_param_ : kParams[i - 1];
    }
    float get(int i) const override {
        switch (i) {
            case 0: {  // clamp persistovane do aktualniho rozsahu banky
                float lo = ctx_.engine.bankPeakRmsMinDb(), hi = ctx_.engine.bankPeakRmsMaxDb();
                if (hi < lo) hi = lo;
                return std::clamp(ctx_.state.resonance_layer_db, lo, hi);
            }
            case 1: return ctx_.state.resonance_gain_db;
            case 2: return ctx_.state.excite_decay_ms;
            default: return (float)ctx_.state.max_resonance_voices;
        }
    }
    void set(int i, float v) override {
        switch (i) {
            case 0: ctx_.state.resonance_layer_db = v;
                    ctx_.engine.setResonanceLayerDb(v); break;
            case 1: { const auto& p = kParams[0];
                      v = std::clamp(v, p.min, p.max);
                      ctx_.state.resonance_gain_db = v;
                      ctx_.engine.setResonanceGainDb(v); } break;
            case 2: { const auto& p = kParams[1];
                      v = std::clamp(v, p.min, p.max);
                      ctx_.state.excite_decay_ms = v;
                      ctx_.engine.setExciteDecayMs(v); } break;
            default: { const auto& p = kParams[2];
                       v = std::clamp(v, p.min, p.max);
                       ctx_.state.max_resonance_voices = (int)v;
                       ctx_.engine.setMaxResonanceVoices((int)v); } break;
        }
    }
    bool hasEnable() const override { return true; }
    bool enabled() const override { return ctx_.state.resonance_enabled; }
    void setEnabled(bool on) override {
        ctx_.state.resonance_enabled = on;
        ctx_.engine.setResonanceEnabled(on);
    }
    bool meter(float&, const char*&) const override { return false; }
private:
    AppContext& ctx_;
    mutable ithaca::dsp::Param layer_param_ =
        {"reso_layer_db", "RESONANCE LAYER", -60.f, 0.f, -30.f, "%.1f dB", false};
    // index 0 = GAIN, 1 = EXCITE DECAY, 2 = MAX RESONANCE (mapovani v get/set: i-1)
    static constexpr ithaca::dsp::Param kParams[3] = {
        {"reso_gain_db", "RESONANCE GAIN", -60.f, 0.f,     -12.f,  "%.1f dB", false},
        {"excite_ms",    "EXCITE DECAY",   500.f, 30000.f, 5000.f, "%.0f ms", false},
        {"max_res",      "MAX RESONANCE",    1.f, 64.f,    32.f,   "%.0f",    false},
    };
};

} // namespace ithaca::gui
```

- [ ] **Step 3: main.cpp — pole stránek 5 + clamp + debounce.** V `app/gui/main.cpp`:

Nahraď include `voice_page.h` (pokud je) za `#include "master_page.h"` a `#include "resonance_page.h"`. Nahraď blok (ř. ~180–187):
```cpp
    VoicePage voice_page(ctx);
    ithaca::dsp::IParamPage* pages[4] = {
        &voice_page,
        &ctx.engine.dspChain().stage(0),   // AGC
        &ctx.engine.dspChain().stage(1),   // BBE
        &ctx.engine.dspChain().stage(2),   // LIMITER
    };
    if (ctx.state.config_page < 0 || ctx.state.config_page > 3) ctx.state.config_page = 0;
```
za:
```cpp
    MasterPage    master_page(ctx);
    ResonancePage resonance_page(ctx);
    ithaca::dsp::IParamPage* pages[5] = {
        &master_page,
        &resonance_page,
        &ctx.engine.dspChain().stage(0),   // AGC
        &ctx.engine.dspChain().stage(1),   // BBE
        &ctx.engine.dspChain().stage(2),   // LIMITER
    };
    if (ctx.state.config_page < 0 || ctx.state.config_page > 4) ctx.state.config_page = 0;
```
Najdi `renderConfigPanel(ctx, pages, 4, ctx.state.config_page);` (ř. ~246) a změň `4` → `5`.
V debounce bloku nahraď řádek `last_saved.resonance_strength != ctx.state.resonance_strength ||` (ř. ~274) za:
```cpp
            last_saved.resonance_enabled   != ctx.state.resonance_enabled ||
            last_saved.resonance_gain_db   != ctx.state.resonance_gain_db ||
            last_saved.resonance_layer_db  != ctx.state.resonance_layer_db ||
```

- [ ] **Step 4: app_context.cpp — init nových parametrů + default layer 1/3.** V `app/gui/app_context.cpp`:

Nahraď `cfg.resonance_strength   = state.resonance_strength;` (ř. ~61) za:
```cpp
    cfg.resonance_enabled    = state.resonance_enabled;
    cfg.resonance_gain_db    = state.resonance_gain_db;
    cfg.resonance_layer_db   = state.resonance_layer_db;
```
Za blok DSP aplikace / `setMaxResonanceVoices` (po `engine.setMaxResonanceVoices(...)`, ř. ~81) přidej explicitní aplikaci + default layer dle banky:
```cpp
    engine.setResonanceEnabled(state.resonance_enabled);
    engine.setResonanceGainDb(state.resonance_gain_db);
```
A AŽ PO `engine.loadBank(...)` (tj. za blokem bank load, ř. ~85–90) přidej:
```cpp
    // Default Resonance Layer = 1/3 rozsahu banky, kdyz uzivatel jeste nenastavil
    // (heuristika: persistovany default -30 dB). Jinak respektuj ulozenou hodnotu.
    if (!state.bank_path.empty()) {
        const float lo = engine.bankPeakRmsMinDb(), hi = engine.bankPeakRmsMaxDb();
        if (hi > lo && state.resonance_layer_db == -30.f)
            state.resonance_layer_db = lo + (hi - lo) / 3.f;
    }
    engine.setResonanceLayerDb(state.resonance_layer_db);
```

- [ ] **Step 5: panel_topbar.cpp — RESET.** V `app/gui/panel_topbar.cpp` nahraď v RESET bloku řádky (ř. ~146, ~150):
```cpp
        ctx.state.resonance_strength = 0.5f;
```
a
```cpp
        ctx.engine.setResonanceStrength(0.5f);
```
za:
```cpp
        ctx.state.resonance_enabled  = true;
        ctx.state.resonance_gain_db  = -12.f;
```
resp.
```cpp
        ctx.engine.setResonanceEnabled(true);
        ctx.engine.setResonanceGainDb(-12.f);
```
(RESET ponech `release_ms`, `excite_decay_ms`, `master_gain_db` beze změny. `resonance_layer_db` RESET nemění — je vázán na banku.)

- [ ] **Step 6: Smaž voice_page.h:**
```bash
git rm app/gui/voice_page.h
```

- [ ] **Step 7: Build → PASS:**
```
cmake --build build --target ithaca-gui 2>&1 | tail -15
```
Expected: success, no new warnings. (Pokud někde zůstal odkaz na `VoicePage`/`voice_page.h`/`resonance_strength` v GUI, oprav dle chyby.)

- [ ] **Step 8: Commit:**
```bash
git add app/gui/master_page.h app/gui/resonance_page.h app/gui/main.cpp app/gui/app_context.cpp app/gui/panel_topbar.cpp
git commit -m "feat(gui): MASTER + RESONANCE config pages (Layer dyn-range + Gain + enable); drop VoicePage"
```

---

### Task 6: Reference dokumentace

**Files:**
- Modify: `docs/reference/E-resonance.md`, `docs/reference/H-gui.md`, `docs/reference/A-core.md`

- [ ] **Step 1: E-resonance.md** — doplň: výběr velocity vrstvy přes `nearestSlotByRms(NoteSlots, target_db)` (místo `slots[0]`), gain model `excite = vel_norm × harm × 10^(gain_db/20)`, enable toggle, atomiky `gain_lin_/layer_target_db_/enabled_`, settery `setGainDb/setLayerTargetDb/setEnabled`. Uveď, že skip-attack (`resonance_start_frame`) platí pro Streamed mic.

- [ ] **Step 2: H-gui.md** — uprav CONFIG popis: stránky MASTER (Master+Release) | RESONANCE (Resonance Layer dyn. rozsah + Resonance Gain + Excite Decay + Max Resonance, hasEnable=true) | AGC | BBE | LIMITER. `voice_page.h` → `master_page.h` + `resonance_page.h`. Persistence pole: `resonance_enabled/gain_db/layer_db` (místo `resonance_strength`). Pole stránek `pages[5]`, `config_page` 0..4.

- [ ] **Step 3: A-core.md + engine getters** — doplň gettery `bankPeakRmsMinDb/MaxDb` (cache v `loadBank`), settery `setResonanceGainDb/LayerDb/Enabled` (odebrat `setResonanceStrength`), `EngineConfig` pole `resonance_enabled/gain_db/layer_db`.

- [ ] **Step 4: Commit:**
```bash
git add docs/reference/
git commit -m "docs(reference): resonance velocity-layer selection + gain/enable + config reorg"
```

---

### Task 7: Full build, test suite, smoke, finish branch

- [ ] **Step 1: Full build + celá suita:**
```
cmake --build build 2>&1 | tail -6 && ctest --test-dir build --output-on-failure 2>&1 | tail -12
```
Expected: build OK, všechny testy PASS (vč. nových `test_resonance_layer_select`).

- [ ] **Step 2: Smoke (manuální, uživatel, z repo rootu)** — `./build/ithaca-gui`:
  - CONFIG má pořadí MASTER | RESONANCE | AGC | BBE | LIMITER; MASTER = Master+Release; RESONANCE = Layer+Gain+ExciteDecay+MaxResonance + ON/OFF toggle.
  - „Resonance Layer" slider má rozsah dle banky; posun mění, která vrstva rezonuje (slyšitelně jasnější/temnější).
  - „Resonance Gain" mění hlasitost rezonance; toggle ji vypne.
  - Restart app → hodnoty persistované.
  - CLI: `./build/ithaca-cli --resonance -9 --resonance-layer -25 ...` (batch) projde.

- [ ] **Step 3: Finish branch** — REQUIRED SUB-SKILL: superpowers:finishing-a-development-branch (merge → main, push).

---

## Self-Review

- **Spec coverage:** výběr vrstvy (nearestSlotByRms) → T1+T2; gain model + enable → T2; bank RMS range → T2; CLI → T3; persistence (+nové, −strength) → T4; config reorg MASTER/RESONANCE + dyn. rozsah + toggle → T5; docs → T6; testy → T1,T2,T4 + nový select test; finish → T7. ✓
- **Type consistency:** `setResonanceGainDb/LayerDb/Enabled`, `bankPeakRmsMinDb/MaxDb`, `nearestSlotByRms`, `resonance_enabled/gain_db/layer_db`, `gain_lin_/layer_target_db_/enabled_` použity konzistentně napříč tasky. `pages[5]` + `config_page 0..4` + `renderConfigPanel(...,5,...)` v souladu. ✓
- **Placeholders:** žádné — každý krok má konkrétní kód/příkaz. (Task 3 Step 1 a Task 5 Step 7 obsahují „oprav dle chyby" pojistku pro drift, ne placeholder logiky.)
- **Build-green pořadí:** T1 core, T2 core+engine testy, T3 cli, T4 persistence test, T5 gui — každý staví svůj target; plný build v T7. ✓
- **Pozn. k testovatelnosti:** deterministicky testováno T1 (čistá fce), T2 (engine), T4 (persistence round-trip). GUI (T5) + CLI (T3) build + smoke (T7).
