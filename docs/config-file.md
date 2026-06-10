# ithaca-gui Configuration File (`state.json`)

## 1. Overview

`state.json` is the persisted configuration for **ithaca-gui** (the F8 GUI front-end).
It stores window geometry, the selected bank and MIDI port, voice/resonance
parameters, the full DSP-chain settings (Convolver, AGC, Enhancer, Limiter), the
active CONFIG page, the audio device settings, and the log level.

The file is **auto-managed** by ithaca-gui — you normally never edit it by hand:

- It is created on first clean exit if it does not exist (the GUI starts from
  built-in defaults when no file is present).
- During a session it is written with a **debounced save**: the file is written
  **~1 second after the first tracked change** (not after 1 s of idle — the
  timer starts at the first change and is not reset by further changes). While
  a slider is being dragged continuously, the file is therefore written about
  once per second instead of on every frame.
- It is also written **unconditionally on clean exit** (when the window is closed),
  capturing the final window geometry.

Writes are atomic: the GUI writes to `state.json.tmp`, flushes and checks the
stream state, and only then renames it over `state.json`. On an I/O error (e.g.
a full disk) the truncated `.tmp` is deleted and the existing good config is
left untouched (see `saveState` in `app/gui/persistence.cpp`).

The format is a flat JSON object (no nested objects or arrays). It is parsed by a
minimal hand-written parser, not a full JSON library, so keep it flat if you do
edit it manually. Key order does not matter to the parser (it looks up keys by
name); the order below matches what `saveState` emits.

## 2. Location

The path is computed by `defaultStatePath()` → `platformConfigDir() / "ithaca-legacy" / "state.json"`
in `app/gui/persistence.cpp`. The per-OS base directory:

| OS      | Base directory (`platformConfigDir()`)                     | Full path                                                        |
|---------|------------------------------------------------------------|-----------------------------------------------------------------|
| macOS   | `$HOME/Library/Application Support`                        | `$HOME/Library/Application Support/ithaca-legacy/state.json`     |
| Linux   | `$XDG_CONFIG_HOME`, else `$HOME/.config`                   | `$XDG_CONFIG_HOME/ithaca-legacy/state.json` (or `~/.config/...`) |
| Windows | `%APPDATA%`                                                | `%APPDATA%\ithaca-legacy\state.json`                            |

If the relevant environment variable is unset, `platformConfigDir()` falls back
to the **current working directory**, so the file would land at
`./ithaca-legacy/state.json`. The parent directories are created automatically on
save.

## 3. Schema Versioning & Migration

- The current schema is **`schema_version` 4**.
- `loadState()` accepts files with `schema_version` **3 or 4**; any other value
  (or a missing `schema_version`) causes the load to fail and the GUI starts from
  defaults. After a successful load, `schema_version` is unconditionally set to
  `4` in memory, so the **next save rewrites the file as v4**.
- **All keys are read defensively** — a missing **or corrupted** (non-numeric)
  key falls back to the `GuiState` struct default *for that field only*; the rest
  of the file still loads. (`loadState` reads every numeric/bool field through
  `readF`/`readB`/`readI` helpers with a per-field `try/catch`.) This is what
  makes old files forward-compatible: the schema has grown well beyond the
  original v4 (Convolver, Enhancer, the resonance gain/layer split,
  `resonance_window_ms`, the audio device fields), yet a file that predates
  those keys still loads, with each absent key taking its default. A single bad
  key no longer discards the whole state — only a missing/foreign
  `schema_version` makes `loadState` return `nullopt` (full fallback to
  defaults).
- **Value sanitization on load:** `midi_channel` is clamped to `[-1, 15]`
  (out-of-range → `-1` = OMNI); window geometry is sanitized — `window_w < 320`
  falls back to `1280`, `window_h < 240` falls back to `720` (a minimized window
  on Windows persists 0×0, and `glfwCreateWindow(0,0)` would fail on the next
  start).
- **BBE → Enhancer migration.** The old "BBE" stage was renamed to "Enhancer".
  The `enhancer_*` keys fall back to the legacy `bbe_*` keys when absent:
  `enhancer_process` ← `bbe_definition`, `enhancer_contour` ← `bbe_bass`,
  `enhancer_enabled` ← `bbe_enabled`. So a file written before the rename migrates
  its old BBE values into the Enhancer on first load, then is rewritten with the
  new keys.
