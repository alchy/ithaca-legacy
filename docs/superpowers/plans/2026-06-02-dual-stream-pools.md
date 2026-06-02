# Dual Stream Pools + Ring Diagnostics + Live Resonance Budget — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give resonance streaming its own StreamEngine (rings + workers) separate from main voices, surface per-pool ring usage + underruns to the GUI (count flashes red 4 s on underrun), and make the resonance voice budget live-adjustable so the user can tune it per hardware.

**Architecture:** Two `StreamEngine` instances — `stream_main_` (VoicePool) and `stream_resonance_` (ResonanceEngine) — each with its own rings, workers, and queue. Each records its last-underrun timestamp; voices stamp it at underrun sites. The Engine exposes per-pool `ringsUsed/Total` and `…UnderrunRecent(ms)` (same atomic-timestamp pattern as `noteOnRecent`). `ResonanceEngine::max_voices_` becomes atomic and is driven live by the now-editable MAX RESONANCE slider.

**Tech Stack:** C++20 engine, Dear ImGui GUI, doctest + CTest.

**Spec:** `docs/superpowers/specs/2026-06-02-dual-stream-pools-design.md`

**Build/test (from repo root `/Users/j/Projects/ithaca-legacy`):** `cmake --build build -j` · `ctest --test-dir build` · GUI smoke `( ./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca >/tmp/s.txt 2>&1 & echo $! >/tmp/p ); sleep 3; kill $(cat /tmp/p)`

**Conventions:** Czech comments; namespace `ithaca`; commits end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`; don't commit `imgui.ini`. Work on branch `feat/dual-stream-pools`.

---

## File Structure

- `engine/stream/stream_engine.h/.cpp` — add per-instance underrun timestamp (`last_underrun_us_`, `noteUnderrun()`, `underrunRecent()`).
- `engine/voice/voice.cpp` — main voice stamps `stream_->noteUnderrun()` at its underrun site.
- `engine/voice/resonance_voice.cpp` — resonance voice stamps `stream_->noteUnderrun()` at its underrun site.
- `engine/engine.h/.cpp` — split `stream_` into `stream_main_` + `stream_resonance_`; `EngineConfig` resonance-pool fields; wiring; per-pool getters; `setMaxResonanceVoices`.
- `engine/resonance/resonance_engine.h/.cpp` — atomic `max_voices_` + `setMaxVoices/maxVoices`.
- `app/gui/voice_page.h` — make MAX RESONANCE param editable + live.
- `app/gui/panel_indicators.cpp` — per-pool ring readout with red-flash.
- `tests/test_stream_engine.cpp` (new) + `tests/test_resonance_engine.cpp` (existing) — unit tests.
- `tests/CMakeLists.txt` — register new test.

---

## Task 1: StreamEngine per-instance underrun timestamp

**Files:**
- Modify: `engine/stream/stream_engine.h`, `engine/stream/stream_engine.cpp`
- Create: `tests/test_stream_engine.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test** — create `tests/test_stream_engine.cpp`

```cpp
// tests/test_stream_engine.cpp — StreamEngine diagnostika (underrun timestamp).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "stream/stream_engine.h"

using namespace ithaca;

TEST_CASE("StreamEngine: underrunRecent je false dokud nenastal underrun") {
    StreamEngine se(4, 1024, 1);
    CHECK(se.underrunRecent(1000.f) == false);
}

TEST_CASE("StreamEngine: noteUnderrun orazitkuje a underrunRecent to vidi") {
    StreamEngine se(4, 1024, 1);
    se.noteUnderrun();
    CHECK(se.underrunRecent(1000.f) == true);   // prave ted
    CHECK(se.underrunRecent(0.f)    == false);  // nulove okno → nic neni "recent"
}
```

- [ ] **Step 2: Register the test** — append to `tests/CMakeLists.txt`

```cmake
add_executable(test_stream_engine test_stream_engine.cpp)
target_link_libraries(test_stream_engine PRIVATE ithaca_core doctest)
add_test(NAME test_stream_engine COMMAND test_stream_engine)
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake -S . -B build >/dev/null && cmake --build build --target test_stream_engine -j 2>&1 | tail -5`
Expected: FAIL — `underrunRecent`/`noteUnderrun` not members of `StreamEngine`.

