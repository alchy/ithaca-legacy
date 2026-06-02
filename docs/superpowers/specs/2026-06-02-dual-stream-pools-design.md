# Dual Stream Pools + Ring Diagnostics + Live Resonance Budget — Design

**Goal:** Isolate main-voice streaming from resonance streaming so sympathetic resonance can never starve the primary voices, make each pool's ring usage and underruns visible in the GUI (the count flashes red on underrun), and make the resonance voice budget live-adjustable so the user can tune it to their hardware.

**Why:** Diagnosed artifact — under sustained pedal-down play, up to ~32 resonance voices stream from disk concurrently and saturate the single shared ring pool / 4 workers, causing **resonance-voice underruns** (63 in a 20 s test; 0 main-voice underruns). Refill logic is correct; it is genuine streaming contention. Instead of a full architectural cure (RAM-only looped resonance — deferred), this feature gives **isolation + observability + a live tuning knob** so the user can dial resonance polyphony to what each machine sustains.

**Architecture:** Split the single `StreamEngine` into **two independent instances** — `stream_main_` (used by `VoicePool`) and `stream_resonance_` (used by `ResonanceEngine`) — each owning its own ring pool, worker threads, and request queue. Each instance records its last underrun timestamp; the engine exposes per-pool ring usage + a `…UnderrunRecent(ms)` query. The GUI shows MAIN and RESONANCE ring usage and flashes the number red for 4 s after an underrun. `ResonanceEngine`'s active-voice cap becomes atomic and is driven live by the MAX RESONANCE slider (persisted).

**Tech stack:** C++20 engine, Dear ImGui GUI, doctest. Reuses the existing `StreamEngine`, `ResonanceEngine`, and the `noteOnRecent`-style atomic-timestamp pattern.

---

## Current state (grounded)

- One `StreamEngine` (`engine.cpp:35`): `num_rings`=288 rings × 8192 frames (~170 ms), `stream_threads`=4, one `StreamRequestQueue`. Shared by `VoicePool` (main, ≤256) and `ResonanceEngine` (≤32) — both call `setStreamEngine(stream_.get())`.
- `num_rings` is sized `max_voices + max_resonance_voices` (`engine.cpp:27`).
- Underruns are logged (`LOG_RT_WARN "underrun"` in `resonance_voice.cpp`, `"UNDERRUN"` in `voice.cpp`) but not surfaced to the GUI.
- `ResonanceEngine::max_voices_` is the active-voice budget enforced in `enforceVoiceBudget` (`liveCount() > max_voices_`); set once at construction. The GUI MAX RESONANCE slider is currently **read-only / init-only**.
- GUI already shows `RESONANCE` and `RINGS` stat tiles via `ctx.engine.resonanceVoices()` / `numRingsUsed()`.

---

## Components

### 1. Two StreamEngine instances (`engine/engine.{h,cpp}`)

- Replace the single `std::unique_ptr<StreamEngine> stream_` with **two**: `stream_main_` and `stream_resonance_`.
- `EngineConfig` gains per-pool sizing (keep existing fields, add resonance ones):
  - main: `stream_threads` (rename intent: main workers), `num_rings` (main rings), `ring_capacity_frames` (shared capacity is fine).
  - resonance: `resonance_stream_threads` (default 4), `resonance_num_rings` (default = `max_resonance_voices`, e.g. 32).
- `init()`: construct both, `VoicePool::setStreamEngine(stream_main_.get())`, `ResonanceEngine::setStreamEngine(stream_resonance_.get())`. Each gets its own `recomputeRefillThreshold` (per its capacity/block).
- Main pool sized to cover main polyphony (e.g. 256 rings); resonance pool small (e.g. 32). Total RAM ≈ unchanged (~18 MB; 288 rings either way).
- **Worker isolation matters:** separate worker threads per instance is the point — a resonance refill burst occupies only resonance workers, so main refills are never delayed. (Shared workers would defeat the isolation.)

### 2. Per-pool underrun timestamp (`engine/stream/stream_engine.{h,cpp}` + voices)

- `StreamEngine` gains `std::atomic<uint64_t> last_underrun_us_{0}` and `void noteUnderrun() noexcept` (stamps steady-clock micros) + `bool underrunRecent(float ms) const`.
- On underrun, the consuming voice calls `stream_->noteUnderrun()` (both `Voice` and `ResonanceVoice`, at the existing underrun-detection sites). Each voice already holds its `StreamEngine*`.
- `Engine` exposes per-pool getters:
  - `int mainRingsUsed()` / `int mainRingsTotal()` ; `int resonanceRingsUsed()` / `int resonanceRingsTotal()`.
  - `bool mainStreamUnderrunRecent(float ms)` / `bool resonanceStreamUnderrunRecent(float ms)`.
