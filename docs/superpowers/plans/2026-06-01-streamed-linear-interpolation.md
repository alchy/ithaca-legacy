# Streamed Linear Interpolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace nearest-neighbor sample reads in the streamed (ring) playback path of `Voice` and `ResonanceVoice` with linear interpolation (a two-frame "lo/hi sliding window"), including a smooth join across the head→ring (and preload_resonance→ring) seam and hold-last-sample at EOF, so non-48kHz samples play without stair-step aliasing.

**Architecture:** Today the ring branch keeps ONE cached frame at `floor(position_)` and discards the fractional part. We replace it with a `lo`/`hi` pair (frame at `floor(position_)` and the lookahead frame at `+1`) and output `lo*(1-frac) + hi*frac`. The seam is handled by seeding `lo` from the last RAM frame (head or preload_resonance) and `hi` from the first ring pop. At EOF we clamp `hi = lo` so the tail decays cleanly. At 48kHz (`pos_inc_ = 1.0`) `frac` is always 0, so output is bit-identical to before.

**Tech Stack:** C++20, doctest, CMake. Audio engine with a lock-free SPSC ring buffer (`RingHandle::popFrame`).

---

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `engine/voice/voice.h` | Voice member state | replace `ring_cur_*` with `ring_lo_*`/`ring_hi_*`/`ring_lo_idx_` |
| `engine/voice/voice.cpp` | main voice render | ring branch lo/hi interpolation + merge "last head sample" case + EOF clamp; reset in start()/hardStop() |
| `engine/voice/resonance_voice.h` | ResonanceVoice member state | same field replacement |
| `engine/voice/resonance_voice.cpp` | resonance render | preload_resonance interpolation + ring branch lo/hi + seam + EOF clamp; reset in start()/hardStop() |
| `tests/test_streamed_interp.cpp` | NEW test target | Voice @44.1k smoothness, seam, EOF hold, 48k regression |
| `tests/test_resonance_stream_sr.cpp` | existing | add a resonance smoothness assertion |
| `tests/CMakeLists.txt` | build | register test_streamed_interp |

**Current field block being replaced** — `engine/voice/voice.h` lines 106-114:
```cpp
    // -- SR konverze ve streamovane (ring) casti --
    // ring_cur_* = naposledy POPnuty frame z ringu (nearest-neighbor),
    // ring_cur_idx_ = jeho FILE-GLOBAL index. Z ringu popujeme dokud nedosahneme
    // floor(position_), tj. spotrebujeme ~pos_inc_ frames na vystupni frame.
    // Tim funguje i sample_sr != engine_sr (driv se cetl 1 frame/vystup bez
    // ohledu na pos_inc_ -> spatna vyska + position_ utikal pred ctenim -> predcasny EOF).
    float    ring_cur_l_   = 0.f;
    float    ring_cur_r_   = 0.f;
    int64_t  ring_cur_idx_ = -1;   // -1 = neseedovano
```

---

## Task 1: Voice — fields + start()/hardStop() reset (no behavior change yet)

This task only renames/extends the cache fields and their resets. The render loop still uses them in Task 2. Keeping it as a separate commit isolates the mechanical field change from the algorithm change.

**Files:**
- Modify: `engine/voice/voice.h:106-114`
- Modify: `engine/voice/voice.cpp` (hardStop ~line 77, start ~line 89-90 area)

- [ ] **Step 1: Replace the field block in voice.h**

Replace lines 106-114 (the block shown in File Structure above) with:

```cpp
    // -- SR konverze ve streamovane (ring) casti: lo/hi sliding window --
    // Pro lin. interpolaci drzime DVA sousedni framy: ring_lo_* na indexu
    // floor(position_) a ring_hi_* na floor(position_)+1 (lookahead). Vystup =
    // lo*(1-frac) + hi*frac. Okno se posouva popovanim z ringu dokud lo
    // nedosahne floor(position_). Seed pri prvnim vstupu: lo = posledni head
    // frame, hi = prvni ring pop (plynuly sev head->ring). Pri EOF clamp hi=lo
    // (hold last sample). Na 48k (pos_inc=1) je frac=0 → vystup = puvodni vzorek.
    float    ring_lo_l_   = 0.f;
    float    ring_lo_r_   = 0.f;
    float    ring_hi_l_   = 0.f;
    float    ring_hi_r_   = 0.f;
    int64_t  ring_lo_idx_ = -1;   // -1 = neseedovano
```

- [ ] **Step 2: Update hardStop() reset in voice.cpp**