- [ ] **Step 4: Add the API to `engine/stream/stream_engine.h`** — in the `public:` section after `numRingsUsed()` (line ~159):

```cpp
    // -- Underrun diagnostika (per-pool). Hlas pri underrunu vola noteUnderrun();
    //    GUI cte underrunRecent (vzor jako Engine::noteOnRecent). --
    void noteUnderrun() noexcept;                 // orazitkuje steady_clock micros
    bool underrunRecent(float ms) const noexcept; // true kdyz posledni underrun < ms
```

and in the `private:` members (after `refill_threshold_`, line ~188):

```cpp
    std::atomic<uint64_t> last_underrun_us_{0};
```

- [ ] **Step 5: Implement in `engine/stream/stream_engine.cpp`** — add `#include <chrono>` near the top includes, and add at the end of `namespace ithaca` (before the closing brace):

```cpp
namespace {
uint64_t nowMicrosSE() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}
} // namespace

void StreamEngine::noteUnderrun() noexcept {
    last_underrun_us_.store(nowMicrosSE(), std::memory_order_relaxed);
}

bool StreamEngine::underrunRecent(float ms) const noexcept {
    const uint64_t t = last_underrun_us_.load(std::memory_order_relaxed);
    if (t == 0) return false;
    return (nowMicrosSE() - t) < (uint64_t)(ms * 1000.f);
}
```

- [ ] **Step 6: Run to verify it passes**

Run: `cmake --build build -j && ctest --test-dir build -R test_stream_engine`
Expected: PASS (2 cases).

- [ ] **Step 7: Commit**

```bash
git add engine/stream/stream_engine.h engine/stream/stream_engine.cpp tests/test_stream_engine.cpp tests/CMakeLists.txt
git commit -m "feat(stream): per-instance underrun timestamp (noteUnderrun/underrunRecent)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Voices stamp underrun on their StreamEngine

**Files:**
- Modify: `engine/voice/voice.cpp`, `engine/voice/resonance_voice.cpp`

Both voices already hold `stream_` (a `StreamEngine*`) and detect underrun by setting `underrun_fading_ = true`. Stamp the pool there.

- [ ] **Step 1: Main voice** — in `engine/voice/voice.cpp`, find the underrun block (the `if (!underrun_fading_) { underrun_fading_ = true; ... }` inside the `if (underrun)` branch). Immediately after `underrun_fading_ = true;` add:

```cpp
                    if (stream_) stream_->noteUnderrun();
```

- [ ] **Step 2: Resonance voice** — in `engine/voice/resonance_voice.cpp`, find `if (!underrun_fading_) { underrun_fading_ = true; underrun_gain_ = 1.f; LOG_RT_WARN(...); }`. Immediately after `underrun_fading_ = true;` add:

```cpp
                        if (stream_) stream_->noteUnderrun();
```

- [ ] **Step 3: Build (no unit test — RT path; verified by Task 6 GUI + existing logs)**

Run: `cmake --build build -j 2>&1 | grep -iE "error|warning:" | head; ctest --test-dir build 2>&1 | grep -E "passed|failed"`
Expected: clean build, all tests pass (26 + new = 28 cases overall green).

- [ ] **Step 4: Commit**

```bash
git add engine/voice/voice.cpp engine/voice/resonance_voice.cpp
git commit -m "feat(voice): stamp StreamEngine.noteUnderrun at underrun sites

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Two StreamEngine instances (main + resonance) in Engine

**Files:**
- Modify: `engine/engine.h`, `engine/engine.cpp`
- Test: `tests/test_dsp.cpp` (append an Engine wiring assertion — it already constructs an Engine)

- [ ] **Step 1: Write the failing test** — append to `tests/test_dsp.cpp` (it already has `#include "engine.h"`)

