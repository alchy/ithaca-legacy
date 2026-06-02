# Resonance RAM Cache (target layer, 6 s) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cachovat 6 s rezonančního okna do RAM jen pro per-notu cílovou velocity vrstvu; rezonance hraje z RAM (cache mód) nebo streamuje (stream mód) dle per-notě ready flagu; změna slideru přebuduje cache na pozadí (fade + guard).

**Architecture:** `buildResonanceCache` (sdílená off-RT fce) plní `preload_resonance` cílové vrstvy po sortu. `ResonanceVoice` dostane `use_cache` při startu (cache vs stream). `ResonanceEngine` drží per-notě `reso_cache_ready_` (atomic, mimo movable MicLayer) + fade-request. `Engine::rebuildResonanceCache` orchestruje runtime rebuild na background vlákně.

**Tech Stack:** C++20, doctest/CTest, std::thread.

**Spec:** `docs/superpowers/specs/2026-06-02-resonance-ram-cache-design.md`

**Build pozn.:** T1–T5 jsou v `ithaca_core` (stavějte `ithaca_core` + engine testy). Feature je end-to-end funkční po T4 (build cache při loadBank); T5 přidá runtime rebuild, T6 GUI trigger. Plný build až T8.

---

### Task 1: ResonanceVoice — `use_cache` (cache vs stream mód)

**Files:** Modify `engine/voice/resonance_voice.h`, `engine/voice/resonance_voice.cpp`

Cíl: hlas při startu zvolí, zda čte `preload_resonance` (cache), nebo ho ignoruje a streamuje z `resonance_start_frame` (stream mód). Default `true` → existující volání + chování beze změny.

- [ ] **Step 1: Header** — v `engine/voice/resonance_voice.h` změň signaturu `start`:
```cpp
    void start(int midi, const MicLayer* mic, float initial_gain,
               float pan_l, float pan_r, float engine_sr, bool use_cache = true);
```
a přidej do private members (vedle `mic_`):
```cpp
    bool   use_cache_ = true;   // false → ignoruj preload_resonance, streamuj od resonance_start_frame
```

- [ ] **Step 2: start() impl** — v `engine/voice/resonance_voice.cpp` v `ResonanceVoice::start(...)`:
  (a) ulož `use_cache_ = use_cache;` (přidej param do definice).
  (b) Najdi výpočet `res_end` pro initial ring request (kolem ř. 71: `const int64_t res_end = (int64_t)mic_->resonance_start_frame + (int64_t)mic_->resonance_frames;`) a nahraď `resonance_frames` efektivní hodnotou:
```cpp
    const int eff_res_frames = use_cache_ ? mic_->resonance_frames : 0;
    const int64_t res_end     = (int64_t)mic_->resonance_start_frame + (int64_t)eff_res_frames;
```
  (Pokud start() používá `resonance_frames` i jinde pro initial fill, použij `eff_res_frames`.)

- [ ] **Step 3: process() impl** — v `ResonanceVoice::process` nahraď načtení resonance polí (ř. 167–169):
```cpp
    const float*  res_data     = mic_->preload_resonance.data();
    const int     res_start    = mic_->resonance_start_frame;
    const int     res_frames   = mic_->resonance_frames;
```
za:
```cpp
    const float*  res_data     = use_cache_ ? mic_->preload_resonance.data() : nullptr;
    const int     res_start    = mic_->resonance_start_frame;
    const int     res_frames   = use_cache_ ? mic_->resonance_frames : 0;
```
Tím stream mód (`use_cache_=false`) nikdy nesáhne na `preload_resonance` (res_frames=0 → větev `p0 < res_end-1 && res_frames>1` je false → rovnou ring; ring_lo seed `if(res_frames>0)` se přeskočí). Bezpečné proti reallokaci bufferu.

- [ ] **Step 4: Build core + engine testy → PASS** (chování beze změny, default true):
```
cmake --build build --target ithaca_core test_resonance_engine test_resonance_stream_sr 2>&1 | tail -6
ctest --test-dir build -R "test_resonance_engine|test_resonance_stream_sr" --output-on-failure 2>&1 | tail -5
```
Expected: build OK, testy PASS.

- [ ] **Step 5: Commit:**
```bash
git add engine/voice/resonance_voice.h engine/voice/resonance_voice.cpp
git commit -m "feat(resonance): ResonanceVoice use_cache flag (cache vs stream mode)"
```