In `Voice::hardStop()` (voice.cpp ~line 77), the last line before the closing brace is:
```cpp
    ring_cur_idx_ = -1;
```
Replace it with:
```cpp
    ring_lo_idx_ = -1;
```

- [ ] **Step 3: Update start() reset in voice.cpp**

In `Voice::start()`, find this line (added by the earlier ring fix, near the other stream-field resets after `file_request_off_ = 0;`):
```cpp
    ring_cur_l_ = 0.f; ring_cur_r_ = 0.f; ring_cur_idx_ = -1;
```
Replace with:
```cpp
    ring_lo_l_ = 0.f; ring_lo_r_ = 0.f;
    ring_hi_l_ = 0.f; ring_hi_r_ = 0.f;
    ring_lo_idx_ = -1;
```

- [ ] **Step 4: Build — expect a COMPILE ERROR in the render loop**

Run: `cmake --build build --target ithaca_core -j 2>&1 | tail -8`
Expected: FAIL — `ring_cur_l_` / `ring_cur_idx_` still referenced in `Voice::process` (lines ~218-247). This proves the fields are wired everywhere; Task 2 fixes the render loop. (This is an intentional intermediate state — do NOT commit yet. Proceed straight to Task 2 before committing.)

*(No commit in Task 1 — the tree doesn't compile until Task 2. Task 2 ends with the commit covering both.)*

---

## Task 2: Voice — lo/hi interpolation in the render loop

**Files:**
- Modify: `engine/voice/voice.cpp` (the head/ring branch block, lines ~201-249)

The current block (verbatim) is:
```cpp
        if (p0 < head_frames - 1) {
            // V hlave: linearni interpolace mezi p0 a p0+1 (jako pred fazi 4).
            float frac = (float)(position_ - (double)p0);
            int   p1 = p0 + 1;
            sL = head_data[p0 * 2]     * (1.f - frac) + head_data[p1 * 2]     * frac;
            sR = head_data[p0 * 2 + 1] * (1.f - frac) + head_data[p1 * 2 + 1] * frac;
            position_ += pos_inc_;
        } else if (p0 < head_frames) {
            // Posledni vzorek hlavy — bez interpolace pres hranici (TODO faze
            // 6/7: 1-frame lookbehind v ringu pro plynuly join).
            sL = head_data[p0 * 2];
            sR = head_data[p0 * 2 + 1];
            position_ += pos_inc_;
        } else if (ring_) {
            // Streamed: nearest-neighbor, ale konzumuj spravny pocet frames podle
            // pos_inc_. ring_cur_idx_ = FILE-GLOBAL index naposledy popnuteho
            // frame; popujeme dokud nedosahneme floor(position_).
            if (ring_cur_idx_ < 0) {
                ring_cur_idx_ = (int64_t)head_frames - 1;
                ring_cur_l_   = head_data[(size_t)(head_frames - 1) * 2];
                ring_cur_r_   = head_data[(size_t)(head_frames - 1) * 2 + 1];
            }
            const int64_t target = (int64_t)position_;
            bool ok = true;
            while (ring_cur_idx_ < target) {
                float L, R;
                if (!ring_->popFrame(L, R)) { ok = false; break; }
                ring_cur_l_ = L; ring_cur_r_ = R; ring_cur_idx_++;
            }
            if (!ok) {
                if (ring_->eof_.load(std::memory_order_acquire)) {
                    log_end("ring_eof_drained");
                    active_ = false;
                    break;
                }
                if (!underrun_fading_) {
                    underrun_fading_ = true;
                    underrun_gain_   = 1.f;
                    log::Logger::default_().log("voice_end", log::Severity::Warning,
                        "UNDERRUN midi=%d pos=%lld total=%d head=%d ring_avail=%d "
                        "ring_eof=%d", midi_, (long long)position_, total_frames,
                        head_frames, ring_->available(),
                        (int)ring_->eof_.load(std::memory_order_relaxed));
                }
                sL = 0.f; sR = 0.f;
            } else {
                sL = ring_cur_l_; sR = ring_cur_r_;
            }
            position_ += pos_inc_;
        } else {
```

- [ ] **Step 1: Replace that block (head-interp branch unchanged, merge last-head into ring, add lo/hi interpolation)**

Replace the whole block above (from `if (p0 < head_frames - 1) {` through the `} else {` that precedes the FullyLoaded-past-head comment) with:

```cpp
        if (p0 < head_frames - 1) {
            // V hlave: linearni interpolace mezi p0 a p0+1 (jako pred fazi 4).
            float frac = (float)(position_ - (double)p0);
            int   p1 = p0 + 1;
            sL = head_data[p0 * 2]     * (1.f - frac) + head_data[p1 * 2]     * frac;
            sR = head_data[p0 * 2 + 1] * (1.f - frac) + head_data[p1 * 2 + 1] * frac;
            position_ += pos_inc_;
        } else if (ring_) {
            // Streamed: lin. interpolace pres lo/hi okno. Posledni head frame
            // (p0 == head_frames-1) sem spada take — seed lo = posledni head
            // frame, hi = prvni ring pop → plynuly sev head->ring.
            if (ring_lo_idx_ < 0) {
                ring_lo_idx_ = (int64_t)head_frames - 1;
                ring_lo_l_   = head_data[(size_t)(head_frames - 1) * 2];
                ring_lo_r_   = head_data[(size_t)(head_frames - 1) * 2 + 1];
                // hi = prvni ring frame (lookahead). Kdyz neni, vyresi to nize
                // posun okna / EOF clamp.
                float L, R;
                if (ring_->popFrame(L, R)) { ring_hi_l_ = L; ring_hi_r_ = R; }
                else { ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_; }
            }
            const int64_t target = (int64_t)position_;
            bool underrun = false;
            while (ring_lo_idx_ < target) {
                // posun okna: hi → lo, novy hi z ringu.
                ring_lo_l_ = ring_hi_l_; ring_lo_r_ = ring_hi_r_;
                float L, R;
                if (ring_->popFrame(L, R)) {
                    ring_hi_l_ = L; ring_hi_r_ = R;
                } else if (ring_->eof_.load(std::memory_order_acquire)) {
                    // EOF: clamp hi=lo (hold last sample), prestan posouvat.
                    ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_;
                    ring_lo_idx_++;
                    // pokud uz jsme za koncem souboru, ukonci cisto.
                    if (ring_lo_idx_ >= (int64_t)total_frames - 1) {
                        log_end("ring_eof_drained");
                        active_ = false;
                    }
                    break;
                } else {
                    underrun = true;
                    break;
                }
                ring_lo_idx_++;
            }
            if (underrun) {
                if (!underrun_fading_) {
                    underrun_fading_ = true;
                    underrun_gain_   = 1.f;
                    log::Logger::default_().log("voice_end", log::Severity::Warning,
                        "UNDERRUN midi=%d pos=%lld total=%d head=%d ring_avail=%d "
                        "ring_eof=%d", midi_, (long long)position_, total_frames,
                        head_frames, ring_->available(),
                        (int)ring_->eof_.load(std::memory_order_relaxed));
                }
                sL = 0.f; sR = 0.f;
            } else {
                float frac = (float)(position_ - (double)ring_lo_idx_);
                if (frac < 0.f) frac = 0.f;
                if (frac > 1.f) frac = 1.f;
                sL = ring_lo_l_ * (1.f - frac) + ring_hi_l_ * frac;
                sR = ring_lo_r_ * (1.f - frac) + ring_hi_r_ * frac;
            }
            position_ += pos_inc_;
        } else {
```

- [ ] **Step 2: Build ithaca_core**

Run: `cmake --build build --target ithaca_core -j 2>&1 | tail -5`
Expected: builds with no errors (all `ring_cur_*` references gone; `ring_lo_*`/`ring_hi_*` resolved).

- [ ] **Step 3: Full build + existing suite (no regression yet expected)**

Run: `cmake --build build -j && ctest --test-dir build 2>&1 | tail -3`
Expected: `100% tests passed ... out of 23`. The existing `test_long_sample_stream` (48k ramp) and `test_sample_rate_and_reload` (96k) must still pass — at 48k frac=0 so output is unchanged; the 96k duration test still holds because consumption count is unchanged.

- [ ] **Step 4: Commit (Tasks 1 + 2 together)**

```bash
git add engine/voice/voice.h engine/voice/voice.cpp
git commit -m "feat(voice): linear interpolation in streamed ring read (lo/hi window + seam + EOF hold)"
```

---

## Task 3: Voice — TDD smoothness / seam / EOF / 48k regression tests

**Files:**
- Create: `tests/test_streamed_interp.cpp`
- Modify: `tests/CMakeLists.txt`

These tests drive a full `Engine` with a streamed ramp WAV (mirrors `tests/test_long_sample_stream.cpp` helpers). A linear ramp is the ideal probe: correct interpolation yields a near-linear output slope; nearest-neighbor yields stair-steps (repeated then jumped values).

- [ ] **Step 1: Write the test file**

Create `tests/test_streamed_interp.cpp`:

```cpp
// tests/test_streamed_interp.cpp
// -------------------------------
// Faze 6/7: lin. interpolace ve streamovane (ring) casti. Ramp WAV je idealni
// sonda — spravna interpolace da temer linearni vystupni sklon; nearest-neighbor
// da schody (opakovane pak preskocene hodnoty). Testy: hladkost @44.1k, plynuly
// sev head->ring, EOF hold-last-sample, a 48k regrese (frac=0 → beze zmeny).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine.h"
#include "io/wav_writer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace ithaca;
namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        path = fs::temp_directory_path() / ("ithaca_interp_" + tag);
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

// Napis stereo ramp WAV: frame i má L=R=i/frames (0..~1), dany header SR.
// Nazev m060-vel1-fNN.wav (NN je jen kosmeticky tag; rozhoduje header SR).
std::string writeRamp(const fs::path& dir, int frames, int header_sr,
                      const char* tag) {
    std::vector<float> s((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        float v = (float)i / (float)frames;
        s[(size_t)i * 2]     = v;
        s[(size_t)i * 2 + 1] = v;
    }
    std::string p = (dir / (std::string("m060-vel1-f") + tag + ".wav")).string();
    REQUIRE(writeWavStereo16(p, s, header_sr));
    return p;
}

// Vyrenderuj notu 60 do jednoho souvisleho L-bufferu (mono L), s ~2ms sleep
// mezi bloky aby stream worker stihal plnit ring. Vrati posbiranych vzorku.
std::vector<float> renderNote(Engine& eng, int max_blocks) {
    constexpr int block = 256;
    std::vector<float> L(block), R(block), out;
    out.reserve((size_t)max_blocks * block);
    bool started = false;
    for (int b = 0; b < max_blocks; ++b) {
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        eng.processBlock(L.data(), R.data(), block);
        for (int i = 0; i < block; ++i) out.push_back(L[i]);
        if (eng.activeVoices() >= 1) started = true;
        if (started && eng.activeVoices() == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return out;
}

EngineConfig streamCfg(int sample_rate) {
    EngineConfig cfg;
    cfg.sample_rate = sample_rate;   // engine SR = 48000 vzdy v testech nize
    cfg.block_size  = 256;
    cfg.midi_from   = 59;
    cfg.midi_to     = 61;
    cfg.preload_ms  = 50;            // head ~ kratky → zbytek Streamed
    return cfg;
}
} // namespace

TEST_CASE("Streamed 44.1k ramp je hladky (lin. interpolace, ne schody)") {
    // Header 44100, engine 48000 → pos_inc ≈ 0.91875 (upsampling). Sousedni
    // vystupni framy v ustalenem stavu se lisi ~ pos_inc * krok_rampy. Nearest
    // by daval bloky stejnych hodnot (frac zahozen) → vyssi pocet nulovych
    // diferenci mezi sousedy. Test: zadne dlouhe plato stejnych hodnot.
    TempDir tmp{"sr441"};
    constexpr int frames = 44100;    // 1 s @44.1k
    writeRamp(tmp.path, frames, 44100, "44");

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> out = renderNote(eng, 1200);

    // Najdi souvisly rostouci usek (po onsetu, pred koncem). Spocti, kolik
    // sousednich dvojic ma PRESNE stejnou hodnotu (=stair tread). U lin.
    // interpolace s pos_inc!=1 by melo byt temer 0 takovych (kazdy krok posune
    // o ~0.918 vzorku → ruzna interpolovana hodnota). U nearest by byly bloky.
    int equal_pairs = 0, rising_pairs = 0;
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i] > 0.001f && out[i-1] > 0.001f && out[i] < 0.99f) {
            if (out[i] == out[i-1]) equal_pairs++;
            else if (out[i] > out[i-1]) rising_pairs++;
        }
    }
    REQUIRE(rising_pairs > 1000);                 // máme dost dat z rostouci rampy
    // Lin. interpolace: prakticky zadne presne-stejne sousedni vzorky.
    // (Tolerance: < 1 % rostoucich, kvuli okrajum/blokovym svum.)
    CHECK(equal_pairs < rising_pairs / 100);
}

TEST_CASE("Streamed sev head->ring nema diskontinuitu") {
    // Lin. ramp @48k (pos_inc=1) — sev mezi head a ring musi byt plynuly:
    // zadny skok mezi sousednimi vzorky vyrazne vetsi nez normalni krok rampy.
    TempDir tmp{"seam"};
    constexpr int frames = 48000;
    writeRamp(tmp.path, frames, 48000, "48");

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> out = renderNote(eng, 1200);

    // Normalni krok rampy na vystupu = vel_gain * (1/frames). vel_gain pro
    // vel127 = 1.0, pan default → cos(pi/4)≈0.707. Krok ≈ 0.707/48000 ≈ 1.47e-5.
    // Hledame max skok mezi sousednimi NENULOVYMI vzorky v rostoucim usek;
    // nesmi byt radove vetsi (sev by delal ~2x krok max, ne 100x).
    float max_jump = 0.f;
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i] > 0.01f && out[i-1] > 0.01f && out[i] < 0.98f) {
            float d = std::fabs(out[i] - out[i-1]);
            if (d > max_jump) max_jump = d;
        }
    }
    // Tolerance: krok ~1.5e-5; povolime do 1e-3 (rezerva na blokove hranice),
    // coz je porad ~70x mensi nez kdyby sev pohltil cely head frame.
    CHECK(max_jump < 1e-3f);
}

TEST_CASE("Streamed EOF: hold last sample, cisty konec") {
    // Kratky streamovany ramp @48k dohraje az do konce, deaktivuje se, a
    // posledni nenulovy vzorek je blizko konce rampy (drzeny, ne uriznuty na 0).
    TempDir tmp{"eof"};
    constexpr int frames = 20000;
    writeRamp(tmp.path, frames, 48000, "48");

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> out = renderNote(eng, 1200);

    CHECK(eng.activeVoices() == 0);               // dohral a deaktivoval se
    // Posledni nenulovy vzorek odpovida vrcholu rampy (drzeny last sample),
    // tj. blizko vel_gain*pan*~1.0 ≈ 0.707. Pozadujeme aspon 0.5.
    float last_nonzero = 0.f;
    for (float v : out) if (v > 0.001f) last_nonzero = v;
    CHECK(last_nonzero > 0.5f);
}

TEST_CASE("Streamed 48k regrese: vystup = vstup (frac=0)") {
    // pos_inc=1.0 → frac vzdy 0 → interpolace vraci presne lo. Vystup musi
    // monotonne rust po rampe az k vrcholu (jako pred zmenou). Overuje, ze
    // jsme nezmenili chovani na produkcnich 48k bankach.
    TempDir tmp{"sr48"};
    constexpr int frames = 48000;
    writeRamp(tmp.path, frames, 48000, "48");

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> out = renderNote(eng, 1200);

    float peak = 0.f;
    for (float v : out) peak = (std::max)(peak, v);
    CHECK(peak > 0.6f);                            // dorazili k vrcholu rampy
    CHECK(eng.activeVoices() == 0);                // a cisto dohrali
}
```

- [ ] **Step 2: Register the test target in tests/CMakeLists.txt**

After the `test_resonance_engine` registration block (mirror the existing pattern), add:

```cmake
add_executable(test_streamed_interp test_streamed_interp.cpp)
target_link_libraries(test_streamed_interp PRIVATE ithaca_core doctest)
add_test(NAME test_streamed_interp COMMAND test_streamed_interp)
```

- [ ] **Step 3: Build + run the new test — expect PASS (fix already in from Task 2)**

Run: `cmake --build build --target test_streamed_interp -j && ctest --test-dir build -R test_streamed_interp --output-on-failure`
Expected: PASS (4 cases). The fix from Task 2 is already in place, so these confirm it.

- [ ] **Step 4: Prove the smoothness test is meaningful (temporary revert check)**

To confirm the 44.1k smoothness test actually discriminates the fix, temporarily make the ring output nearest-neighbor: in `engine/voice/voice.cpp` ring branch, temporarily replace the interpolation lines
```cpp
                sL = ring_lo_l_ * (1.f - frac) + ring_hi_l_ * frac;
                sR = ring_lo_r_ * (1.f - frac) + ring_hi_r_ * frac;
```
with
```cpp
                sL = ring_lo_l_; sR = ring_lo_r_;   // TEMP nearest-neighbor
```
Build and run ONLY `test_streamed_interp`. Expected: the "44.1k ramp je hladky" case FAILS (equal_pairs high). Then REVERT (restore the two interpolation lines), rebuild, confirm PASS again. Verify with `git diff engine/voice/voice.cpp` that the file matches Task 2's committed state (no TEMP line remains).

- [ ] **Step 5: Full suite**

Run: `ctest --test-dir build 2>&1 | tail -3`
Expected: `100% tests passed ... out of 24`.

- [ ] **Step 6: Commit**

```bash
git add tests/test_streamed_interp.cpp tests/CMakeLists.txt
git commit -m "test(voice): streamed linear interpolation (smoothness, seam, EOF, 48k regression)"
```

---

## Task 4: ResonanceVoice — fields + start()/hardStop() reset

**Files:**
- Modify: `engine/voice/resonance_voice.h:135-138`
- Modify: `engine/voice/resonance_voice.cpp` (start() reset line, hardStop ~line 147)

Current field block — `engine/voice/resonance_voice.h:135-138`:
```cpp
    // -- SR konverze ve streamovane (ring) casti (viz Voice). --
    float    ring_cur_l_   = 0.f;
    float    ring_cur_r_   = 0.f;
    int64_t  ring_cur_idx_ = -1;
```

- [ ] **Step 1: Replace the field block in resonance_voice.h**

Replace those 4 lines with:
```cpp
    // -- SR konverze ve streamovane (ring) casti: lo/hi sliding window (viz Voice). --
    float    ring_lo_l_   = 0.f;
    float    ring_lo_r_   = 0.f;
    float    ring_hi_l_   = 0.f;
    float    ring_hi_r_   = 0.f;
    int64_t  ring_lo_idx_ = -1;
```

- [ ] **Step 2: Update start() reset in resonance_voice.cpp**

In `ResonanceVoice::start()` find (added by earlier ring fix, after `file_request_off_ = 0;`):
```cpp
    ring_cur_l_ = 0.f; ring_cur_r_ = 0.f; ring_cur_idx_ = -1;
```
Replace with:
```cpp
    ring_lo_l_ = 0.f; ring_lo_r_ = 0.f;
    ring_hi_l_ = 0.f; ring_hi_r_ = 0.f;
    ring_lo_idx_ = -1;
```

- [ ] **Step 3: Update hardStop() reset in resonance_voice.cpp**

In `ResonanceVoice::hardStop()` the last line before the closing brace is:
```cpp
    ring_cur_idx_ = -1;
```
Replace with:
```cpp
    ring_lo_idx_ = -1;
```

- [ ] **Step 4: Build — expect compile error in process() (fixed in Task 5)**

Run: `cmake --build build --target ithaca_core -j 2>&1 | tail -6`
Expected: FAIL — `ring_cur_*` still referenced in `ResonanceVoice::process`. Intentional intermediate state; do NOT commit. Proceed to Task 5.

*(No commit — tree compiles again after Task 5.)*

---

## Task 5: ResonanceVoice — interpolation in preload_resonance + ring branch

**Files:**
- Modify: `engine/voice/resonance_voice.cpp` (streamed block, lines ~177-229)

Current streamed block (verbatim, inside `if (streamed) {`):
```cpp
            if (p0 < res_start) {
                // Pod resonance_start (nemelo by se stavat — start nas nastavil
                // primo na res_start). Defensive: tichy vzorek + posun.
                sL = 0.f; sR = 0.f;
                position_ += pos_inc_;
            } else if (p0 < res_end && res_frames > 0) {
                // V preload_resonance regionu. Nearest-neighbor (jako Voice
                // ve streamed casti). TODO faze 6/7: linearni interpolace
                // pres preload/ring boundary.
                int local = p0 - res_start;
                if (local >= res_frames) local = res_frames - 1;  // safety clamp
                sL = res_data[local * 2];
                sR = res_data[local * 2 + 1];
                position_ += pos_inc_;
            } else if (ring_) {
                if (ring_cur_idx_ < 0) {
                    ring_cur_idx_ = res_end - 1;
                    if (res_frames > 0) {
                        ring_cur_l_ = res_data[(size_t)(res_frames - 1) * 2];
                        ring_cur_r_ = res_data[(size_t)(res_frames - 1) * 2 + 1];
                    }
                }
                const int64_t target = (int64_t)position_;
                bool ok = true;
                while (ring_cur_idx_ < target) {
                    float L, R;
                    if (!ring_->popFrame(L, R)) { ok = false; break; }
                    ring_cur_l_ = L; ring_cur_r_ = R; ring_cur_idx_++;
                }
                if (!ok) {
                    if (ring_->eof_.load(std::memory_order_acquire)) {
                        active_ = false;
                        break;
                    }
                    if (!underrun_fading_) {
                        underrun_fading_ = true;
                        underrun_gain_   = 1.f;
                        LOG_RT_WARN("resonance_voice", "underrun midi=%d", midi_);
                    }
                    sL = 0.f; sR = 0.f;
                } else {
                    sL = ring_cur_l_; sR = ring_cur_r_;
                }
                position_ += pos_inc_;
            } else {
```

- [ ] **Step 1: Replace the streamed block**

Replace the block above (from `if (p0 < res_start) {` through the `} else {` that precedes the "Streamed bez ringu" comment) with:

```cpp
            if (p0 < res_start) {
                // Pod resonance_start (nemelo by se stavat — start nas nastavil
                // primo na res_start). Defensive: tichy vzorek + posun.
                sL = 0.f; sR = 0.f;
                position_ += pos_inc_;
            } else if (p0 < res_end - 1 && res_frames > 1) {
                // V preload_resonance regionu (RAM): lin. interpolace mezi
                // local a local+1. Posledni frame regionu spada do ring vetve
                // (sev preload->ring).
                int   local = p0 - res_start;
                float frac  = (float)(position_ - (double)p0);
                sL = res_data[local * 2]     * (1.f - frac) + res_data[(local + 1) * 2]     * frac;
                sR = res_data[local * 2 + 1] * (1.f - frac) + res_data[(local + 1) * 2 + 1] * frac;
                position_ += pos_inc_;
            } else if (ring_) {
                // Streamed: lin. interpolace pres lo/hi okno. Seed lo = posledni
                // preload_resonance frame, hi = prvni ring pop → plynuly sev.
                if (ring_lo_idx_ < 0) {
                    ring_lo_idx_ = res_end - 1;
                    if (res_frames > 0) {
                        ring_lo_l_ = res_data[(size_t)(res_frames - 1) * 2];
                        ring_lo_r_ = res_data[(size_t)(res_frames - 1) * 2 + 1];
                    }
                    float L, R;
                    if (ring_->popFrame(L, R)) { ring_hi_l_ = L; ring_hi_r_ = R; }
                    else { ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_; }
                }
                const int64_t target = (int64_t)position_;
                bool underrun = false;
                while (ring_lo_idx_ < target) {
                    ring_lo_l_ = ring_hi_l_; ring_lo_r_ = ring_hi_r_;
                    float L, R;
                    if (ring_->popFrame(L, R)) {
                        ring_hi_l_ = L; ring_hi_r_ = R;
                    } else if (ring_->eof_.load(std::memory_order_acquire)) {
                        ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_;
                        ring_lo_idx_++;
                        if (ring_lo_idx_ >= (int64_t)total_frames - 1) {
                            active_ = false;
                        }
                        break;
                    } else {
                        underrun = true;
                        break;
                    }
                    ring_lo_idx_++;
                }
                if (underrun) {
                    if (!underrun_fading_) {
                        underrun_fading_ = true;
                        underrun_gain_   = 1.f;
                        LOG_RT_WARN("resonance_voice", "underrun midi=%d", midi_);
                    }
                    sL = 0.f; sR = 0.f;
                } else {
                    float frac = (float)(position_ - (double)ring_lo_idx_);
                    if (frac < 0.f) frac = 0.f;
                    if (frac > 1.f) frac = 1.f;
                    sL = ring_lo_l_ * (1.f - frac) + ring_hi_l_ * frac;
                    sR = ring_lo_r_ * (1.f - frac) + ring_hi_r_ * frac;
                }
                position_ += pos_inc_;
            } else {
```

Note: the `if (!active_) break;`/end-guard logic after this block is unchanged. When the EOF branch sets `active_ = false`, the existing post-loop handling deactivates cleanly (same as before).

- [ ] **Step 2: Build ithaca_core**

Run: `cmake --build build --target ithaca_core -j 2>&1 | tail -5`
Expected: builds with no errors.

- [ ] **Step 3: Full build + suite**

Run: `cmake --build build -j && ctest --test-dir build 2>&1 | tail -3`
Expected: `100% tests passed ... out of 24`. Existing `test_resonance_stream_sr` (96k resonance) still passes — consumption count unchanged, just interpolated.

- [ ] **Step 4: Commit (Tasks 4 + 5)**

```bash
git add engine/voice/resonance_voice.h engine/voice/resonance_voice.cpp
git commit -m "feat(resonance): linear interpolation in streamed/preload resonance read"
```

---

## Task 6: ResonanceVoice — smoothness assertion in existing test

**Files:**
- Modify: `tests/test_resonance_stream_sr.cpp`

The existing test plays a 96k streamed resonance and checks duration. Add a smoothness probe (resonance output should not have long flat treads from nearest-neighbor).

- [ ] **Step 1: Read the existing test to find where output is collected**

Run: `sed -n '1,140p' tests/test_resonance_stream_sr.cpp`
Identify the loop that renders blocks and the buffer holding rendered samples (e.g. an accumulated L/peak). The test renders note `played` (72) which excites resonance on N=60.

- [ ] **Step 2: Add a smoothness assertion**

Inside the existing TEST_CASE, accumulate the per-block max-magnitude resonance samples into a vector during the render loop (if not already), then after the loop add a check that the resonance signal is not stair-stepped. Concretely, collect the L output per sample into a `std::vector<float> reso_out;` across blocks (push `L[i]` each sample), and after the loop add:

```cpp
    // Hladkost: streamovana rezonance @96k (pos_inc=2.0) nesmi mit dlouha plata
    // stejnych sousednich vzorku (nearest-neighbor artefakt). Spocti pomer
    // presne-stejnych sousednich dvojic mezi nenulovymi vzorky.
    int eq = 0, nz = 0;
    for (size_t i = 1; i < reso_out.size(); ++i) {
        if (std::fabs(reso_out[i]) > 1e-4f && std::fabs(reso_out[i-1]) > 1e-4f) {
            ++nz;
            if (reso_out[i] == reso_out[i-1]) ++eq;
        }
    }
    REQUIRE(nz > 200);
    CHECK(eq < nz / 10);   // <10 % stejnych → interpolovano, ne schody
```

If the existing test does not already collect per-sample output, add `std::vector<float> reso_out;` before the render loop and `for (int i=0;i<block;++i) reso_out.push_back(L[i]);` inside it (using whatever the block buffer variable is named — inspect in Step 1).

- [ ] **Step 3: Build + run that test**

Run: `cmake --build build --target test_resonance_stream_sr -j && ctest --test-dir build -R test_resonance_stream_sr --output-on-failure`
Expected: PASS.

- [ ] **Step 4: Full suite**

Run: `ctest --test-dir build 2>&1 | tail -3`
Expected: `100% tests passed ... out of 24`.

- [ ] **Step 5: Commit**

```bash
git add tests/test_resonance_stream_sr.cpp
git commit -m "test(resonance): assert streamed resonance is interpolated (no stair-steps)"
```

---

## Task 7: Final verification

**Files:** none (verification only)

- [ ] **Step 1: Clean rebuild + full suite**

Run: `cmake --build build -j 2>&1 | tail -4 && ctest --test-dir build 2>&1 | tail -3`
Expected: build OK (incl. ithaca-gui if vendored), `100% tests passed ... out of 24`.

- [ ] **Step 2: Confirm no leftover `ring_cur_` references**

Run: `grep -rn "ring_cur_" engine/ tests/`
Expected: ZERO matches (all renamed to ring_lo_/ring_hi_).

- [ ] **Step 3: Confirm no TEMP debug code left**

Run: `grep -rn "TEMP nearest" engine/`
Expected: ZERO matches.

- [ ] **Step 4: Manual smoke (optional, if a 44.1k bank exists)**

Run: `./build/ithaca-cli --play <path-to-44100Hz-bank> --midi-in 0`
Expected: notes play at correct pitch and sound smooth (no gritty aliasing on sustained streamed tail). At 48k banks: unchanged.

---

## Notes for the implementer

- **48kHz is the regression guard.** `pos_inc_ = 1.0` ⇒ `frac = position_ - floor(position_) = 0` ⇒ interpolation returns exactly `ring_lo_*`. Output must be bit-identical to the old nearest-neighbor path on 48k banks. The 48k tests must never change behavior.
- **The `hi` frame is always valid after seed.** Seeding fills it from the first ring pop (or clamps to `lo` if the ring is momentarily empty at seed time). The window-advance keeps it filled; EOF clamps it to `lo`. No separate "hi valid" flag needed.
- **Why merge the "last head sample" case:** to interpolate across the head→ring seam we need `lo` = last head frame and `hi` = first ring frame in the SAME branch. Keeping a separate non-interpolated `p0 == head_frames-1` case would leave a one-sample discontinuity at the seam (the original TODO).
- **Underrun vs EOF are distinct.** Underrun (ring empty, not EOF) keeps the existing fast-fade-to-silence. EOF holds the last sample then deactivates. Don't collapse them.
- **Do not commit** `imgui.ini` (now gitignored anyway) or any stray build dirs.
- Tests run a real `Engine` + stream worker, so they need the per-block sleep to let the worker fill the ring (mirrors `test_long_sample_stream.cpp`). If a test is flaky, increase the block budget or sleep — do not weaken the smoothness ratio thresholds.