```cpp
TEST_CASE("Engine ma oddelene main + resonance stream pooly") {
    ithaca::EngineConfig cfg;
    ithaca::Engine eng;
    REQUIRE(eng.init(cfg));
    CHECK(eng.mainRingsTotal()      == cfg.num_rings);
    CHECK(eng.resonanceRingsTotal() == cfg.resonance_num_rings);
    CHECK(eng.mainRingsTotal() != eng.resonanceRingsTotal());   // dva ruzne pooly
    CHECK(eng.mainStreamUnderrunRecent(1000.f) == false);
    CHECK(eng.resonanceStreamUnderrunRecent(1000.f) == false);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: FAIL — `mainRingsTotal`/`resonance_num_rings` undefined.

- [ ] **Step 3: Add config fields to `engine/engine.h`** — in `EngineConfig`, after `int num_rings = 288;` change the default to a main-sized value and add resonance fields:

Replace `inline ... num_rings = 288;` region — set `num_rings = 256` (main pool) and add:
```cpp
    int   num_rings             = 256;    // MAIN ring pool (>= max_voices)
    // -- Oddeleny streaming pro rezonanci (izolace od hlavnich hlasu) --
    int   resonance_num_rings      = 48;  // RESONANCE ring pool (>= max_resonance_voices)
    int   resonance_stream_threads = 4;   // workeri jen pro rezonancni pool
```
(Keep `stream_threads = 4` as the MAIN workers; keep `ring_capacity_frames = 8192` shared by both.)

- [ ] **Step 4: Split the member + add getters in `engine/engine.h`**

Replace the member `std::unique_ptr<StreamEngine> stream_;` with:
```cpp
    std::unique_ptr<StreamEngine>     stream_main_;
    std::unique_ptr<StreamEngine>     stream_resonance_;
```
Change `StreamEngine* streamEngine() { return stream_.get(); }` to:
```cpp
    StreamEngine* streamEngine() { return stream_main_.get(); }   // back-compat (main)
```
After `numRingsUsed()` declaration add per-pool getters (inline):
```cpp
    int  mainRingsUsed()      const noexcept { return stream_main_ ? stream_main_->numRingsUsed() : 0; }
    int  mainRingsTotal()     const noexcept { return stream_main_ ? stream_main_->numRings() : 0; }
    int  resonanceRingsUsed() const noexcept { return stream_resonance_ ? stream_resonance_->numRingsUsed() : 0; }
    int  resonanceRingsTotal()const noexcept { return stream_resonance_ ? stream_resonance_->numRings() : 0; }
    bool mainStreamUnderrunRecent(float ms)      const noexcept { return stream_main_ && stream_main_->underrunRecent(ms); }
    bool resonanceStreamUnderrunRecent(float ms) const noexcept { return stream_resonance_ && stream_resonance_->underrunRecent(ms); }
```

- [ ] **Step 5: Wire both instances in `engine/engine.cpp` `init()`** — replace the single-stream construction block (currently lines ~27-43, the `rings_min`/`stream_ = make_unique<StreamEngine>` / `pool_->setStreamEngine` / `recomputeRefillThreshold` / `stream_->start()` / resonance setup) with:

```cpp
    // Oddelene streaming pooly: hlavni hlasy vs rezonance (izolace ringu + workeru,
    // aby rezonancni burst pod pedalem nevyhladovel hlavni hlasy). Kazdy pool ma
    // vlastni ringy + workery + frontu.
    const int main_rings = (cfg.num_rings >= cfg.max_voices) ? cfg.num_rings : cfg.max_voices;
    const int res_rings  = (cfg.resonance_num_rings >= cfg.max_resonance_voices)
                         ? cfg.resonance_num_rings : cfg.max_resonance_voices;
    cfg_.num_rings = main_rings;
    cfg_.resonance_num_rings = res_rings;

    stream_main_ = std::make_unique<StreamEngine>(main_rings, cfg.ring_capacity_frames,
                                                  cfg.stream_threads);
    stream_resonance_ = std::make_unique<StreamEngine>(res_rings, cfg.ring_capacity_frames,
                                                       cfg.resonance_stream_threads);
    pool_->setStreamEngine(stream_main_.get());
    recomputeRefillThreshold();
    stream_main_->start();
    stream_resonance_->start();

    resonance_ = std::make_unique<ResonanceEngine>(cfg.max_resonance_voices);
    resonance_->setStrength(cfg.resonance_strength);
    resonance_->setStreamEngine(stream_resonance_.get());
    resonance_->setExciteDecayTimeMs(cfg.excite_decay_ms, cfg.block_size,
                                     (float)cfg.sample_rate);