---

### Task 2: ResonanceEngine — ready flags + fade-request + use_cache do start

**Files:** Modify `engine/resonance/resonance_engine.h`, `engine/resonance/resonance_engine.cpp`

- [ ] **Step 1: Header members + API** — v `engine/resonance/resonance_engine.h`:
přidej include `#include <array>` (pokud chybí). Do public přidej:
```cpp
    // RAM cache rezonance: per-nota true = cilova vrstva ma naplneny preload_resonance
    // (cache mod). false = stream mod (ring). Psano off-RT (loadBank/rebuild), cteno
    // audio threadem v onPlayedNoteOn.
    void setCacheReady(const std::array<bool, 128>& ready) noexcept;
    void clearCacheReady() noexcept;                 // vse false (start rebuildu)
    void requestRecacheFade() noexcept;              // audio thread fadene aktivni pri pristim processBlock
```
Do private members přidej:
```cpp
    std::array<std::atomic<bool>, 128> reso_cache_ready_{};   // default vse false
    std::atomic<bool>                  recache_fade_request_{false};
```

- [ ] **Step 2: Impl setterů + fade** — v `engine/resonance/resonance_engine.cpp`:
```cpp
void ResonanceEngine::setCacheReady(const std::array<bool, 128>& ready) noexcept {
    for (int n = 0; n < 128; ++n)
        reso_cache_ready_[(size_t)n].store(ready[(size_t)n], std::memory_order_release);
}
void ResonanceEngine::clearCacheReady() noexcept {
    for (auto& f : reso_cache_ready_) f.store(false, std::memory_order_release);
}
void ResonanceEngine::requestRecacheFade() noexcept {
    recache_fade_request_.store(true, std::memory_order_relaxed);
}
```

- [ ] **Step 3: onPlayedNoteOn — předej use_cache do start.** Najdi volání `slot->start(N, m, init_gain, pl, pr, engine_sr);` (kolem ř. 185) a nahraď:
```cpp
        const bool use_cache = reso_cache_ready_[(size_t)N].load(std::memory_order_acquire);
        slot->start(N, m, init_gain, pl, pr, engine_sr, use_cache);
```

- [ ] **Step 4: processBlock — obsluž fade-request.** Na začátku `ResonanceEngine::processBlock` (před per-blok decay smyčkou) přidej:
```cpp
    if (recache_fade_request_.exchange(false, std::memory_order_relaxed)) {
        for (auto& slot : voices_)
            if (slot && slot->active() && !slot->fadingOut())
                slot->fadeOut(/*engine_sr=*/ (float)0);  // sr doplníme níže
    }
```
POZN.: `fadeOut` potřebuje `engine_sr`. `processBlock` má k dispozici sr? Pokud ne, ulož poslední `engine_sr_` člen při `onPlayedNoteOn`/`start` nebo předej. Zkontroluj signaturu `processBlock(out_l,out_r,n,pedal)` — sr v ní není. **Řešení:** přidej člen `float last_engine_sr_ = 48000.f;` nastavený v `onPlayedNoteOn` (`last_engine_sr_ = engine_sr;`) a v `processBlock` použij `slot->fadeOut(last_engine_sr_);`. Uprav snippet výše na `slot->fadeOut(last_engine_sr_);` a přidej člen + jeho nastavení v onPlayedNoteOn.

- [ ] **Step 5: Failing test** — přidej do `tests/test_resonance_engine.cpp` nový TEST_CASE (ověří, že bez ready se nehraje cache — nepřímo přes to, že API existuje a ncrashne):
```cpp
TEST_CASE("ResonanceEngine cache-ready API") {
    using namespace ithaca;
    ResonanceEngine res(8);
    std::array<bool,128> ready{}; ready[60] = true;
    CHECK_NOTHROW(res.setCacheReady(ready));
    CHECK_NOTHROW(res.clearCacheReady());
    CHECK_NOTHROW(res.requestRecacheFade());
}
```
(přidej `#include <array>` do testu, pokud chybí.)

- [ ] **Step 6: Build + test → PASS:**
```
cmake --build build --target ithaca_core test_resonance_engine 2>&1 | tail -6 && ctest --test-dir build -R test_resonance_engine --output-on-failure 2>&1 | tail -4
```

