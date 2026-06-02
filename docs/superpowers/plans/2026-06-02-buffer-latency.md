# Buffer Size + DSP Load/Latency Meter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Měřit DSP load (čas renderu vs perioda bloku) s červeným overload indikátorem, umožnit runtime změnu audio bufferu (32–8192), zobrazit read-only SAMPLE RATE — vše perzistované v `state.json`.

**Architecture:** (A) Engine časuje `processBlock` → atomiky (peak-hold load + overload timestamp) → gettery, vzor jako master peak / `noteOnRecent`. (B) GUI topbar combo mění buffer přes nový `AppContext::setAudioBlockSize` (stop→setBlockSize→start), SR jen read-only label. Perzistence rozšíří `GuiState` o `audio_block_size` + `audio_sample_rate`.

**Tech Stack:** C++20, Dear ImGui, miniaudio, doctest/CTest.

**Spec:** `docs/superpowers/specs/2026-06-02-buffer-latency-design.md`

---

### Task 1: Engine DSP load metering

**Files:**
- Modify: `engine/engine.h` (atomiky + gettery)
- Modify: `engine/engine.cpp:126-239` (`processBlock` timing)
- Test: `tests/test_engine_diagnostics.cpp` (přidat TEST_CASE)

- [ ] **Step 1: Přidat failing test** do `tests/test_engine_diagnostics.cpp` (na konec souboru, před žádný main — soubor má `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` nahoře, jen přidej TEST_CASE):

```cpp
TEST_CASE("Engine DSP load meter - fresh + after block") {
    using namespace ithaca;
    Engine e;
    EngineConfig cfg;
    cfg.sample_rate = 48000;
    cfg.block_size  = 256;
    REQUIRE(e.init(cfg));

    // Cerstvy engine: zadny render, load 0, zadny overload.
    CHECK(e.dspLoadPeak() == 0.f);
    CHECK_FALSE(e.overloadRecent(1e9f));

    // Po jednom bloku (bez banky = ticho): load je definovany, konecny, >= 0.
    std::vector<float> l(256, 0.f), r(256, 0.f);
    e.processBlock(l.data(), r.data(), 256);
    const float load = e.dspLoadPeak();
    CHECK(load >= 0.f);
    CHECK(std::isfinite(load));
    // Render ticha na 256 framu se vejde do okna (~5.3 ms) → zadny overload.
    CHECK_FALSE(e.overloadRecent(1e9f));

    // overloadRecent s nulovym oknem je vzdy false (i kdyby load byl >=1).
    CHECK_FALSE(e.overloadRecent(0.f));
}
```

Přidej include nahoře (po `#include "engine.h"`): `#include <cmath>` a `#include <vector>`.

- [ ] **Step 2: Spustit test → FAIL** (gettery neexistují):

```
cmake --build build --target test_engine_diagnostics 2>&1 | tail -20
```
Expected: compile error „no member named 'dspLoadPeak'".

- [ ] **Step 3: Přidat atomiky + gettery** do `engine/engine.h`.

Za blok master peak getterů (engine.h:145-146, řádek s `masterPeakR`) přidej:

```cpp
    // -- DSP load meter (GUI; atomic) --
    // Peak-hold zatizeni audio threadu = cas renderu / perioda bloku. 1.0 = na
    // hranici deadline, > 1.0 = blok se nestihl (dropout riziko). Decay ~0.5 s.
    float dspLoadPeak() const noexcept { return dsp_load_peak_.load(std::memory_order_relaxed); }
    // True kdyz overload (load >= 1.0) nastal pred mene nez `ms` ms. Vzor
    // noteOnRecent — GUI cervena dlazba.
    bool  overloadRecent(float ms) const noexcept;
```

Do private member sekce, za `master_peak_r_` (engine.h:172), přidej:

```cpp
    // DSP load meter — psano z audio threadu (processBlock), cteno z GUI.
    std::atomic<float>                dsp_load_peak_{0.f};
    std::atomic<uint64_t>             last_overload_us_{0};
```

- [ ] **Step 4: Implementovat timing** v `engine/engine.cpp`.

(a) `overloadRecent` — přidej za `noteOffRecent` (engine.cpp:118, před `allNotesOff`):