```

- [ ] **Step 6: Update `recomputeRefillThreshold()` for both pools (`engine/engine.cpp`)** — replace its body:

```cpp
void Engine::recomputeRefillThreshold() noexcept {
    auto setFor = [this](StreamEngine* se) {
        if (!se) return;
        const int cap = se->capacityFrames();
        int thr = (std::max)(cap / 2, cfg_.block_size * 4);
        if (thr > cap - 64) thr = cap - 64;
        if (thr < 0) thr = 0;
        se->setRefillThresholdFrames(thr);
    };
    setFor(stream_main_.get());
    setFor(stream_resonance_.get());
}
```

- [ ] **Step 7: Update `numRingsUsed()` + destructor (`engine/engine.cpp`)**

`numRingsUsed()` → total across both pools:
```cpp
int Engine::numRingsUsed() const noexcept {
    int n = 0;
    if (stream_main_)      n += stream_main_->numRingsUsed();
    if (stream_resonance_) n += stream_resonance_->numRingsUsed();
    return n;
}
```
Destructor (`Engine::~Engine`): replace `if (stream_) stream_->stop();` with:
```cpp
    if (stream_main_)      stream_main_->stop();
    if (stream_resonance_) stream_resonance_->stop();
```

- [ ] **Step 8: Build + run**

Run: `cmake --build build -j 2>&1 | grep -iE "error|warning:" | head; ctest --test-dir build 2>&1 | grep -E "passed|failed"`
Expected: clean; new Engine wiring test passes; all green.

- [ ] **Step 9: Smoke**

Run: `( ./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca >/tmp/s.txt 2>&1 & echo $! >/tmp/p ); sleep 3; kill $(cat /tmp/p) 2>/dev/null && echo "ran ok"`
Expected: `ran ok` (bank loads, audio+MIDI start, two stream pools running).

- [ ] **Step 10: Commit**

```bash
git add engine/engine.h engine/engine.cpp tests/test_dsp.cpp
git commit -m "feat(engine): separate main + resonance stream pools (rings + workers)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Live resonance voice budget (atomic max_voices_)

**Files:**
- Modify: `engine/resonance/resonance_engine.h`, `engine/resonance/resonance_engine.cpp`
- Modify: `engine/engine.h` (forwarder)
- Test: `tests/test_resonance_engine.cpp` (existing)

- [ ] **Step 1: Write the failing test** — append to `tests/test_resonance_engine.cpp`

```cpp
TEST_CASE("ResonanceEngine: setMaxVoices / maxVoices round-trip + clamp") {
    ithaca::ResonanceEngine re(32);
    CHECK(re.maxVoices() == 32);
    re.setMaxVoices(8);
    CHECK(re.maxVoices() == 8);
    re.setMaxVoices(0);            // clamp na >= 1
    CHECK(re.maxVoices() == 1);
    re.setMaxVoices(999);          // clamp na <= 64
    CHECK(re.maxVoices() == 64);
}
```
(If `test_resonance_engine.cpp` lacks the include, ensure `#include "resonance/resonance_engine.h"` is present.)

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: FAIL — `setMaxVoices`/`maxVoices` undefined.

- [ ] **Step 3: Make `max_voices_` atomic + add API in `engine/resonance/resonance_engine.h`**