- [ ] **Step 7: Commit:**
```bash
git add engine/resonance/resonance_engine.h engine/resonance/resonance_engine.cpp tests/test_resonance_engine.cpp
git commit -m "feat(resonance): per-note cache-ready flags + recache fade-request; pass use_cache to voice"
```

---

### Task 3: Loader — `buildResonanceCache` + ingest přestane plnit preload_resonance

**Files:** Modify `engine/sample/sample_store.h`, `engine/sample/sample_store.cpp`; Test `tests/test_resonance_cache_build.cpp` (+ `tests/CMakeLists.txt`)

- [ ] **Step 1: ingest — jen resonance_start_frame, neplnit buffer.** V `engine/sample/sample_store.cpp` v `ingestSampleFile` nahraď blok plnění (ř. 80–97):
```cpp
    if (mic.mode == MicLayerMode::Streamed && resonance_window_ms > 0) {
        mic.resonance_start_frame = ae;          // = attack_end_frame
        int rwin = (int)((int64_t)resonance_window_ms * info.sample_rate / 1000);
        int avail = info.frames - mic.resonance_start_frame;
        if (avail < 0) avail = 0;
        if (rwin > avail) rwin = avail;
        if (rwin > 0) {
            WavData rd = readWavRange(full_path, mic.resonance_start_frame, rwin);
            if (rd.valid && rd.frames > 0) {
                mic.resonance_frames  = rd.frames;
                mic.preload_resonance = std::move(rd.samples);
            } else {
                logger.log("bank", log::Severity::Warning,
                           "Nelze nacist preload_resonance: %s", filename.c_str());
            }
        }
    }
```
za (jen nastav start_frame; buffer naplní buildResonanceCache po sortu):
```cpp
    if (mic.mode == MicLayerMode::Streamed) {
        mic.resonance_start_frame = ae;          // = attack_end_frame (buffer plni buildResonanceCache)
    }
```
(Pozn.: `resonance_window_ms` param zůstává v signatuře pro kompatibilitu, už ho ingest nepoužívá.)

- [ ] **Step 2: buildResonanceCache deklarace** — v `engine/sample/sample_store.h` přidej (po `loadBank` deklaraci):
```cpp
#include <array>
// Naplni preload_resonance JEN pro per-notu cilovou velocity vrstvu
// (nearestSlotByRms(target_db)), oknem window_ms od resonance_start_frame
// (Streamed mics; cte z disku). Drive cilove, ktere uz cilove nejsou, vycisti.
// FullyLoaded cilove nepotrebuji buffer (hraji z preload_head). Vraci pole
// 128 bool: true = nota ma platnou cilovou vrstvu (cache mod pouzitelny).
// Off-RT (disk I/O). Volat po sortBankSlotsByRms.
std::array<bool, 128> buildResonanceCache(Bank& bank, float target_db,
                                          int window_ms, log::Logger& logger);
```

- [ ] **Step 2b: nearestSlotByRms include** — v `engine/sample/sample_store.cpp` přidej `#include "resonance/resonance_layer_select.h"`.