- `log_level` defaults to `"info"` if empty/absent; `midi_channel` defaults to
  `-1` if empty/absent.

## 4. Field Reference

Every field of the `GuiState` struct (`app/gui/persistence.h`) is listed below.
Ranges/units for the voice/resonance and DSP fields come from the `Param` tables
in `app/gui/master_page.h`, `resonance_page.h` and `engine/dsp/{convolver,agc,
enhancer,limiter}.cpp`; the GUI clamps values to these ranges when set via a
control, and the DSP stages additionally clamp on `set()`.

### Meta

| JSON key         | Type | Default | Allowed values | Meaning | Set by |
|------------------|------|---------|----------------|---------|--------|
| `schema_version` | int  | `4`     | accepts 3 or 4 on load; always written as 4 | File format version | init/migration only |

### Window geometry

| JSON key   | Type | Default | Range | Meaning | Set by |
|------------|------|---------|-------|---------|--------|
| `window_x` | int  | `100`   | any (off-screen clamped) | Window X position (screen px) | GUI (tracked every frame) |
| `window_y` | int  | `100`   | any (off-screen clamped) | Window Y position (screen px) | GUI (tracked every frame) |
| `window_w` | int  | `1280`  | ≥ 320 (smaller → falls back to 1280 on load) | Window width (px) | GUI (tracked every frame) |
| `window_h` | int  | `720`   | ≥ 240 (smaller → falls back to 720 on load) | Window height (px). Default targets a 1280×720 HW display. | GUI (tracked every frame) |

Window geometry is updated into the in-memory state every frame and is persisted
on exit (and via the debounce if other fields change). See the off-screen clamp
note in section 7.

### Bank

| JSON key          | Type   | Default | Meaning | Set by |
|-------------------|--------|---------|---------|--------|
| `bank_search_dir` | string | `""`    | Directory scanned by the bank dropdown for candidate banks. When `bank_path` is empty this is the only source of bank candidates. | `--bank-dir` CLI flag (overrides on startup) |
| `bank_path`       | string | `""`    | Path to the bank to load at startup. Loaded best-effort: failure logs a warning and the engine runs empty. | GUI (bank panel) |

### MIDI

| JSON key         | Type   | Default | Range/values | Meaning | Set by |
|------------------|--------|---------|--------------|---------|--------|
| `midi_port_name` | string | `""`    | substring of a real port name | MIDI input port. Matched as a substring against the live port list at startup; on a match the exact port label is written back. No match → warning, no port opened. | GUI / init |
| `midi_channel`   | int    | `-1`    | `-1` = OMNI, `0`–`15` = channel (0-based) | MIDI receive channel. OMNI accepts all channels — required for multi-channel material (e.g. Synthesia left/right hand on ch0/ch1). | GUI / init |

### Voice & resonance parameters

Applied to the engine at init via `EngineConfig`; the live-adjustable ones are on
the **MASTER** and **RESONANCE** CONFIG pages.

| JSON key               | Type  | Default  | Range          | Unit | Meaning | Set by |
|------------------------|-------|----------|----------------|------|---------|--------|
| `master_gain_db`       | float | `0.0`    | `-60` … `6`    | dB   | Master output gain (converted to linear `10^(dB/20)`) | GUI (MASTER) |
| `release_ms`           | float | `200.0`  | `50` … `2000`  | ms   | Voice release time | GUI (MASTER) |
| `resonance_enabled`    | bool  | `true`   | `true`/`false` | —    | Enable sympathetic resonance engine | GUI (RESONANCE / topbar) |
| `resonance_gain_db`    | float | `-12.0`  | `-60` … `0`    | dB   | Resonance output gain | GUI (RESONANCE) |
| `resonance_layer_db`   | float | `-30.0`  | `-60` … `0`    | dB   | Target velocity-layer (peak RMS) the resonance picks per note (`nearestSlotByRms`) | GUI (RESONANCE) |
| `excite_decay_ms`      | float | `5000.0` | `500` … `30000`| ms   | Excitation decay time | GUI (RESONANCE) |
| `max_resonance_voices` | int   | `32`     | `1` … `64`     | —    | Max resonance voice count. **Live** — the RESONANCE "MAX RESONANCE" slider (param 3) calls `engine.setMaxResonanceVoices`; also applied at engine init and tracked by the debounced save. | GUI (RESONANCE) |
| `resonance_window_ms`  | int   | `12000`  | ≥ 0 (ms)       | ms   | RAM-cache window of the resonance target layer per note. **JSON-only — there is intentionally no GUI control**; edit by hand. Larger = longer resonance tails held in RAM (more memory). | JSON only |
| `preload_ms`           | int   | `150`    | ≥ 0 (ms)       | ms   | Per-sample preload head length kept in RAM (rest streams from disk). **JSON-only.** Larger = more RAM resident, less disk streaming (fewer underruns) — useful on embedded with fast RAM / slow storage; load whole short samples by raising this. | JSON only |
| `cache_budget_mb`      | int   | `0`      | `0`=auto, else MB | MB | RAM budget for bank load. `0` = **auto** (~60 % of physical RAM, via `sysinfo`). `>0` = hard cap. On exceed, loading is **aborted** (incomplete bank + error log) instead of crashing on `bad_alloc`. **JSON-only.** Protects embedded (RPi5/4 GB) from OOM. | JSON only |

