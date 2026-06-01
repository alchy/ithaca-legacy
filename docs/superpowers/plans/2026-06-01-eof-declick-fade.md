# EOF Safety Declick Fade Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the "hold last sample" behavior at streamed end-of-file (ring drained + `eof_` flag) in `Voice` and `ResonanceVoice` with a short fade-to-zero declick (reusing the existing 5 ms underrun fade), so a sample that ends on a non-zero frame deactivates without a DC-step click.

**Architecture:** The voices already have an `underrun_fading_` envelope (5 ms = `kUnderrunFadeMs`) that multiplies a falling gain over the output and self-deactivates when it reaches zero. We route the natural-EOF case through that same machinery instead of holding the last sample. `hi=lo` is clamped so the fade descends from the real last value. The underrun (non-EOF) path is unchanged.

**Tech Stack:** C++20, doctest, CMake. Real-time audio voice render loop.

---

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `engine/voice/voice.cpp` | main voice render | EOF case in ring branch: hold → trigger underrun fade |
| `engine/voice/resonance_voice.cpp` | resonance render | EOF case in ring branch: silent deactivate → trigger underrun fade |
| `tests/test_streamed_interp.cpp` | Voice tests | new declick test; fix the existing "hold last sample" EOF test |
| `tests/test_resonance_stream_sr.cpp` | resonance test | confirm/keep clean EOF deactivation |

No header changes (all fields/constants already exist: `underrun_fading_`, `underrun_gain_`, `underrun_step_`, `kUnderrunFadeMs`).

---

## Task 1: Voice — EOF triggers declick fade instead of hold

**Files:**
- Modify: `engine/voice/voice.cpp` (ring branch EOF case, lines ~232-241)

The current EOF case (verbatim) inside the `while (ring_lo_idx_ < target)` loop:
```cpp
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
```

- [ ] **Step 1: Write the failing declick test FIRST**

In `tests/test_streamed_interp.cpp`, the helper `writeRamp(dir, frames, header_sr, tag)` writes a stereo ramp `L=R=i/frames` (rises 0→~1). For a declick test we need a sample that ends on a NON-ZERO value, so add a constant-amplitude helper next to `writeRamp` (inside the same anonymous namespace, after the `writeRamp` function):

```cpp
// Stereo konstantni amplituda (NE doznivajici) — konci na nenulovem framu,
// takze "hold last sample" by udelalo DC step → lup. Pro EOF declick test.
std::string writeConst(const fs::path& dir, int frames, int header_sr,
                       float amp, const char* tag) {
    std::vector<float> s((size_t)frames * 2, amp);
    std::string p = (dir / (std::string("m060-vel1-f") + tag + ".wav")).string();
    REQUIRE(writeWavStereo16(p, s, header_sr));
    return p;
}
```

Then add this new TEST_CASE (place it right after the existing "Streamed EOF: hold last sample, cisty konec" case):

```cpp
TEST_CASE("Streamed EOF declick: nenulovy konec fadne k nule (zadny DC step)") {
    // Konstantni amplituda 0.5 @48k, streamovany. Po EOF musi vystup PLYNULE
    // klesnout k ~0 (declick fade ~5ms), NE skocit z ~0.5*gain na 0 (DC step).
    TempDir tmp{"eofdeclick"};
    constexpr int frames = 20000;
    writeConst(tmp.path, frames, 48000, 0.5f, "48");

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> out = renderNote(eng, 1200);

    CHECK(eng.activeVoices() == 0);                 // hlas se deaktivoval

    // Najdi posledni nenuly vzorek a max skok mezi sousednimi vzorky v zaveru.
    // Declick fade: sousedni vzorky se lisi o male kroky (5ms rampa pres 240
    // frames @48k → krok ~ 0.5*gain/240 ≈ 0.0015). DC step (hold→cut) by byl
    // jeden skok ~ 0.5*gain*pan ≈ 0.26 z posledniho vzorku rovnou na 0.
    size_t last_nz = 0;
    for (size_t i = 0; i < out.size(); ++i) if (std::fabs(out[i]) > 1e-4f) last_nz = i;
    REQUIRE(last_nz > 100);
    float max_tail_jump = 0.f;
    for (size_t i = last_nz > 300 ? last_nz - 300 : 1; i <= last_nz; ++i) {
        float d = std::fabs(out[i] - out[i-1]);
        if (d > max_tail_jump) max_tail_jump = d;
    }
    // Plynuly fade: zadny skok v zaveru vetsi nez ~0.05 (s velkou rezervou nad
    // ~0.0015/frame rampou; DC step by byl ~0.26 → cleanly nad prahem).
    CHECK(max_tail_jump < 0.05f);
}
```