- [ ] **Step 3: buildResonanceCache impl** — v `engine/sample/sample_store.cpp` (mimo anonymní namespace, vedle `loadBank`):
```cpp
std::array<bool, 128> buildResonanceCache(Bank& bank, float target_db,
                                          int window_ms, log::Logger& logger) {
    std::array<bool, 128> ready{};   // vse false
    for (int n = 0; n < 128; ++n) {
        auto& note = bank.notes[(size_t)n];
        if (!note.recorded || note.slots.empty()) continue;
        const int si = nearestSlotByRms(note, target_db);
        if (si < 0) continue;
        // Vycisti preload_resonance vsech NEcilovych slotu (uvolni RAM po zmene cile).
        for (int s = 0; s < (int)note.slots.size(); ++s) {
            if (s == si) continue;
            for (auto& mic : note.slots[(size_t)s].variants.empty()
                     ? std::vector<MicLayer>{} : note.slots[(size_t)s].variants[0].mics) {
                (void)mic;  // viz pozn. nize — cisteni resime na cilovem; necilove necht
            }
        }
        VelocitySlot& vs = note.slots[(size_t)si];
        if (vs.variants.empty() || vs.variants[0].mics.empty()) continue;
        MicLayer& mic = vs.variants[0].mics[0];
        if (mic.mode == MicLayerMode::FullyLoaded) {
            ready[(size_t)n] = true;   // hraje z preload_head, buffer netreba
            continue;
        }
        // Streamed: nacti window_ms od resonance_start_frame z disku.
        int rwin = (int)((int64_t)window_ms * mic.file.sample_rate / 1000);
        int avail = mic.file.frames - mic.resonance_start_frame;
        if (avail < 0) avail = 0;
        if (rwin > avail) rwin = avail;
        if (rwin <= 0) { ready[(size_t)n] = true; continue; }  // nic za attackem → stream sám utichne
        WavData rd = readWavRange(mic.file.path, mic.resonance_start_frame, rwin);
        if (rd.valid && rd.frames > 0) {
            mic.resonance_frames  = rd.frames;
            mic.preload_resonance = std::move(rd.samples);
            ready[(size_t)n] = true;
        } else {
            logger.log("bank", log::Severity::Warning,
                       "buildResonanceCache: read failed note %d", n);
        }
    }
    return ready;
}
```
**Zjednodušení čištění:** výše uvedená NEcilová-clear smyčka je neohrabaná. Místo ní použij jednoduchou variantu — projdi VŠECHNY sloty noty a u necílových `preload_resonance.clear(); resonance_frames = 0;`, u cílového naplň. Implementuj takto (nahrazuje vnitřek smyčky `for n`):
```cpp
        for (int s = 0; s < (int)note.slots.size(); ++s) {
            auto& vslot = note.slots[(size_t)s];
            if (vslot.variants.empty() || vslot.variants[0].mics.empty()) continue;
            MicLayer& m = vslot.variants[0].mics[0];
            if (s != si) { m.preload_resonance.clear(); m.preload_resonance.shrink_to_fit(); m.resonance_frames = 0; continue; }
            if (m.mode == MicLayerMode::FullyLoaded) { ready[(size_t)n] = true; continue; }
            int rwin = (int)((int64_t)window_ms * m.file.sample_rate / 1000);
            int avail = m.file.frames - m.resonance_start_frame; if (avail < 0) avail = 0;
            if (rwin > avail) rwin = avail;
            if (rwin <= 0) { m.preload_resonance.clear(); m.resonance_frames = 0; ready[(size_t)n] = true; continue; }
            WavData rd = readWavRange(m.file.path, m.resonance_start_frame, rwin);
            if (rd.valid && rd.frames > 0) { m.resonance_frames = rd.frames; m.preload_resonance = std::move(rd.samples); ready[(size_t)n] = true; }
            else logger.log("bank", log::Severity::Warning, "buildResonanceCache: read failed note %d", n);
        }
```
(Použij JEN tuto druhou variantu — smaž první neohrabaný blok i tu prázdnou clear-smyčku z předchozího snippetu. Výsledná funkce: loop `n` → spočti `si` → tento `for s` blok.)

- [ ] **Step 4: Test** — `tests/test_resonance_cache_build.cpp`. Vyžaduje banku na disku; použij stejný způsob jako `test_sample_store.cpp` (ten má fixturu / generuje WAV). Otevři `tests/test_sample_store.cpp`, zjisti jak získává `dir` s WAV samply, a napiš:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "sample/sample_store.h"
#include "util/log.h"
#include <array>
using namespace ithaca;