### DSP chain

The DSP chain processes the post-mix stereo buffer in this fixed audio order:
**Convolver → AGC → Enhancer → Limiter**. Each stage has an `enabled` flag
(all default **off**, so a fresh config is audibly transparent) plus its own
parameters. (Note: the JSON *key* order differs from the chain order because the
Convolver keys were added later — see the example. The parser is order-independent.)

#### Convolver — cabinet/body simulation (chain stage 0)

Adds the instrument body to close-miked samples via a short FIR convolution.
Ranges from `Convolver::kParams` (`engine/dsp/convolver.cpp`).

| JSON key            | Type  | Default | Range          | Meaning | Set by |
|---------------------|-------|---------|----------------|---------|--------|
| `convolver_enabled` | bool  | `false` | `true`/`false` | Enable Convolver stage | GUI (CONVOLVER page) |
| `convolver_mix`     | float | `0.15`  | `0` … `1`      | Wet/dry mix (low = subtle body) | GUI (CONVOLVER) |
| `convolver_choice`  | int   | `0`     | `0` or `1`     | IR selection: `0` = "Body soft (modal)", `1` = "Body bright (modal)". Both are synthesized procedurally (`ir_modal.cpp`), not files. | GUI (CONVOLVER) |
| `convolver_decay`   | float | `0.5`   | `0` … `1`      | IR decay shaping (shorter ↔ longer body) | GUI (CONVOLVER) |
| `convolver_tone`    | float | `0.6`   | `0` … `1`      | IR low-pass tone | GUI (CONVOLVER) |
| `convolver_size`    | float | `0.5`   | `0` … `1`      | Body size (modal frequency shift: smaller ↔ bigger) | GUI (CONVOLVER) |

#### AGC (chain stage 1)

Ranges from `AGC::kParams` (`engine/dsp/agc.cpp`).

| JSON key          | Type  | Default | Range          | Unit | Meaning | Set by |
|-------------------|-------|---------|----------------|------|---------|--------|
| `agc_enabled`     | bool  | `false` | `true`/`false` | —    | Enable AGC stage | GUI (AGC page) |
| `agc_target`      | float | `0.15`  | `0.01` … `0.5` | RMS (linear) | Target RMS the AGC follows down to | GUI (AGC page) |
| `agc_release_ms`  | float | `200.0` | `10` … `2000`  | ms   | AGC release time (attack is fixed at 5 ms) | GUI (AGC page) |
| `agc_floor`       | float | `0.05`  | `0` … `1`      | gain (linear) | Minimum gain floor (AGC never attenuates below this) | GUI (AGC page) |

#### Enhancer (chain stage 2) — was "BBE"

An original piano enhancer (parallel-boost 3-band + dynamic HF + harmonic exciter
+ all-pass phase). Ranges from `Enhancer::kParams` (`engine/dsp/enhancer.cpp`).

| JSON key            | Type  | Default | Range       | Unit | Meaning | Set by |
|---------------------|-------|---------|-------------|------|---------|--------|
| `enhancer_enabled`  | bool  | `false` | `true`/`false` | — | Enable Enhancer stage | GUI (ENHANCER page) |
| `enhancer_process`  | float | `0.0`   | `0` … `12`  | dB   | PROCESS — dynamic high-band boost + exciter (boost-when-loud) | GUI (ENHANCER) |
| `enhancer_contour`  | float | `0.0`   | `0` … `12`  | dB   | CONTOUR — low-band boost | GUI (ENHANCER) |
| `enhancer_mid`      | float | `0.0`   | `-6` … `6`  | dB   | MID — presence bell (~2.7 kHz) | GUI (ENHANCER) |