Change the member (line ~118) `int max_voices_ = kDefaultMaxResonanceVoices;` to:
```cpp
    std::atomic<int>    max_voices_{kDefaultMaxResonanceVoices};
```
(Ensure `#include <atomic>` is present in the header.) In `public:` (near `setStrength`) add:
```cpp
    // Zivy strop poctu soucasne znejicich rezonancnich hlasu (GUI slider, dle HW).
    // Meni jen kolik hlasu smi znit (enforceVoiceBudget) — bez resize poli/ringu.
    void setMaxVoices(int n) noexcept {
        if (n < 1)  n = 1;
        if (n > 64) n = 64;
        max_voices_.store(n, std::memory_order_relaxed);
    }
    int  maxVoices() const noexcept { return max_voices_.load(std::memory_order_relaxed); }
```

- [ ] **Step 4: Update reads in `engine/resonance/resonance_engine.cpp`**

In the constructor, where it stores the ctor arg into `max_voices_`, use the atomic store (the in-class `{...}` init already covers default; the ctor body likely does `max_voices_ = max_resonance_voices;` → change to `max_voices_.store(max_resonance_voices, std::memory_order_relaxed);` and clamp same as setMaxVoices, or just call a shared clamp). Simplest: in the ctor body set via the setter:
```cpp
    setMaxVoices(max_resonance_voices);
```
In `enforceVoiceBudget`, the `liveCount() > max_voices_` comparison must read the atomic — change to:
```cpp
    const int cap = max_voices_.load(std::memory_order_relaxed);
    while (liveCount() > cap) {
```
(and use `cap` in the loop condition).

- [ ] **Step 5: Add forwarder in `engine/engine.h`** — after `setExciteDecayMs` declaration add:
```cpp
    void setMaxResonanceVoices(int n) noexcept { if (resonance_) resonance_->setMaxVoices(n); }
    int  maxResonanceVoices()    const noexcept { return resonance_ ? resonance_->maxVoices() : 0; }
```

- [ ] **Step 6: Run to verify it passes**

Run: `cmake --build build -j && ctest --test-dir build -R test_resonance_engine`
Expected: PASS incl. the new round-trip/clamp case.

- [ ] **Step 7: Commit**

```bash
git add engine/resonance/resonance_engine.h engine/resonance/resonance_engine.cpp engine/engine.h tests/test_resonance_engine.cpp
git commit -m "feat(resonance): live max_voices_ (atomic) + setMaxVoices/maxVoices

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: GUI — live MAX RESONANCE slider

**Files:**
- Modify: `app/gui/voice_page.h`
- Modify: `app/gui/app_context.cpp` (apply persisted value at init)

- [ ] **Step 1: Make the param editable + live in `app/gui/voice_page.h`**

In `kParams`, change the MAX RESONANCE entry's `readonly` flag from `true` to `false`:
```cpp
        {"max_res",    "MAX RESONANCE",   1.f, 64.f,   32.f,   "%.0f",    false},
```
In `set(int i, float v)`, replace the `default: break;   // MAX RESONANCE readonly` case with an applied case:
```cpp
            default:  // MAX RESONANCE — zivy strop soucasne znejicich rezonanci
                ctx_.state.max_resonance_voices = (int)v;
                ctx_.engine.setMaxResonanceVoices((int)v);
                break;
```

- [ ] **Step 2: Apply persisted value at init in `app/gui/app_context.cpp`** — in `initFromState`, after `engine.init(cfg)` succeeds (the cfg already passes `max_resonance_voices` to construction, but ensure the live setter matches the persisted value), add right after the DSP-apply block:
```cpp
    engine.setMaxResonanceVoices(state.max_resonance_voices);
```

- [ ] **Step 3: Build + smoke**

Run: `cmake --build build --target ithaca-gui -j 2>&1 | grep -iE "error|warning:" | head; ( ./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca >/tmp/s.txt 2>&1 & echo $! >/tmp/p ); sleep 3; kill $(cat /tmp/p) 2>/dev/null && echo "ran ok"`
Expected: clean build, `ran ok`. Manual: VOICE page MAX RESONANCE slider is now draggable; dragging changes live resonance budget; value persists across restart.

- [ ] **Step 4: Commit**

