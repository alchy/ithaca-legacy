# Modular DSP Chain + CONFIG Switcher — Design

**Goal:** Replace the GUI's placeholder DSP RACK with a real, modular post-mix DSP chain (AGC → BBE → Limiter) and a CONFIG selector that swaps the middle parameter panel to edit the selected stage (VOICE included).

**Architecture:** A generic parameter-descriptor model. Each DSP stage and the VOICE page implement a common `IParamPage` interface (list of `Param` descriptors + get/set/enable/meter). The engine owns a fixed-order `DspChain`; the GUI renders the selected page through one generic loop. Adding a stage requires zero GUI code.

**Tech stack:** C++17, Dear ImGui 1.91 (existing GUI), doctest (existing tests). DSP modules ported from the sibling repo `/Users/j/Projects/icr` (`engine/dsp/`).

**Reference:** icr `engine/dsp/` (modular chain, `dsp_math.h`, `agc.h`, `bbe`, `limiter`); icr2 `player/gui/sections/dsp_rack.cpp` + `widgets.h` (card/slider widget patterns). icr2 shows all stages at once via a card registry; this design instead uses a list→detail switcher (new, cleaner for our 3-column layout).

---

## Scope

**In scope (this round):** modular chain framework, three ported stages **AGC, BBE, Limiter**, engine wiring, persistence, the GUI CONFIG selector + generic switchable parameter panel, unit tests for the engine.