- [ ] **Step 2: Run the new test — verify it FAILS (current hold-last-sample causes a DC step)**

Run: `cmake --build build --target test_streamed_interp -j && ctest --test-dir build -R test_streamed_interp --output-on-failure`
Expected: the new "EOF declick" case FAILS — `max_tail_jump` is ~0.26 (the held last sample cut straight to 0), well above 0.05. (The constant-amplitude voice holds ~0.5*vel_gain*pan until deactivation, then drops to 0 in one frame.)

- [ ] **Step 3: Implement — route EOF through the underrun fade**

In `engine/voice/voice.cpp`, replace the EOF case block shown above with:

```cpp
                } else if (ring_->eof_.load(std::memory_order_acquire)) {
                    // EOF: ring dotekl + worker oznacil konec. Misto
                    // hold-last-sample (DC step → lup) spustime rychly declick
                    // fade do nuly (stejny jako underrun) a hlas dohaje. hi=lo
                    // clamp → fade klesa z posledni realne hodnoty, ne ze skoku.
                    ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_;
                    if (!underrun_fading_) {
                        underrun_fading_ = true;
                        underrun_gain_   = 1.f;
                        log_end("ring_eof_fade");
                    }
                    break;
                } else {
```

(Note: `ring_lo_idx_` is NOT incremented and `active_` is NOT set here — the envelope block at lines ~300-309 multiplies the falling `underrun_gain_` and deactivates with `log_end("underrun_fade_zero")` once it hits zero. The output below computes `sL = ring_lo_l_*(1-frac)+ring_hi_l_*frac = ring_lo_l_` since hi=lo.)

- [ ] **Step 4: Run the new test — verify it PASSES**

Run: `cmake --build build --target test_streamed_interp -j && ctest --test-dir build -R test_streamed_interp --output-on-failure`
Expected: "EOF declick" case PASSES (max_tail_jump small). BUT the existing "Streamed EOF: hold last sample, cisty konec" case will now FAIL (its `last_nonzero > 0.5f` no longer holds — the voice fades to 0). That is expected and fixed in Task 2. (If you prefer, run only the declick case: `... -R "EOF declick"`.)

- [ ] **Step 5: Commit (implementation + new test)**

```bash
git add engine/voice/voice.cpp tests/test_streamed_interp.cpp
git commit -m "feat(voice): EOF declick fade instead of hold-last-sample"
```

---

## Task 2: Fix the existing EOF test (no longer holds last sample)

**Files:**
- Modify: `tests/test_streamed_interp.cpp` (the "Streamed EOF: hold last sample, cisty konec" case, lines ~129-146)

The current case (verbatim):
```cpp
TEST_CASE("Streamed EOF: hold last sample, cisty konec") {
    TempDir tmp{"eof"};
    constexpr int frames = 20000;
    writeRamp(tmp.path, frames, 48000, "48");

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> out = renderNote(eng, 1200);

    CHECK(eng.activeVoices() == 0);
    float last_nonzero = 0.f;
    for (float v : out) if (v > 0.001f) last_nonzero = v;
    CHECK(last_nonzero > 0.5f);
}
```

- [ ] **Step 1: Replace the case to reflect fade-to-zero behavior**

Replace that whole TEST_CASE with:

```cpp
TEST_CASE("Streamed EOF: dohraje cely ramp a cisto se deaktivuje") {
    // Ramp doznivajici k vrcholu; po EOF hlas declick-fadne k 0 a deaktivuje
    // se. Drive tento case testoval "hold last sample" (last_nonzero>0.5), ale
    // EOF nyni fadne do nuly — overujeme misto toho dosazeni vrcholu rampy
    // (pres cely beh) a cistou deaktivaci.
    TempDir tmp{"eof"};
    constexpr int frames = 20000;
    writeRamp(tmp.path, frames, 48000, "48");

    Engine eng;
    REQUIRE(eng.init(streamCfg(48000)));
    REQUIRE(eng.loadBank(tmp.path.string()));
    eng.noteOn(60, 127);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> out = renderNote(eng, 1200);

    CHECK(eng.activeVoices() == 0);                 // cista deaktivace po EOF fade
    float peak = 0.f;
    for (float v : out) peak = (std::max)(peak, v);
    CHECK(peak > 0.6f);                             // dosahli jsme vrcholu rampy pred fade
}
```

- [ ] **Step 2: Run the full streamed-interp test — all cases PASS**

Run: `cmake --build build --target test_streamed_interp -j && ctest --test-dir build -R test_streamed_interp --output-on-failure`
Expected: all cases PASS (smoothness, seam, the renamed EOF case, declick, 48k regression).

- [ ] **Step 3: Full suite**

Run: `ctest --test-dir build 2>&1 | tail -3`
Expected: `100% tests passed ... out of 25` (24 prior + the new declick case).

