# EOF Dead-Code Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the unreachable EOF branch and hard guard from `Voice::process`, and make a clean end-of-sample log as `Info "END-OF-SAMPLE"` instead of `Warning "UNDERRUN"` (distinguishing it from a real buffer underrun); audio behavior (5 ms fade) unchanged.

**Architecture:** The stream worker only sets `ring_->eof_` on a short/zero read, but `Voice` caps every read request to the exact file end, so `eof_` never becomes true for the main voice — its EOF branch and hard guard are dead. A normally-ending streamed voice already exits via the underrun path (ring drains, `popFrame` fails) with a 5 ms fade. We delete the dead branches and split the underrun log into "clean end" (`file_request_off_ >= total_frames`) vs "real underrun". `ResonanceVoice` is untouched — its EOF branch is reachable via the `total_after<=0` start path.

**Tech Stack:** C++20, doctest, CMake. Logger has a synchronous subscriber API (`Logger::default_().addSubscriber`) used to assert log output in tests.

---

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `engine/voice/voice.cpp` | main voice render | delete dead EOF branch + hard guard; split underrun log into clean-end (Info) vs underrun (Warning) |
| `tests/test_streamed_interp.cpp` | Voice tests | new clean-end log test via log subscriber |

No header changes. `engine/voice/resonance_voice.cpp` is intentionally NOT changed (its EOF branch is reachable).

---

## Task 1: Voice — delete dead EOF code + distinguish clean-end log (TDD)

**Files:**
- Modify: `engine/voice/voice.cpp` (ring branch ~lines 226-266; hard guard ~lines 274-281)
- Modify: `tests/test_streamed_interp.cpp` (new test case + include)

### Background for the test

`tests/test_streamed_interp.cpp` already has (in an anonymous namespace): `TempDir`,
`writeRamp(dir, frames, header_sr, tag)` (writes stereo ramp, returns path),
`renderNote(Engine&, max_blocks)` (renders 256-frame blocks with 2 ms sleeps,
returns per-sample mono-L vector), and `streamCfg(sample_rate)` (EngineConfig:
sample_rate, block_size=256, midi 59-61, preload_ms=50). In the test, the engine
runs single-threaded (the test thread calls `eng.processBlock` directly), so the
voice's `Logger::default_().log(...)` calls fire subscribers synchronously —
deterministic. `LogEntry{timestamp_us, topic, sev, message}` and
`addSubscriber`/`clearSubscribers`/`setOutputMode` are in `util/log.h`
(ithaca_core), so no app/gui dependency is needed.

- [ ] **Step 1: Write the failing test FIRST**

In `tests/test_streamed_interp.cpp`, ensure `#include "util/log.h"` is present near
the top (after the existing includes; add it if missing). Then add this TEST_CASE
at the end of the file (after the last existing case):

```cpp
TEST_CASE("Streamed clean end loguje END-OF-SAMPLE (Info), ne UNDERRUN (Warning)") {
    // Cisty konec streamovaneho vzorku: ring se vyprazdni az kdyz uz byl cely
    // soubor vyzadan → ma se logovat jako Info "END-OF-SAMPLE", NE jako
    // Warning "UNDERRUN" (to je matouci). Engine bezi single-thread v testu,
    // takze subscriber fire je synchronni a deterministicky.
    TempDir tmp{"cleanend"};
    constexpr int frames = 20000;
    writeRamp(tmp.path, frames, 48000, "48");

    auto& lg = ithaca::log::Logger::default_();
    lg.setOutputMode(false, false);   // ticho behem testu
    lg.clearSubscribers();
    bool saw_end_of_sample_info = false;
    bool saw_underrun_warning   = false;
    lg.addSubscriber([&](const ithaca::log::LogEntry& e) {
        if (e.topic != "voice_end") return;
        if (e.sev == ithaca::log::Severity::Info &&
            e.message.find("END-OF-SAMPLE") != std::string::npos)
            saw_end_of_sample_info = true;
        if (e.sev == ithaca::log::Severity::Warning &&
            e.message.find("UNDERRUN") != std::string::npos)
            saw_underrun_warning = true;
    });

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    (void)renderNote(eng, 1200);

    lg.clearSubscribers();
    lg.setOutputMode(true, false);    // restore (jiny test by jinak zdedil ticho)

    CHECK(eng.activeVoices() == 0);
    CHECK(saw_end_of_sample_info);
    CHECK_FALSE(saw_underrun_warning);
}
```