TEST_CASE("buildResonanceCache plni jen cilovou vrstvu") {
    auto& L = log::Logger::default_();
    // <dir> = adresar s vice velocity vrstvami jedne noty — viz fixtura v test_sample_store.cpp.
    const char* dir = "<DOPLN_DLE_test_sample_store.cpp>";
    Bank bank = loadBank(dir, L);
    if (bank.loaded_samples == 0) return;   // fixtura nedostupna v CI → skip
    auto ready = buildResonanceCache(bank, /*target_db=*/-20.f, /*window_ms=*/1000, L);
    // Najdi notu s >1 slotem a over: prave jedna vrstva ma resonance_frames>0
    // (Streamed) NEBO je FullyLoaded; ostatni 0.
    for (int n = 0; n < 128; ++n) {
        auto& note = bank.notes[n];
        if (!note.recorded || note.slots.size() < 2) continue;
        int filled = 0;
        for (auto& s : note.slots)
            if (!s.variants.empty() && !s.variants[0].mics.empty()
                && s.variants[0].mics[0].resonance_frames > 0) ++filled;
        CHECK(filled <= 1);            // max jedna vrstva (cilova) naplnena
        if (filled == 1) CHECK(ready[n] == true);
    }
}
```
**POZN. implementátorovi:** nahraď `<DOPLN_DLE_test_sample_store.cpp>` reálnou fixturou (přečti `test_sample_store.cpp`). Pokud fixtura nemá vícevrstvou notu, test jen ověří `filled <= 1` (triviálně projde) — to je OK jako smoke; primární pokrytí výběru je `test_resonance_layer_select`.

Registruj v `tests/CMakeLists.txt`:
```cmake
add_executable(test_resonance_cache_build test_resonance_cache_build.cpp)
target_link_libraries(test_resonance_cache_build PRIVATE ithaca_core doctest)
add_test(NAME test_resonance_cache_build COMMAND test_resonance_cache_build)
```

- [ ] **Step 5: Build + testy → PASS** (vč. test_sample_store — ověř, že nepadá na chybějícím preload_resonance po této změně; pokud test_sample_store assertoval naplněný `preload_resonance`/`resonance_frames`, uprav ho — nově se plní až přes buildResonanceCache):
```
cmake -S . -B build >/dev/null && cmake --build build --target ithaca_core test_resonance_cache_build test_sample_store 2>&1 | tail -8
ctest --test-dir build -R "test_resonance_cache_build|test_sample_store" --output-on-failure 2>&1 | tail -8
```

- [ ] **Step 6: Commit:**
```bash
git add engine/sample/sample_store.h engine/sample/sample_store.cpp tests/test_resonance_cache_build.cpp tests/CMakeLists.txt tests/test_sample_store.cpp
git commit -m "feat(loader): buildResonanceCache fills target velocity layer only; ingest stops eager fill"
```

---

### Task 4: Engine — wire buildResonanceCache do loadBank + okno 6 s

**Files:** Modify `engine/engine.h`, `engine/engine.cpp`

- [ ] **Step 1: EngineConfig okno 6 s.** V `engine/engine.h` změň `int resonance_window_ms = 500;` na:
```cpp
    int   resonance_window_ms = 6000;  // RAM cache rezonance: okno [ms] cilove vrstvy
```

- [ ] **Step 2: loadBank wiring.** V `engine/engine.cpp` v `Engine::loadBank`, za blok RMS cache (před `return true;`), přidej:
```cpp
    // Postav RAM cache rezonance pro per-notu cilovou vrstvu + zapis ready flagy.
    {
        auto ready = ithaca::buildResonanceCache(bank_, cfg_.resonance_layer_db,
                                                 cfg_.resonance_window_ms, L);
        if (resonance_) resonance_->setCacheReady(ready);
    }
```
(`L` = logger v loadBank; `buildResonanceCache` je v `sample_store.h` — engine.cpp už includuje sample_store nepřímo přes engine.h; pokud ne, přidej `#include "sample/sample_store.h"`.)

- [ ] **Step 3: Build core + engine testy → PASS:**
```
cmake --build build --target ithaca_core test_resonance_engine test_resonance_stream_sr test_engine_diagnostics 2>&1 | tail -6
ctest --test-dir build -R "test_resonance" --output-on-failure 2>&1 | tail -6
```
Expected: build OK, testy PASS. (Rezonance teď hraje z 6 s RAM cache cílové vrstvy.)

- [ ] **Step 4: Commit:**
```bash
git add engine/engine.h engine/engine.cpp
git commit -m "feat(engine): build resonance RAM cache (6s target layer) on loadBank"
```

---

### Task 5: Engine — runtime rebuild na pozadí (`rebuildResonanceCache`)

**Files:** Modify `engine/engine.h`, `engine/engine.cpp`

- [ ] **Step 1: Header — metoda + thread member.** V `engine/engine.h` přidej include `#include <thread>`. Public:
```cpp
    // Runtime prestavba rezonancni cache pro novy layer target (GUI slider).
    // Fadene aktivni rezonance + ready=false (nove streamuji), pak na pozadi
    // znovu nacte cilove vrstvy a ready=true. Volat z GUI threadu (debounced).
    void rebuildResonanceCache(float target_db) noexcept;
```
Private:
```cpp
    std::thread          recache_thread_;
    std::atomic<bool>    recache_running_{false};
    std::atomic<float>   recache_pending_target_{-30.f};
    std::atomic<bool>    recache_has_pending_{false};
```