```bash
git add app/gui/voice_page.h app/gui/app_context.cpp
git commit -m "feat(gui): MAX RESONANCE slider live-adjusts resonance budget

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: GUI — per-pool ring diagnostics with red-flash

**Files:**
- Modify: `app/gui/panel_indicators.cpp`

Add a small two-line readout in the indicator strip: `MAIN <used>/<total>` and `RESO <used>/<total>`, each flashing red for 4 s after an underrun on that pool.

- [ ] **Step 1: Add the readout in `app/gui/panel_indicators.cpp`** — inside the peak/col3 child (or after the RINGS tile), add (uses `theme::Colors`, already in scope):

```cpp
    // Stream ring diagnostika per-pool. Cislo zcervena na 4 s po underrunu
    // (engine drzi timestamp; vzor jako note lampy). Sliderem MAX RESONANCE
    // ladis rezonancni pool dokud RESO neprestane blikat.
    {
        const bool main_ur = ctx.engine.mainStreamUnderrunRecent(4000.f);
        const bool res_ur  = ctx.engine.resonanceStreamUnderrunRecent(4000.f);
        const ImU32 red = IM_COL32(0xd0,0x5a,0x4a,255);
        char b1[32], b2[32];
        std::snprintf(b1, sizeof(b1), "MAIN  %d/%d",
                      ctx.engine.mainRingsUsed(), ctx.engine.mainRingsTotal());
        std::snprintf(b2, sizeof(b2), "RESO  %d/%d",
                      ctx.engine.resonanceRingsUsed(), ctx.engine.resonanceRingsTotal());
        wdg::Eyebrow("RINGS");
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(main_ur ? red : Colors::muted));
        ImGui::TextUnformatted(b1);
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(res_ur ? red : Colors::muted));
        ImGui::TextUnformatted(b2);
        ImGui::PopStyleColor();
    }
```
(If `panel_indicators.cpp` lacks `#include <cstdio>`, it already uses `std::snprintf` elsewhere — keep as is.)

- [ ] **Step 2: Build + smoke (visual)**

Run: `cmake --build build --target ithaca-gui -j 2>&1 | grep -iE "error|warning:" | head; ( ./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca >/tmp/s.txt 2>&1 & echo $! >/tmp/p ); sleep 3; kill $(cat /tmp/p) 2>/dev/null && echo "ran ok"`
Expected: clean build, `ran ok`. Manual: MAIN/RESO ring counts visible; under heavy pedal play the RESO line flashes red on resonance underrun; lowering MAX RESONANCE stops the flashing; MAIN should not flash.

- [ ] **Step 3: Commit**

```bash
git add app/gui/panel_indicators.cpp
git commit -m "feat(gui): per-pool ring usage readout, red-flash on underrun

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (after all tasks)

- [ ] `cmake --build build -j` — clean, no warnings.
- [ ] `ctest --test-dir build` — 100% pass (existing + test_stream_engine + new cases).
- [ ] Reproduce the pedal-down underrun (the 63-underrun baseline run): RESO ring count flashes red; MAIN does not.
- [ ] Lower MAX RESONANCE slider until RESO stops flashing on this hardware; confirm value persists across restart.
- [ ] `grep -rn "stream_\b" engine/engine.cpp` → no stray references to the old single `stream_` member.
- [ ] `git status` clean except `imgui.ini`.

---

## Notes for the implementer

- **Worker isolation is the point:** each StreamEngine has its own worker threads, so a resonance refill burst occupies only resonance workers — main refills are never delayed. Don't collapse the pools back to shared workers.
- **`max_voices_` is just the budget cap** (active-count limit in `enforceVoiceBudget`) — making it atomic is enough; never resize `voices_` (always 128 slots) or the ring pool at runtime.
- **RAM:** main 256 + resonance 48 = 304 rings × 8192 × 2 × 4 B ≈ 20 MB (was ~18 MB with 288) — negligible delta.
- **Underrun is stamped by the consuming voice** (audio thread) and read by the GUI (atomic) — same lock-free pattern as `noteOnRecent`. No new threading concerns.
- Convolver / RAM-only looped resonance remain out of scope (separate future work).