- [ ] **Step 4: Commit**

```bash
git add tests/test_streamed_interp.cpp
git commit -m "test(voice): update EOF test for fade-to-zero (was hold-last-sample)"
```

---

## Task 3: ResonanceVoice — EOF triggers declick fade

**Files:**
- Modify: `engine/voice/resonance_voice.cpp` (ring branch EOF case, lines ~216-222)

The current EOF case (verbatim) inside `while (ring_lo_idx_ < target)`:
```cpp
                    } else if (ring_->eof_.load(std::memory_order_acquire)) {
                        ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_;
                        ring_lo_idx_++;
                        if (ring_lo_idx_ >= (int64_t)total_frames - 1) {
                            active_ = false;
                        }
                        break;
                    } else {
```

ResonanceVoice already applies the underrun fade (resonance_voice.cpp ~295-304: `env *= underrun_gain_; underrun_gain_ += underrun_step_;` → `active_ = false` at zero) and `start()` sets `underrun_step_` (resonance_voice.cpp ~56). So the same routing works.

- [ ] **Step 1: Implement — route EOF through underrun fade**

Replace the EOF case block above with:

```cpp
                    } else if (ring_->eof_.load(std::memory_order_acquire)) {
                        // EOF: rychly declick fade do nuly (stejny jako
                        // underrun), deaktivace v envelope bloku. hi=lo clamp →
                        // fade z posledni hodnoty (zadny DC step).
                        ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_;
                        if (!underrun_fading_) {
                            underrun_fading_ = true;
                            underrun_gain_   = 1.f;
                            LOG_RT_WARN("resonance_voice", "eof-fade midi=%d", midi_);
                        }
                        break;
                    } else {
```

- [ ] **Step 2: Build + full suite (no regression)**

Run: `cmake --build build -j && ctest --test-dir build 2>&1 | tail -3`
Expected: `100% tests passed ... out of 25`. The existing `test_resonance_stream_sr` cases (96k duration discriminator, 44.1k smoothness) must still pass — they assert the resonance deactivates within budget and is interpolated; an EOF fade of 5 ms doesn't change the duration discriminator (~183 vs ~347 blocks) or the smoothness ratio meaningfully.

- [ ] **Step 3: Commit**

```bash
git add engine/voice/resonance_voice.cpp
git commit -m "feat(resonance): EOF declick fade instead of silent deactivate"
```

---

## Task 4: Final verification

**Files:** none (verification only)

- [ ] **Step 1: Clean rebuild + full suite**

Run: `cmake --build build -j 2>&1 | tail -4 && ctest --test-dir build 2>&1 | tail -3`
Expected: build OK (incl. ithaca-gui), `100% tests passed ... out of 25`.

- [ ] **Step 2: Confirm no "hold last sample" / "ring_eof_drained" leftovers**

Run: `grep -rn "hold last sample\|ring_eof_drained" engine/ tests/`
Expected: ZERO matches (the EOF comment and the old log tag are replaced by `ring_eof_fade` / the renamed test).

- [ ] **Step 3: Confirm both voices route EOF to the fade**

Run: `grep -rn "ring_eof_fade\|eof-fade" engine/voice/`
Expected: `voice.cpp` has `ring_eof_fade`, `resonance_voice.cpp` has `eof-fade` — one each.

---

## Notes for the implementer

- **Why reuse `underrun_fading_`:** it already does exactly "multiply a falling 5 ms gain over the output, then deactivate" (`kUnderrunFadeMs`, `underrun_step_` set in `start()`). EOF and underrun thus share one declick path; they differ only in the log line (EOF is a legitimate end, underrun is data-starvation). No new fields or constants.
- **Do NOT change the underrun (non-EOF) path** — it already fades correctly. Only the `eof_`-true branch changes.
- **hi=lo clamp matters:** it makes the interpolated output equal the last real sample during the fade, so the envelope ramps down from the true value (no pre-fade discontinuity).
- **The hard guard** (`position_ >= total_frames ...`) stays as a safety net; the fade normally deactivates first.
- **Test threshold rationale:** at 48k, vel127 → vel_gain=1.0, pan default cos(pi/4)≈0.707, so the constant-0.5 voice outputs ~0.354. A hold→cut DC step is ~0.354; the 5 ms fade (~240 frames @48k) steps by ~0.354/240 ≈ 0.0015/frame. Threshold 0.05 sits far above the fade step and far below the DC step — clean discrimination.
- **Do not commit** `imgui.ini` (gitignored) or any stray build dirs.
- Tests run a real `Engine` + stream worker; keep the per-block 2 ms sleep in `renderNote`. If flaky, raise the block budget — don't weaken the jump threshold.