- [ ] **Step 2: Impl + coalescing + join.** V `engine/engine.cpp`:
```cpp
void Engine::rebuildResonanceCache(float target_db) noexcept {
    if (!resonance_) return;
    cfg_.resonance_layer_db = target_db;          // engine si pamatuje aktualni cil
    resonance_->setLayerTargetDb(target_db);      // nove spawny vyberou novou vrstvu hned
    // Pokud uz rebuild bezi, jen ulozim pending cil (coalescing) — doběhne a spustí znovu.
    if (recache_running_.exchange(true, std::memory_order_acq_rel)) {
        recache_pending_target_.store(target_db, std::memory_order_relaxed);
        recache_has_pending_.store(true, std::memory_order_release);
        return;
    }
    resonance_->clearCacheReady();      // nove hlasy → stream mod
    resonance_->requestRecacheFade();   // audio thread fadene aktivni cache-mod hlasy
    if (recache_thread_.joinable()) recache_thread_.join();   // predchozi uz dobehl
    recache_thread_ = std::thread([this]() {
        for (;;) {
            const float t = cfg_.resonance_layer_db;
            // Pockej na dobeh fade (rapid 5ms ramp) — bezpecna rezerva.
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            auto ready = ithaca::buildResonanceCache(
                bank_, t, cfg_.resonance_window_ms, log::Logger::default_());
            if (resonance_) resonance_->setCacheReady(ready);
            // Coalescing: prisel mezitim novy cil?
            if (recache_has_pending_.exchange(false, std::memory_order_acquire)) {
                const float nt = recache_pending_target_.load(std::memory_order_relaxed);
                cfg_.resonance_layer_db = nt;
                if (resonance_) { resonance_->clearCacheReady(); resonance_->requestRecacheFade(); }
                continue;   // dalsi kolo
            }
            break;
        }
        recache_running_.store(false, std::memory_order_release);
    });
}
```

- [ ] **Step 3: Join při shutdown.** V `Engine::~Engine()` (najdi destruktor; pokud není explicitní, přidej ho do engine.cpp/h) zajisti join PŘED destrukcí resonance/bank:
```cpp
    if (recache_thread_.joinable()) recache_thread_.join();
```
Vlož na začátek destruktoru (před uvolněním `resonance_`/`bank_`). Pokud Engine nemá explicitní `~Engine()`, deklaruj `~Engine();` v engine.h a definuj v engine.cpp s tímto joinem. **Také** v `reloadBank` PŘED `loadBank(dir)` přidej join (rebuild nesmí běžet při reloadu):
```cpp
    if (recache_thread_.joinable()) recache_thread_.join();
    recache_running_.store(false, std::memory_order_release);
```
(Umísti za `bank_loading_=true` + sleep, před `loadBank`.)

- [ ] **Step 4: Build core → PASS:**
```
cmake --build build --target ithaca_core 2>&1 | tail -4
ctest --test-dir build -R "test_resonance|test_engine" --output-on-failure 2>&1 | tail -6
```
Expected: build OK, testy PASS.

- [ ] **Step 5: Commit:**
```bash
git add engine/engine.h engine/engine.cpp
git commit -m "feat(engine): rebuildResonanceCache — background rebuild on layer change (fade+guard, coalesced)"
```

---

### Task 6: GUI — debounce změny Resonance Layer → rebuild

**Files:** Modify `app/gui/main.cpp`

Cíl: když se `resonance_layer_db` ustálí (~400 ms beze změny), zavolej `ctx.engine.rebuildResonanceCache(...)`. (Slider ho zapisuje okamžitě přes `setResonanceLayerDb`; rebuild jen řídí cache.)

- [ ] **Step 1: Debounce stav.** V `app/gui/main.cpp` poblíž persistence debounce (`dirty_since`) přidej:
```cpp
    float prev_layer_db = ctx.state.resonance_layer_db;
    std::optional<std::chrono::steady_clock::time_point> layer_dirty_since;
```

- [ ] **Step 2: Detekce + debounce v render loop.** Poblíž persistence debounce bloku přidej:
```cpp
        if (ctx.state.resonance_layer_db != prev_layer_db) {
            prev_layer_db = ctx.state.resonance_layer_db;
            layer_dirty_since = std::chrono::steady_clock::now();
        }
        if (layer_dirty_since) {
            auto now = std::chrono::steady_clock::now();
            if (now - *layer_dirty_since > std::chrono::milliseconds(400)) {
                ctx.engine.rebuildResonanceCache(ctx.state.resonance_layer_db);
                layer_dirty_since.reset();
            }
        }
```