#### Limiter (chain stage 3)

Ranges from `Limiter::kParams` (`engine/dsp/limiter.cpp`).

| JSON key               | Type  | Default | Range          | Unit | Meaning | Set by |
|------------------------|-------|---------|----------------|------|---------|--------|
| `limiter_enabled`      | bool  | `false` | `true`/`false` | —    | Enable limiter stage | GUI (LIMITER page) |
| `limiter_threshold_db` | float | `0.0`   | `-40` … `0`    | dB   | Peak threshold (stereo-linked) | GUI (LIMITER page) |
| `limiter_release_ms`   | float | `200.0` | `10` … `2000`  | ms   | Limiter release time (attack is fixed at 1 ms) | GUI (LIMITER page) |

### CONFIG page selector

| JSON key      | Type | Default | Allowed values | Meaning | Set by |
|---------------|------|---------|----------------|---------|--------|
| `config_page` | int  | `0`     | `0`–`5`        | Selected CONFIG page: `0`=MASTER, `1`=RESONANCE, `2`=CONVOLVER, `3`=AGC, `4`=ENHANCER, `5`=LIMITER. Out-of-range values are clamped to `0` at startup. | GUI (CONFIG switch) |

### Audio device

| JSON key            | Type | Default | Meaning | Set by |
|---------------------|------|---------|---------|--------|
| `audio_block_size`  | int  | `256`   | Audio callback block size (latency). Runtime-changeable from the GUI BUFFER combo; clamped to `[32, 8192]`. | GUI (BUFFER combo) |
| `audio_sample_rate` | int  | `48000` | Engine sample rate. **JSON-only / read-only in GUI** (the GUI displays it but does not change it). A non-positive value falls back to 48000. | JSON only |

### Log

| JSON key    | Type   | Default  | Allowed values | Meaning | Set by |
|-------------|--------|----------|----------------|---------|--------|
| `log_level` | string | `"info"` | `debug` \| `info` \| `warn` \| `error` \| `fatal` (also `off`/`warning` accepted by the parser) | Minimum log severity, applied via `setMinSeverity`. Unparseable values fall back to `info`. | `--log-level` CLI flag (overrides on startup) / GUI (live) |

The severity strings are parsed by `severity_from_string` (`engine/util/log.h`):
`debug`, `info`, `warn`/`warning`, `error`, `fatal` (case-insensitive). `Off`
exists as an internal severity that suppresses all output.

## 5. CLI Overrides

`ithaca-gui` accepts two flags (`app/gui/main.cpp`) that override the persisted
values **at startup**, before the state is applied to the engine:

| Flag                 | Overrides         | Behavior |
|----------------------|-------------------|----------|
| `--bank-dir <path>`  | `bank_search_dir` | Sets the bank search directory. Applied only if non-empty. |
| `--log-level <lvl>`  | `log_level`       | `debug` \| `info` \| `warn` \| `error` \| `fatal`. Applied only if non-empty. |

`--help` / `-h` prints usage and exits.

Important: an override is **persisted** like any other value. Once you pass
`--bank-dir` or `--log-level` once, the new value is written back into
`state.json` (on the next debounced save or on exit), so you typically only need
to pass each flag once.

## 6. Annotated Example `state.json`

The following matches the exact key order and format emitted by `saveState`,
with the built-in default values. (JSON does not allow comments; the trailing
`//` notes are for documentation only — remove them if you paste this into a real
file.)