```cpp
bool Engine::overloadRecent(float ms) const noexcept {
    const uint64_t t = last_overload_us_.load(std::memory_order_relaxed);
    if (t == 0) return false;
    return (nowMicros() - t) < (uint64_t)(ms * 1000.f);
}
```

(b) Timing v `processBlock` — `t0` hned po `bank_loading_` guardu. Najdi řádek `const float sr = (float)cfg_.sample_rate;` (engine.cpp:139) a vlož PŘED něj:

```cpp
    const uint64_t block_t0 = nowMicros();
```

(c) Na úplném konci `processBlock` — za `master_peak_r_.store(new_r, ...)` (engine.cpp:238), před uzavírací `}` funkce, vlož:

```cpp
    // 5. DSP load meter: cas renderu / perioda bloku. Peak-hold s decay ~0.5 s,
    // aby cislo na liste bylo citelne. Overload (load >= 1.0 = minul deadline)
    // orazitkujeme pro cervene blikani v GUI.
    const uint64_t dt_us     = nowMicros() - block_t0;
    const uint64_t period_us = (uint64_t)n_samples * 1000000ull / (uint64_t)cfg_.sample_rate;
    const float    load      = period_us > 0 ? (float)dt_us / (float)period_us : 0.f;
    if (load >= 1.0f)
        last_overload_us_.store(nowMicros(), std::memory_order_relaxed);
    const float load_decay = std::exp(-(float)n_samples / (0.5f * sr));
    const float cur_load   = dsp_load_peak_.load(std::memory_order_relaxed);
    dsp_load_peak_.store((load > cur_load * load_decay) ? load : cur_load * load_decay,
                         std::memory_order_relaxed);
```

- [ ] **Step 5: Spustit test → PASS** + celá suita:

```
cmake --build build --target test_engine_diagnostics 2>&1 | tail -5 && ctest --test-dir build -R test_engine_diagnostics --output-on-failure 2>&1 | tail -5
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/engine.h engine/engine.cpp tests/test_engine_diagnostics.cpp
git commit -m "feat(engine): DSP load meter — processBlock timing + dspLoadPeak/overloadRecent"
```

---

### Task 2: Persistence — audio_block_size + audio_sample_rate

**Files:**
- Modify: `app/gui/persistence.h:10-33` (GuiState pole)
- Modify: `app/gui/persistence.cpp:119-134` (load) + `142-178` (save)

> Pozn.: GUI/persistence vrstva nemá doctest target → ověřeno buildem + smoke (Task 6). Nejde o produkční logiku enginu, ale o serializaci.

- [ ] **Step 1: Přidat pole** do `GuiState` (`app/gui/persistence.h`), za `config_page` (řádek 30):

```cpp
    // -- Audio (Faze 8) --
    int   audio_block_size  = 256;    // runtime-menitelny z GUI (BUFFER combo)
    int   audio_sample_rate = 48000;  // jen z JSONu; GUI zobrazuje read-only
```

- [ ] **Step 2: Defensive load** v `loadState` (`app/gui/persistence.cpp`), za řádek `s.config_page = readI("config_page", s.config_page);` (řádek 134):

```cpp
        s.audio_block_size  = readI("audio_block_size", s.audio_block_size);
        s.audio_sample_rate = readI("audio_sample_rate", s.audio_sample_rate);
```

- [ ] **Step 3: Save** v `saveState` (`app/gui/persistence.cpp`). Zaměň poslední položku (řádek 177, `config_page` — má `\n` bez čárky) za verzi s čárkou + přidej dvě nové jako poslední:

```cpp
        f << "  \"config_page\": "        << s.config_page          << ",\n";
        f << "  \"audio_block_size\": "   << s.audio_block_size     << ",\n";
        f << "  \"audio_sample_rate\": "  << s.audio_sample_rate    << "\n";
```

- [ ] **Step 4: Build** (kompiluje se s GUI targetem):

```
cmake --build build --target ithaca-gui 2>&1 | tail -10
```
Expected: success.

- [ ] **Step 5: Commit**

```bash
git add app/gui/persistence.h app/gui/persistence.cpp
git commit -m "feat(gui): persist audio_block_size + audio_sample_rate in state.json"
```

---

### Task 3: AppContext — apply SR/block at init + setAudioBlockSize helper

**Files:**
- Modify: `app/gui/app_context.h:31` (deklarace helper)
- Modify: `app/gui/app_context.cpp:53-54` (cfg z state) + `app_context.cpp:135` (definice helper)