- [ ] **Step 2: Run the test — verify it FAILS**

Run: `cmake --build build --target test_streamed_interp -j && ctest --test-dir build -R test_streamed_interp --output-on-failure`
Expected: the new case FAILS — `saw_end_of_sample_info` is false and `saw_underrun_warning` is true, because the current code logs the clean end as `Warning "UNDERRUN"` (there is no `END-OF-SAMPLE` log yet).

- [ ] **Step 3: Delete the dead EOF branch + add clean-end/underrun split**

In `engine/voice/voice.cpp`, the current ring-branch tail (the window-advance loop
through the output computation) is:

```cpp
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
```

Replace it with (removes the unreachable `else if (ring_->eof_...)` case;
splits the underrun log into clean-end vs underrun):

```cpp
            const int64_t target = (int64_t)position_;
            bool underrun = false;
            while (ring_lo_idx_ < target) {
                // posun okna: hi → lo, novy hi z ringu.
                ring_lo_l_ = ring_hi_l_; ring_lo_r_ = ring_hi_r_;
                float L, R;
                if (ring_->popFrame(L, R)) {
                    ring_hi_l_ = L; ring_hi_r_ = R;
                } else {
                    // Ring prazdny — bud cisty konec vzorku (cely soubor uz
                    // vyzadan) nebo skutecny underrun. Rozlisi se nize v logu;
                    // oba doznivaji stejnym 5ms fade.
                    underrun = true;
                    break;
                }
                ring_lo_idx_++;
            }
            if (underrun) {
                if (!underrun_fading_) {
                    underrun_fading_ = true;
                    underrun_gain_   = 1.f;
                    // Cisty konec: cely soubor uz byl vyzadan (file_request_off_
                    // dosahl konce) a ring je prazdny → legitimni konec, Info.
                    // Jinak worker nestihl dodat data → skutecny underrun, Warning.
                    const bool clean_end = (file_request_off_ >= (int64_t)total_frames);
                    if (clean_end) {
                        log::Logger::default_().log("voice_end", log::Severity::Info,
                            "END-OF-SAMPLE midi=%d pos=%lld total=%d", midi_,
                            (long long)position_, total_frames);
                    } else {
                        log::Logger::default_().log("voice_end", log::Severity::Warning,
                            "UNDERRUN midi=%d pos=%lld total=%d head=%d ring_avail=%d",
                            midi_, (long long)position_, total_frames,
                            head_frames, ring_->available());
                    }
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
```

- [ ] **Step 4: Delete the dead hard guard**

In `engine/voice/voice.cpp`, delete this entire block (it sits right after the
big `if/else if (ring_)/else` chain, before the onset/release envelope code):

```cpp
        // Hard guard na konec souboru (kdyby pos jsel mimo file.frames i v ringu).
        if ((int)position_ >= total_frames && ring_ &&
            ring_->eof_.load(std::memory_order_acquire) &&
            ring_->available() == 0) {
            // Spotrebovan posledni vzorek; cisto vypneme po tomto frame.
            log_end("hard_guard_file_end");
            active_ = false;
        }
```

Delete it completely (it requires `eof_`, which never becomes true for Voice).
The `log_end` lambda stays — it's still used by other paths (`release_ramp_zero`,
`underrun_fade_zero`, `fullyloaded_past_head`, `no_mic_or_empty_head`).

- [ ] **Step 5: Run the test — verify it PASSES**