- Pattern mirrors `noteOnRecent` (atomic timestamp + steady_clock); lock-free, GUI-pollable.

### 3. GUI ring diagnostics (`app/gui/` — indicator/config panel)

- A STREAM/RINGS readout showing two lines: `MAIN  <used>/<total>` and `RESONANCE <used>/<total>`.
- The used-count (or whole line) renders **red** when `…StreamUnderrunRecent(4000.f)` is true, else the base color. No separate timer — the 4 s window comes from the timestamp comparison (same mechanic as the note lamps).
- Placement: in the diagnostics/indicator strip or the CONFIG page; small, read-only.

### 4. Live MAX RESONANCE budget (`engine/resonance/resonance_engine.{h,cpp}` + GUI + persistence)

- `ResonanceEngine::max_voices_` → `std::atomic<int>`; add `void setMaxVoices(int)` / `int maxVoices() const`. `enforceVoiceBudget` reads it atomically. No array/ring resize needed — `voices_` is always 128 slots; the cap only limits how many sound at once.
- `Engine::setMaxResonanceVoices(int)` forwards to `resonance_->setMaxVoices()`.
- GUI: the existing MAX RESONANCE slider becomes **editable** (drop the read-only flag in `VoicePage`/`panel`), writing live via `setMaxResonanceVoices`; value persists in `state.json` (`max_resonance_voices` already exists).
- The resonance **ring pool** stays fixed-size; lowering the budget reduces concurrent streams (the actual contention lever).

---

## Data flow

```
play + pedal → resonance voices spawn (≤ maxVoices) → stream from stream_resonance_
                                                        (own rings + workers)
main notes → main voices → stream from stream_main_ (own rings + workers)
underrun in either → voice stamps that StreamEngine.last_underrun_us_
GUI (per frame) → reads ringsUsed/total + underrunRecent(4000) per pool
                → RESONANCE count flashes red 4 s on resonance underrun
user lowers MAX RESONANCE slider → resonance_->setMaxVoices() (atomic, live)
                → fewer concurrent resonance streams → underruns stop → flashing stops
```

---

## Error handling / edge cases

- `acquireRing` failure on the resonance pool (pool full) → voice plays head/preload only then fades (existing behavior), now isolated to the resonance pool.
- Lowering `maxVoices` below current live count: `enforceVoiceBudget` fades out the quietest excess (existing mechanism) — smooth, no click.
- Live `setMaxVoices` is a single atomic store from the GUI thread; `enforceVoiceBudget` (audio thread) reads atomically — no lock.
- Block-size change re-`prepare`s refill thresholds on both pools.

---

## Testing

- **Unit (doctest):** `StreamEngine::underrunRecent` timestamp window (stamp → recent true; after window → false). `ResonanceEngine::setMaxVoices/maxVoices` round-trip + that `enforceVoiceBudget` honors a lowered cap (drives active count down). Two-instance wiring: `Engine` exposes distinct main/resonance ring totals.
- **Manual / smoke:** reproduce the pedal-down underrun, watch the RESONANCE ring count flash red; lower MAX RESONANCE until it stops; confirm main ring count never flashes. (The 63-underrun log run is the baseline.)
- GUI not unit-tested (per convention).

---

## File structure

**Modify:**
- `engine/engine.h` / `engine.cpp` — two StreamEngine members + config + per-pool getters + `setMaxResonanceVoices`.
- `engine/stream/stream_engine.h` / `.cpp` — `last_underrun_us_`, `noteUnderrun()`, `underrunRecent()`.
- `engine/voice/voice.cpp` / `resonance_voice.cpp` — call `stream_->noteUnderrun()` at underrun sites.
- `engine/resonance/resonance_engine.h` / `.cpp` — atomic `max_voices_` + setter/getter.
- `app/gui/` — ring-diagnostics readout (new lines + red-flash); make MAX RESONANCE slider live.
- `app/gui/persistence.*` — `max_resonance_voices` already persisted; ensure live value is saved.
- `CMakeLists` / `tests/` — new test cases.

**New:** possibly `tests/test_stream_engine.cpp` (underrun timestamp) if not folding into an existing test.

---

## Out of scope / future

- **RAM-only looped resonance** (the full underrun cure) — deferred; this feature instead makes underruns visible + tunable.
- Auto hardware detection of resonance budget — for now the user sets it via the GUI slider.
- Per-pool worker/ring counts exposed in the GUI — config-level only for v1 (defaults: main 4 workers / 256 rings, resonance 4 workers / 32 rings).
- Separate request-queue priority schemes — not needed once pools+workers are split.