**Out of scope (later follow-up):** **Convolver** (body impulse response). It needs an IR asset and is CPU-heavy (icr's is naive O(N)/sample time-domain). The `DspStage` interface and `DspChain` are designed so the Convolver drops in later as another stage with no GUI changes. The CONFIG list has four items this round: VOICE, AGC, BBE, LIMITER.

**No unrelated refactoring.** The existing master-gain, peak-meter, voice, and resonance code is untouched except for the single chain-insertion point in `processBlock`.

---

## Signal flow

The chain runs **after master gain, before the peak meter** in `Engine::processBlock` (`engine/engine.cpp`, currently lines 209–231):

```
voices (voice_pool) + resonance  →  × master_gain  →  AGC → BBE → Limiter  →  peak meter  →  interleave → out
```

Stage order is fixed: **AGC → BBE → Limiter** (Limiter last as the output safety net). Only `enabled()` stages process; a disabled stage is a no-op (input passes through unchanged). Processing is in-place, non-interleaved stereo (`float* L`, `float* R`, `int n`), matching the existing buffers.

---

## Component 1 — `engine/dsp/dsp_stage.h` (interfaces)

Defines the descriptor and the two interfaces shared by engine and GUI.

```cpp
#pragma once
#include <cstdint>

namespace ithaca::dsp {

struct Param {
    const char* id;             // stable persistence key, e.g. "threshold_db"
    const char* label;          // UI eyebrow, e.g. "THRESHOLD"
    float       min, max, def;
    const char* fmt;            // DecoSlider format, e.g. "%.1f dB"
    bool        readonly = false;
};

// GUI-facing interface. Implemented by every DspStage AND by the GUI's VoicePage.
struct IParamPage {
    virtual ~IParamPage() = default;
    virtual const char* name() const = 0;            // "AGC", "VOICE"
    virtual int         paramCount() const = 0;
    virtual const Param& param(int i) const = 0;
    virtual float       get(int i) const = 0;        // current value
    virtual void        set(int i, float v) = 0;     // clamps to [min,max]
    virtual bool        hasEnable() const = 0;       // VOICE returns false
    virtual bool        enabled() const = 0;
    virtual void        setEnabled(bool on) = 0;
    // Optional read-only meter (limiter GR, AGC current gain).
    // Returns false if the page has no meter.
    virtual bool        meter(float& value, const char*& label) const = 0;
};

// Audio-thread processing stage = a param page plus DSP.
struct DspStage : IParamPage {
    virtual void prepare(float sample_rate, int max_block) = 0;
    virtual void reset() = 0;
    virtual void process(float* L, float* R, int n) = 0;
};

} // namespace ithaca::dsp
```

**RT-safety contract:** `set()`/`setEnabled()` are called from the GUI thread; `process()` from the audio thread. Each stage stores parameter values as `std::atomic<float>` and `enabled` as `std::atomic<bool>`. `process()` reads them once at block start and recomputes derived coefficients (biquad/limiter envelope coeffs) only when a value changed (epsilon compare against a cached last-applied value). No coefficient writes happen on the GUI thread, so there is no torn read or race. `set()` clamps to `[min,max]`.

---

## Component 2 — `engine/dsp/dsp_math.h`

Ported verbatim from icr `engine/dsp/dsp_math.h` (header-only, stateless, RT-safe): `db_to_lin`/`lin_to_db`, `decay_coeff`, `onepole_alpha`, `BiquadCoeffs` + `biquad_tick`, `rbj_high_shelf`/`rbj_low_shelf`, `gain_envelope_smooth`, constants. Namespace adjusted to `ithaca::dsp`. Only the functions used by AGC/BBE/Limiter are required; unused helpers may be dropped.

---

## Component 3 — Stages (ported from icr, wrapped as `DspStage`)

Each stage owns its `std::atomic` parameters, the static `Param[]` descriptor table, and the DSP state. `param/get/set/enabled/meter` are mechanical. `process()` applies the icr algorithm.

### AGC — `engine/dsp/agc.{h,cpp}`
Wraps icr's `AgcState` (RMS-following downward gain, fast attack / slow release, never amplifies, floored). Enable toggle.

| id | label | min | max | default | fmt |
|---|---|---|---|---|---|
| `target_rms` | TARGET | 0.01 | 0.5 | 0.15 | `%.3f` |
| `release_ms` | RELEASE | 10 | 2000 | 200 | `%.0f ms` |
| `gain_floor` | GAIN FLOOR | 0.0 | 1.0 | 0.05 | `%.2f` |

Meter: `CURRENT GAIN` = current smoothed gain (×), `fmt %.3f`. Attack fixed at 5 ms (icr default).

### BBE — `engine/dsp/bbe.{h,cpp}`
Two RBJ shelving biquads per channel (high shelf @ 5 kHz, low shelf @ 180 Hz), ported from icr `bbe`. Enable toggle. When both params are 0 and enabled, output is effectively unity (flat shelves); when disabled it is bypassed entirely.

| id | label | min | max | default | fmt |
|---|---|---|---|---|---|
| `definition` | DEFINITION | 0 | 12 | 0 | `%.1f dB` |
| `bass` | BASS | 0 | 10 | 0 | `%.1f dB` |

Meter: none.

### Limiter — `engine/dsp/limiter.{h,cpp}`
Peak limiter ported from icr `limiter` (per-sample peak detect, smoothed envelope, gain ≤ 1, fixed 1 ms attack). Enable toggle.

| id | label | min | max | default | fmt |
|---|---|---|---|---|---|
| `threshold_db` | THRESHOLD | -40 | 0 | 0 | `%.1f dB` |
| `release_ms` | RELEASE | 10 | 2000 | 200 | `%.0f ms` |

Meter: `GAIN REDUCTION` (dB, 0 = none, negative = limiting), `fmt %.1f dB`.

---

## Component 4 — `engine/dsp/dsp_chain.{h,cpp}`

```cpp
class DspChain {
public:
    void prepare(float sample_rate, int max_block);   // prepares all stages
    void reset();                                       // resets all stages
    void process(float* L, float* R, int n);           // runs enabled stages in order
    int  stageCount() const { return 3; }
    DspStage& stage(int i);                             // 0=AGC,1=BBE,2=Limiter (GUI access)
private:
    Agc     agc_;
    Bbe     bbe_;
    Limiter lim_;
    DspStage* stages_[3] = { &agc_, &bbe_, &lim_ };
};
```

`process()`:
```cpp
for (auto* s : stages_) if (s->enabled()) s->process(L, R, n);
```

---

## Component 5 — Engine wiring (`engine/engine.{h,cpp}`)

- Add member `ithaca::dsp::DspChain dsp_;`
- In the engine init path (where sample rate / block size are known, alongside the existing setup), call `dsp_.prepare(sample_rate, block_size)`. On `setBlockSize`, re-`prepare`.
- In `processBlock`, insert after the master-gain loop (line ~212) and before the peak-meter loop (line ~214):
  ```cpp
  dsp_.process(out_l, out_r, n_samples);
  ```
- Expose `ithaca::dsp::DspChain& dspChain() { return dsp_; }` for the GUI.

No change to voice/resonance/master-gain/meter logic.

---

## Component 6 — GUI VOICE page (`app/gui/voice_page.h`)

`VoicePage : ithaca::dsp::IParamPage` lives in the GUI layer and bridges existing engine setters + `GuiState`. Constructed with references to `AppContext` (engine + state). Not part of the audio chain; `hasEnable()` returns false (always on).

Params (each `get` reads `GuiState`/engine, each `set` writes `GuiState` and calls the existing engine setter):

| id | label | min | max | default | fmt | engine setter |
|---|---|---|---|---|---|---|
| `master_db` | MASTER | -60 | 6 | 0 | `%.1f dB` | `setMasterGain(pow(10, db/20))` |
| `resonance` | RESONANCE | 0 | 1 | 0.5 | `%.2f` | `setResonanceStrength` |
| `release_ms` | RELEASE | 50 | 2000 | 200 | `%.0f ms` | `setReleaseMs` |
| `excite_ms` | EXCITE DECAY | 500 | 30000 | 5000 | `%.0f ms` | `setExciteDecayMs` |
| `max_res` | MAX RESONANCE | 1 | 64 | 32 | `%.0f` | — (readonly = true) |

`meter()` returns false. This preserves exactly the current VOICE controls; they just render through the generic loop now.

---

## Component 7 — GUI generic parameter panel (middle column)

The existing `panel_params.cpp` becomes a generic renderer `renderParamPage(AppContext&, IParamPage&)`:

1. `Eyebrow(page.name(), silver2)` as the header.
2. If `page.hasEnable()`: an ON/OFF toggle (gold when on) wired to `enabled()/setEnabled()`.
3. For each `i` in `paramCount()`: a `DecoSlider(p.label, &tmp, p.min, p.max, p.fmt, gold, /*enabled=*/!p.readonly)` where `tmp = page.get(i)`; on change call `page.set(i, tmp)`. (DecoSlider already supports the read-only/disabled mode added earlier.) MASTER/first param uses the gold accent like today.
4. If `page.meter(v, label)`: render `Eyebrow(label)` + value via the existing value styling.

MASTER stays the visually primary (gold) slider on the VOICE page, matching the current look.

---

## Component 8 — GUI CONFIG panel (right column, replaces DSP RACK)

`app/gui/panel_config.{h,cpp}` — `renderConfigPanel(AppContext&, IParamPage** pages, int n, int& selected)`:

- `Eyebrow("CONFIG", silver2)` header.
- For each page: a row with an LED dot (`enabled()` → gold, else muted; VOICE always gold) + the page name; the whole row is an `ImGui::Selectable`/invisible-button. Click sets `selected = i`. The selected row is marked (gold name / left marker).
- VOICE has no toggle in the list (always on); its LED is always lit.

`main.cpp` owns the page list and selection:
```cpp
VoicePage voice(ctx);
IParamPage* pages[] = { &voice,
                        &ctx.engine.dspChain().stage(0),   // AGC
                        &ctx.engine.dspChain().stage(1),   // BBE
                        &ctx.engine.dspChain().stage(2) };  // LIMITER
int selected = ctx.state.config_page;   // persisted
```
Middle column: `renderParamPage(ctx, *pages[selected])`. Right column: `renderConfigPanel(ctx, pages, 4, selected)`; write `selected` back to `ctx.state.config_page`. `panel_dsp.{h,cpp}` is deleted.

Layout (unchanged 3-column main row, BANK | params | CONFIG):
```
┌─BANK───┐ ┌─ AGC ────────┐ ┌─CONFIG──┐
│ select │ │ [ON]          │ │ ● VOICE │
│ LEGACY │ │ TARGET ───●─  │ │ ● AGC ◀ │
│ facts  │ │ RELEASE ─●──  │ │ ● BBE   │
│ RELOAD │ │ GAIN FLOOR ●  │ │ ○ LIMIT │
└────────┘ │ CURRENT: 0.98 │ └─────────┘
           └───────────────┘
```

---

## Persistence (`app/gui/persistence.{h,cpp}`)

`GuiState` schema bumps to **version 4**. New explicit fields (matching the existing flat style — no generic map):

```
// DSP — defaults below = no behavior change vs today (all stages off)
bool  agc_enabled = false;       float agc_target = 0.15f;  float agc_release_ms = 200.f;  float agc_floor = 0.05f;
bool  bbe_enabled = false;       float bbe_definition = 0.f; float bbe_bass = 0.f;
bool  limiter_enabled = false;   float limiter_threshold_db = 0.f; float limiter_release_ms = 200.f;
int   config_page = 0;           // 0 = VOICE
```

**Migration (must not discard v3 state).** Today `loadState` does `if (s.schema_version != 3) return std::nullopt;`, which would throw away an existing v3 file (bank, window, MIDI) the moment we bump the version. Also, the existing code reads numeric fields with `std::stof(findValue(...))`, which throws when a key is missing. So the migration rule is concrete:

- Accept **both** v3 and v4: `if (sv != 3 && sv != 4) return std::nullopt;`
- Read every new DSP key **defensively** (missing → default), the same pattern `log_level`/`midi_channel` already use:
  `{ std::string v = findValue(json,"agc_target"); s.agc_target = v.empty()? 0.15f : std::stof(v); }` (and bool keys parse `"true"`/`1`).
- `saveState` always writes **v4** with all DSP fields.

Result: a v3 file loads cleanly with DSP defaults (all stages off) and is rewritten as v4 on the next save. The unconditional `saveState` on exit (already in `main.cpp`) serializes everything. `AppContext::initFromState` applies persisted DSP values to the chain via `stage(i).set(...)` / `setEnabled(...)` at startup, the same way `log_level` is applied.

**Default behavior:** all three DSP stages **disabled** by default → the signal path is identical to today's. Limiter default threshold 0 dB / release 200 ms means enabling it is transparent until 0 dBFS.

---

## Testing

`tests/test_dsp.cpp` (doctest, registered in `tests/CMakeLists.txt` like the others):

- **Param round-trip:** `set(i, v)` then `get(i) == v`; out-of-range `set` clamps to `[min,max]`; `setEnabled`/`enabled` toggle.
- **Bypass:** every stage with `enabled=false` leaves the buffer bit-identical (no-op). `DspChain` with all stages disabled = identity.
- **Limiter:** a block whose peak exceeds the threshold comes out with peak ≤ threshold (within envelope tolerance); a block below threshold is unchanged; `GAIN REDUCTION` meter is negative when limiting, ~0 otherwise; never amplifies.
- **AGC:** loud input (RMS ≫ target) is attenuated toward target RMS; quiet input keeps gain ≈ 1; gain never exceeds 1 and never drops below `gain_floor`.
- **BBE:** both params 0 → output ≈ input (flat); definition/bass > 0 → output differs from input (shelf applied); disabled → bit-identical bypass.
- **Chain order:** stage indices 0/1/2 are AGC/BBE/Limiter; `prepare`/`reset` propagate.

GUI is validated by a smoke run (build + launch + load bank, consistent with the current no-GUI-unit-test convention). No GUI unit tests added.

---

## File structure

**New (engine):** `engine/dsp/dsp_math.h`, `engine/dsp/dsp_stage.h`, `engine/dsp/agc.{h,cpp}`, `engine/dsp/bbe.{h,cpp}`, `engine/dsp/limiter.{h,cpp}`, `engine/dsp/dsp_chain.{h,cpp}`.
**New (GUI):** `app/gui/voice_page.h`, `app/gui/panel_config.{h,cpp}`.
**Modify:** `engine/engine.{h,cpp}` (chain member + wiring + getter), `app/gui/main.cpp` (page list, selection, generic middle renderer, CONFIG right column), `app/gui/panel_params.cpp` (→ generic `renderParamPage`), `app/gui/persistence.{h,cpp}` (schema v4), `app/gui/app_context.cpp` (apply DSP params at init), `CMakeLists.txt` + `tests/CMakeLists.txt` (new sources + `test_dsp`).
**Delete:** `app/gui/panel_dsp.{h,cpp}` (placeholder).

---

## Open follow-ups (not this spec)

- **Convolver** stage (body IR) — new `DspStage`, needs an IR asset and a performance decision (partitioned/FFT vs time-domain). Drops into `DspChain` + CONFIG list with no other changes.
- Optional: per-stage bypass metering in the CONFIG list (e.g., small GR readout next to LIMITER).