- [ ] **Step 1: Deklarace helper** v `app/gui/app_context.h`, za `bool initFromState(...)` (řádek 31):

```cpp
    // Runtime zmena audio bufferu (BUFFER combo). Zastavi audio device, prenastavi
    // engine block size, znovu nastartuje device a aktualizuje state.audio_block_size.
    // Kratky audio gap je ocekavany (uzivatelska akce). Volat z GUI threadu.
    void setAudioBlockSize(int n);
```

- [ ] **Step 2: Aplikovat SR + block z state** v `initFromState` (`app/gui/app_context.cpp`). Zaměň řádky 53-54:

```cpp
    cfg.sample_rate          = 48000;
    cfg.block_size           = 256;
```
za:
```cpp
    cfg.sample_rate          = state.audio_sample_rate;
    cfg.block_size           = state.audio_block_size;
```

- [ ] **Step 3: Definice helper** v `app/gui/app_context.cpp`, za `shutdown()` (před uzavírací `} // namespace ithaca::gui` na řádku 135). `audioCallback` je v anonymním namespace téhož souboru → dostupný:

```cpp
void AppContext::setAudioBlockSize(int n) {
    const int applied = engine.setBlockSize(n);   // clamp 32..8192 + re-prepare
    state.audio_block_size = applied;
    if (audio) {
        audio->stop();   // joinne miniaudio callback → zadny RT race s restart
        if (!audio->start(&audioCallback, &engine, engine.sampleRate(), applied)) {
            log::Logger::default_().log("gui", log::Severity::Warning,
                "Restart audio device s block=%d selhal", applied);
        } else {
            log::Logger::default_().log("gui", log::Severity::Info,
                "Audio buffer zmenen na %d framu", applied);
        }
    }
}
```

- [ ] **Step 4: Build**

```
cmake --build build --target ithaca-gui 2>&1 | tail -10
```
Expected: success.

- [ ] **Step 5: Commit**

```bash
git add app/gui/app_context.h app/gui/app_context.cpp
git commit -m "feat(gui): apply persisted SR/block at init + setAudioBlockSize runtime helper"
```

---

### Task 4: GUI topbar — SR label + BUFFER combo + DSP load readout

**Files:**
- Modify: `app/gui/panel_topbar.cpp:85-87` (vložit mezi CH a LOG)

- [ ] **Step 1: Vložit blok** do `renderTopBar` mezi konec CH combo (`app/gui/panel_topbar.cpp:85`, za `ImGui::EndCombo();` uzavírající `##ch`) a komentář LOG (řádek 87). Vlož:

```cpp
    // -- SAMPLE RATE (read-only) | BUFFER (runtime combo + ms) | DSP LOAD --
    const int   sr        = ctx.engine.sampleRate();
    const float sr_f      = (float)(sr > 0 ? sr : 48000);

    ImGui::SameLine(0, 18);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("SR");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    { char b[16]; std::snprintf(b, sizeof(b), "%g kHz", sr_f / 1000.0);
      ImGui::TextUnformatted(b); }

    ImGui::SameLine(0, 18);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("BUFFER");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(72);
    {
        static const int kBufs[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
        const int cur_bs = ctx.engine.blockSize();
        char curlbl[8]; std::snprintf(curlbl, sizeof(curlbl), "%d", cur_bs);
        if (ImGui::BeginCombo("##buffer", curlbl)) {
            for (int v : kBufs) {
                char b[8]; std::snprintf(b, sizeof(b), "%d", v);
                if (ImGui::Selectable(b, v == cur_bs) && v != cur_bs)
                    ctx.setAudioBlockSize(v);
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    { char b[16]; std::snprintf(b, sizeof(b), "%.1f ms",
                                (float)ctx.engine.blockSize() * 1000.0f / sr_f);
      ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
      ImGui::TextUnformatted(b);
      ImGui::PopStyleColor(); }

    ImGui::SameLine(0, 18);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(Colors::muted));
    ImGui::TextUnformatted("DSP");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    {
        const bool overload = ctx.engine.overloadRecent(4000.f);
        const ImU32 col = overload ? IM_COL32(0xd0, 0x5a, 0x4a, 255)
                                   : Colors::v(Colors::muted);
        char b[8]; std::snprintf(b, sizeof(b), "%.0f%%", ctx.engine.dspLoadPeak() * 100.f);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(b);
        ImGui::PopStyleColor();
    }
```