Run: `cmake --build build --target test_streamed_interp -j && ctest --test-dir build -R test_streamed_interp --output-on-failure`
Expected: the new clean-end case PASSES (`saw_end_of_sample_info` true,
`saw_underrun_warning` false), and all other cases in the file still pass.

- [ ] **Step 6: Full suite (no regression)**

Run: `ctest --test-dir build 2>&1 | tail -3`
Expected: `100% tests passed ... out of 25` (24 prior + this new case).

- [ ] **Step 7: Commit**

```bash
git add engine/voice/voice.cpp tests/test_streamed_interp.cpp
git commit -m "refactor(voice): remove dead EOF branch + log clean-end as Info not UNDERRUN"
```

---

## Task 2: Final verification

**Files:** none (verification only)

- [ ] **Step 1: Confirm dead-code log tags are gone**

Run: `grep -rn "ring_eof_drained\|hard_guard_file_end" engine/`
Expected: ZERO matches (both deleted from voice.cpp). Note: `resonance_voice.cpp`
never had these tags, so nothing there to find.

- [ ] **Step 2: Confirm ResonanceVoice EOF branch was NOT touched**

Run: `grep -n "ring_->eof_.load" engine/voice/resonance_voice.cpp`
Expected: still present (the reachable EOF branch in ResonanceVoice's ring read is
intentionally kept). `git diff main -- engine/voice/resonance_voice.cpp` should
show NO changes.

- [ ] **Step 3: Confirm the new Info log exists and Voice no longer has the dead branch**

Run: `grep -n "END-OF-SAMPLE\|ring_->eof_.load" engine/voice/voice.cpp`
Expected: `END-OF-SAMPLE` present once; ZERO `ring_->eof_.load` in voice.cpp
(the only remaining eof reads were in the deleted branch + hard guard).

- [ ] **Step 4: Clean rebuild + full suite**

Run: `cmake --build build -j 2>&1 | tail -4 && ctest --test-dir build 2>&1 | tail -3`
Expected: build OK (incl. ithaca-gui), `100% tests passed ... out of 25`.

---

## Notes for the implementer

- **Why the EOF branch is dead (don't second-guess and "fix" it):** the worker sets
  `ring_->eof_` only on a short/zero read (`stream_engine.cpp:199-203, 219-228`),
  but Voice caps each request to the exact remaining frames
  (`voice.cpp:338-346`), so the worker always delivers exactly what was asked →
  never short-reads → `eof_` stays false for the main voice. The branch and hard
  guard can never execute. This was confirmed by investigation.
- **`file_request_off_`** is an existing Voice member tracking the file offset up to
  which the worker has been asked to read. At a clean end it equals (or exceeds)
  `total_frames`; during a mid-stream starvation it's still `< total_frames`. That's
  the clean-end vs underrun discriminator.
- **Audio is unchanged:** both clean-end and underrun still trigger the same
  `underrun_fading_` 5 ms fade and deactivate via `log_end("underrun_fade_zero")`.
  Only the *severity + message* of the one-time log at fade start differs.
- **Do NOT touch `resonance_voice.cpp`** — its EOF branch is reachable via the
  `total_after<=0` start path (`resonance_voice.cpp:74-78`) and is correct.
- **Honest test gap:** a genuine mid-stream underrun (worker starvation) is not
  reproduced deterministically in a unit test, so the `Warning "UNDERRUN"` path is
  verified by code review of the discriminator, not by an automated test. The
  clean-end path IS tested.
- **If the clean-end test is flaky** (a transient mid-stream underrun logs a spurious
  Warning before the clean end): raise `streamCfg`'s ring headroom (e.g. set
  `cfg.num_rings` / `cfg.ring_capacity_frames` higher) or the per-block budget —
  do NOT weaken `CHECK_FALSE(saw_underrun_warning)`. With 20000 frames + 2 ms
  block sleeps and default ring sizing the worker keeps up (same harness as
  `test_long_sample_stream`), so it should be stable.
- Do not commit `imgui.ini` (gitignored) or stray build dirs.