```json
{
  "schema_version": 4,                 // always 4
  "bank_search_dir": "/Users/me/banks",// dir scanned by bank dropdown
  "bank_path": "",                     // bank loaded at startup ("" = none)
  "midi_port_name": "",                // substring-matched MIDI port
  "log_level": "info",                 // debug|info|warn|error|fatal
  "midi_channel": -1,                  // -1 = OMNI, 0..15 = channel
  "master_gain_db": 0,                 // -60 .. 6 dB
  "resonance_enabled": true,           // sympathetic resonance on
  "resonance_gain_db": -12,            // -60 .. 0 dB
  "resonance_layer_db": -30,           // -60 .. 0 dB (target layer)
  "release_ms": 200,                   // 50 .. 2000 ms
  "excite_decay_ms": 5000,             // 500 .. 30000 ms
  "max_resonance_voices": 32,          // 1 .. 64 (live GUI slider)
  "resonance_window_ms": 12000,        // RAM cache window (JSON-only, no GUI)
  "preload_ms": 150,                   // preload head per sample (JSON-only)
  "cache_budget_mb": 0,                // 0=auto (~60% RAM); RAM budget (JSON-only)
  "window_x": 100,                     // px (off-screen clamped)
  "window_y": 100,                     // px (off-screen clamped)
  "window_w": 1280,                    // px
  "window_h": 720,                     // px (1280x720 HW target)
  "agc_enabled": false,                // AGC stage off by default
  "agc_target": 0.15,                  // 0.01 .. 0.5 RMS
  "agc_release_ms": 200,               // 10 .. 2000 ms
  "agc_floor": 0.05,                   // 0 .. 1 gain
  "enhancer_enabled": false,           // Enhancer stage off by default
  "enhancer_process": 0,               // 0 .. 12 dB
  "enhancer_contour": 0,               // 0 .. 12 dB
  "enhancer_mid": 0,                   // -6 .. 6 dB
  "limiter_enabled": false,            // limiter off by default
  "limiter_threshold_db": 0,           // -40 .. 0 dB
  "limiter_release_ms": 200,           // 10 .. 2000 ms
  "config_page": 0,                    // 0=MASTER 1=RESONANCE 2=CONVOLVER 3=AGC 4=ENHANCER 5=LIMITER
  "convolver_enabled": false,          // convolver off by default
  "convolver_mix": 0.15,               // 0 .. 1 wet/dry
  "convolver_choice": 0,               // 0=Body soft, 1=Body bright (modal)
  "convolver_decay": 0.5,              // 0 .. 1
  "convolver_tone": 0.6,               // 0 .. 1
  "convolver_size": 0.5,               // 0 .. 1
  "audio_block_size": 256,             // 32 .. 8192
  "audio_sample_rate": 48000           // JSON-only / read-only in GUI
}
```

Booleans are written as `true`/`false`; floats use the default `<<` formatting
(integral floats such as `200` print without a decimal point).

## 7. Notes & Caveats

- **DSP defaults = all stages off.** `convolver_enabled`, `agc_enabled`,
  `enhancer_enabled` and `limiter_enabled` all default to `false`, so a fresh
  config produces no change in audio behavior from the DSP chain.
- **Resonance is on by default** (`resonance_enabled: true`), unlike the DSP
  stages.
- **`config_page` index meaning:** `0`=MASTER, `1`=RESONANCE, `2`=CONVOLVER,
  `3`=AGC, `4`=ENHANCER, `5`=LIMITER. Values outside `0`–`5` are reset to `0`.
- **`resonance_window_ms`, `preload_ms`, `cache_budget_mb` and `audio_sample_rate`
  have no GUI control** — set only via this file (engine-init tuning, handy for
  embedded targets like RPi5). `max_resonance_voices` *does* have a live GUI
  control (the RESONANCE "MAX RESONANCE" slider).
- **OOM guard.** `cache_budget_mb` (auto = ~60 % RAM) caps bank load; exceeding it
  aborts the load with an error (incomplete bank) instead of crashing. Stream
  worker-thread counts are **auto-sized** from CPU core count at engine init (no
  JSON field) — ~half the cores for main streaming, ~quarter for resonance.
- **Off-screen window clamp.** At startup the GUI restores `window_x`/`window_y`,
  then checks whether at least a 100×100 px region of the window overlaps any
  connected monitor. If not (e.g. a monitor was unplugged since the last save),
  the window falls back to position `(100, 100)` and that fallback is written back.
- **Bank and MIDI are best-effort.** A non-loadable `bank_path` or an unmatched
  `midi_port_name` only logs a warning; the GUI still starts. A matched MIDI port
  has its exact label written back into `midi_port_name`.
- **Tracked-for-debounce vs. saved fields.** The debounce change-detection in
  `main.cpp` watches the bank/MIDI/voice/resonance/DSP/log/config-page fields
  (including `max_resonance_voices` and all six `convolver_*` fields), but not
  the `window_*` fields, `bank_search_dir`, `resonance_window_ms` or
  `audio_sample_rate`. Those non-tracked fields are still persisted because the
  full state is written on exit (and whenever any debounce save fires).

The default ranges in `persistence.h` and the `Param` tables **agree** — see
section 4 (no discrepancies found).