> Pozn. k barvě: `IM_COL32(0xd0,0x5a,0x4a,255)` je stejná „ring_red" jako v `panel_indicators.cpp` (konzistence). Žlutá varovná úroveň se NEpřidává (jen červená při overloadu) — per spec.

- [ ] **Step 2: Build**

```
cmake --build build --target ithaca-gui 2>&1 | tail -15
```
Expected: success (panel_topbar.cpp už includuje `<cstdio>` pro snprintf a `theme.h` pro Colors).

- [ ] **Step 3: Commit**

```bash
git add app/gui/panel_topbar.cpp
git commit -m "feat(gui): topbar SR label + BUFFER selector (ms) + DSP LOAD % with overload red"
```

---

### Task 5: main.cpp — debounce save pro audio_block_size

**Files:**
- Modify: `app/gui/main.cpp:290` (přidat do `changed`)

- [ ] **Step 1: Přidat do change-detekce.** V `app/gui/main.cpp` zaměň poslední řádek výrazu `changed` (řádek 290, `last_saved.max_resonance_voices != ctx.state.max_resonance_voices;`) za:

```cpp
            last_saved.max_resonance_voices != ctx.state.max_resonance_voices ||
            last_saved.audio_block_size    != ctx.state.audio_block_size;
```

> `audio_sample_rate` se z GUI nemění (jen JSON), takže do debounce nepatří — uloží se ale tak jako tak při každém save (je v saveState) a při čistém exitu.

- [ ] **Step 2: Build**

```
cmake --build build --target ithaca-gui 2>&1 | tail -8
```
Expected: success.

- [ ] **Step 3: Commit**

```bash
git add app/gui/main.cpp
git commit -m "feat(gui): debounce-save audio_block_size on change"
```

---

### Task 6: Full build, test suite, smoke

- [ ] **Step 1: Full build + celá testovací suita**

```
cmake --build build 2>&1 | tail -15 && ctest --test-dir build --output-on-failure 2>&1 | tail -20
```
Expected: build OK, všechny testy PASS.

- [ ] **Step 2: Smoke (manuální, spustí uživatel z repo rootu)**

Ověřit:
- Topbar mezi CH a LOG zobrazuje `SR 48 kHz | BUFFER [256] 5.3 ms | DSP <n>%`.
- Změna BUFFER comba (např. 32, 1024) → zvuk pokračuje (krátký gap OK), ms se přepočítá, DSP % se změní (menší buffer = vyšší %).
- Při velmi malém bufferu (32) + akord/rezonance může DSP % vyskočit a zčervenat (overload) — to je očekávané pro testování.
- Restart appky → BUFFER si pamatuje poslední hodnotu (persist).
- Ruční změna `audio_sample_rate` v `state.json` (např. 44100) → po restartu SR label ukazuje `44.1 kHz`, zvuk hraje správnou výšku (per-voice resampling).

- [ ] **Step 3: Finish branch** — REQUIRED SUB-SKILL: superpowers:finishing-a-development-branch (merge `feat/buffer-latency` → `main`, push).

---

## Self-Review

- **Spec coverage:** A) DSP load metr → Task 1. B) BUFFER selektor → Task 3+4. SR read-only → Task 4. Persistence obou polí → Task 2. Debounce → Task 5. Overload jen červená (load≥1.0) → Task 1 (timestamp) + Task 4 (barva). Hodnoty bufferu {32..8192} → Task 4. ms z `engine.sampleRate()` → Task 4. ✓
- **Type consistency:** `dspLoadPeak()`/`overloadRecent(float)`/`blockSize()`/`sampleRate()`/`setBlockSize(int)`/`setAudioBlockSize(int)` použity konzistentně napříč tasky. `audio_block_size`/`audio_sample_rate` shodně v persistence/app_context/main/topbar. ✓
- **Placeholders:** žádné — každý krok má konkrétní kód/příkaz. ✓
- **Pozn. k testovatelnosti:** deterministicky testován jen engine getter layer (Task 1); GUI/persistence/restart ověřeny buildem + smoke (Task 6) — žádný GUI doctest harness neexistuje, overload-flash je inherentně časově závislý.