- [ ] **Step 3: Build → PASS:**
```
cmake --build build --target ithaca-gui 2>&1 | tail -6
```
Expected: success.

- [ ] **Step 4: Commit:**
```bash
git add app/gui/main.cpp
git commit -m "feat(gui): debounced Resonance Layer change triggers background cache rebuild"
```

---

### Task 7: Reference dokumentace

**Files:** Modify `docs/reference/E-resonance.md`, `docs/reference/F-loader.md`, `docs/reference/A-core.md`, `docs/reference/C-buffers.md`

- [ ] **Step 1:** E-resonance — `use_cache` mód ResonanceVoice (cache vs stream), per-notě `reso_cache_ready_`, fade-request, `onPlayedNoteOn` čte ready. F-loader — `buildResonanceCache` (post-sort, jen cílová vrstva, 6 s; ingest už neplní). A-core — `Engine::rebuildResonanceCache` (background thread, coalescing, join v ~Engine/reloadBank), `resonance_window_ms` 6000. C-buffers — rezonance teď primárně z RAM (6 s), rings jen fallback/během rebuildu.

- [ ] **Step 2: Commit:**
```bash
git add docs/reference/
git commit -m "docs(reference): resonance RAM cache (target layer, use_cache, bg rebuild)"
```

---

### Task 8: Full build, tests, smoke, finish branch

- [ ] **Step 1: Full build + suita:**
```
cmake --build build 2>&1 | tail -6 && ctest --test-dir build --output-on-failure 2>&1 | tail -12
```
Expected: build OK, všechny testy PASS.

- [ ] **Step 2: Smoke (uživatel, z repo rootu)** — `./build/ithaca-gui`:
  - Hraj akord s pedálem → rezonance zní; MAIN/RESO RINGS dlaždice by NEMĚLY červenat (rezonance z RAM, ne stream).
  - Posuň „Resonance Layer" → po ~400 ms se cache přebuduje na pozadí; krátce předtím se znějící rezonance ztiší (fade) a chvíli se streamuje; pak zase z RAM. Žádný pád/dlouhý výpadek.
  - Reload banky během/po změně → bez pádu.
  - Sleduj RAM (Activity Monitor): resident by měl klesnout (~−60 MB vs předchozí 500 ms × 16).

- [ ] **Step 3: Finish branch** — REQUIRED SUB-SKILL: superpowers:finishing-a-development-branch (merge → main, push).

---

## Self-Review

- **Spec coverage:** 6 s target-layer cache → T3+T4; use_cache cache/stream mód → T1; ready flags + fade-request → T2; loader split + buildResonanceCache → T3; loadBank wiring + okno → T4; rebuild bg+coalesce+join → T5; GUI debounce → T6; docs → T7; testy T2/T3 + nové; finish T8. ✓
- **Type consistency:** `buildResonanceCache(Bank&,float,int,Logger&)→array<bool,128>`, `setCacheReady/clearCacheReady/requestRecacheFade`, `reso_cache_ready_`, `recache_fade_request_`, `rebuildResonanceCache(float)`, `use_cache_`/`start(...,use_cache=true)` konzistentní napříč tasky. ✓
- **Placeholders:** test fixtura v T3 Step 4 má explicitní `<DOPLN…>` s instrukcí přečíst `test_sample_store.cpp` (ne logika, ale cesta k fixtuře — implementátor doplní; primární pokrytí je v `test_resonance_layer_select`). Ostatní kroky mají konkrétní kód.
- **Build-green pořadí:** T1–T5 stavějí `ithaca_core` + engine testy; T6 `ithaca-gui`; plný build T8. Default `use_cache=true` (T1) + ready=false-until-build (T2) drží mezistavy funkční.
- **RT-safety:** realloc `preload_resonance` (T3/T5) jen off-RT po fade (T2 fade-request) + ready guard; stream-mód voices se bufferu nedotknou (T1). Join thread v ~Engine/reloadBank (T5).
- **Pozn. k testovatelnosti:** deterministicky T1/T2/T3 (engine/loader); rebuild fade-coordination + RAM pokles = smoke (T8).
