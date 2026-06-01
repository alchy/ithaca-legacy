# ithaca-gui Configuration File (`state.json`)

## 1. Overview

`state.json` is the persisted configuration for **ithaca-gui** (the F8 GUI front-end).
It stores window geometry, the selected bank and MIDI port, voice parameters, the
DSP-chain settings, the active CONFIG page, and the log level.

The file is **auto-managed** by ithaca-gui — you normally never edit it by hand:

- It is created on first clean exit if it does not exist (the GUI starts from
  built-in defaults when no file is present).
- During a session it is written with a **debounced save**: after any tracked
  value changes, the GUI waits until **~1 second of idle** (no further change)
  and then writes the file. This avoids writing on every frame while a slider is
  being dragged.
- It is also written **unconditionally on clean exit** (when the window is closed),
  capturing the final window geometry.

Writes are atomic: the GUI writes to `state.json.tmp` and then renames it over
`state.json` (see `saveState` in `app/gui/persistence.cpp`).

The format is a flat JSON object (no nested objects or arrays). It is parsed by a
minimal hand-written parser, not a full JSON library, so keep it flat if you do
edit it manually.

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
  defaults.
- **v3 → v4 migration is implicit and lossless.** A v3 file simply lacks the DSP
  keys (`agc_*`, `bbe_*`, `limiter_*`, `config_page`). Those keys are read
  **defensively**: a missing key falls back to the struct default (DSP stages off,
  `config_page` 0). After a successful load, `schema_version` is unconditionally
  set to `4` in memory, so the **next save rewrites the file as v4** while keeping
  all existing values. No v3 data is lost.
- **Missing keys → defaults.** The defensive read also applies within v4: any DSP
  key that is absent (e.g. a hand-trimmed file) is replaced by its default. Note
  that the *non-DSP* numeric keys (`master_gain_db`, `window_x`, etc.) are **not**
  read defensively — if those keys are missing or non-numeric, `std::stof`/`std::stoi`
  throws and the whole load fails, dropping you back to all defaults.
- `log_level` is special-cased: if the key is empty/absent it defaults to `"info"`.
- `midi_channel` is special-cased: if the key is empty/absent it defaults to `-1`.

## 4. Field Reference

Every field of the `GuiState` struct (`app/gui/persistence.h`) is listed below.
Ranges/units for the voice and DSP fields come from the `Param` tables in
`app/gui/voice_page.h` and `engine/dsp/{agc,bbe,limiter}.cpp`; the GUI clamps
values to these ranges when set via a control, and the DSP stages additionally
clamp on `set()`.

### Meta

| JSON key         | Type | Default | Allowed values | Meaning | Set by |
|------------------|------|---------|----------------|---------|--------|
| `schema_version` | int  | `4`     | accepts 3 or 4 on load; always written as 4 | File format version | init/migration only |

### Window geometry

| JSON key   | Type | Default | Range | Meaning | Set by |
|------------|------|---------|-------|---------|--------|
| `window_x` | int  | `100`   | any (off-screen clamped) | Window X position (screen px) | GUI (tracked every frame) |
| `window_y` | int  | `100`   | any (off-screen clamped) | Window Y position (screen px) | GUI (tracked every frame) |
| `window_w` | int  | `1280`  | any | Window width (px) | GUI (tracked every frame) |
| `window_h` | int  | `820`   | any | Window height (px) | GUI (tracked every frame) |

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
| `midi_channel`   | int    | `-1`    | `-1` = OMNI, `0`–`15` = channel (0-based) | MIDI receive channel | GUI / init |

### Voice parameters

These are applied to the engine at init via `EngineConfig` and adjustable live on
the **VOICE** CONFIG page. Ranges from `VoicePage::kParams` (`app/gui/voice_page.h`).

| JSON key               | Type  | Default  | Range          | Unit | Meaning | Set by |
|------------------------|-------|----------|----------------|------|---------|--------|
| `master_gain_db`       | float | `0.0`    | `-60` … `6`    | dB   | Master output gain (converted to linear via `10^(dB/20)`) | GUI (VOICE) |
| `resonance_strength`   | float | `0.5`    | `0` … `1`      | —    | Resonance strength | GUI (VOICE) |
| `release_ms`           | float | `200.0`  | `50` … `2000`  | ms   | Voice release time | GUI (VOICE) |
| `excite_decay_ms`      | float | `5000.0` | `500` … `30000`| ms   | Excitation decay time | GUI (VOICE) |
| `max_resonance_voices` | int   | `32`     | `1` … `64`     | —    | Max resonance voice count. **Read-only / init-only** — the VOICE "MAX RESONANCE" control is read-only; the value is applied at engine init and cannot be changed live. | init only |

### DSP chain — AGC (stage 0)

Ranges from `AGC::kParams` (`engine/dsp/agc.cpp`). All defaults match the
`GuiState` defaults.

| JSON key          | Type  | Default | Range          | Unit | Meaning | Set by |
|-------------------|-------|---------|----------------|------|---------|--------|
| `agc_enabled`     | bool  | `false` | `true`/`false` | —    | Enable AGC stage | GUI (AGC page) |
| `agc_target`      | float | `0.15`  | `0.01` … `0.5` | RMS (linear) | Target RMS the AGC follows down to | GUI (AGC page) |
| `agc_release_ms`  | float | `200.0` | `10` … `2000`  | ms   | AGC release time (attack is fixed at 5 ms) | GUI (AGC page) |
| `agc_floor`       | float | `0.05`  | `0` … `1`      | gain (linear) | Minimum gain floor (AGC never attenuates below this) | GUI (AGC page) |

### DSP chain — BBE (stage 1)

Ranges from `BBE::kParams` (`engine/dsp/bbe.cpp`).

| JSON key         | Type  | Default | Range     | Unit | Meaning | Set by |
|------------------|-------|---------|-----------|------|---------|--------|
| `bbe_enabled`    | bool  | `false` | `true`/`false` | — | Enable BBE stage | GUI (BBE page) |
| `bbe_definition` | float | `0.0`   | `0` … `12`| dB   | DEFINITION (high-shelf at 5 kHz) | GUI (BBE page) |
| `bbe_bass`       | float | `0.0`   | `0` … `10`| dB   | BASS (low-shelf at 180 Hz) | GUI (BBE page) |

### DSP chain — Limiter (stage 2)

Ranges from `Limiter::kParams` (`engine/dsp/limiter.cpp`).

| JSON key               | Type  | Default | Range          | Unit | Meaning | Set by |
|------------------------|-------|---------|----------------|------|---------|--------|
| `limiter_enabled`      | bool  | `false` | `true`/`false` | —    | Enable limiter stage | GUI (LIMITER page) |
| `limiter_threshold_db` | float | `0.0`   | `-40` … `0`    | dB   | Peak threshold | GUI (LIMITER page) |
| `limiter_release_ms`   | float | `200.0` | `10` … `2000`  | ms   | Limiter release time (attack is fixed at 1 ms) | GUI (LIMITER page) |

### DSP chain — CONFIG page selector

| JSON key      | Type | Default | Allowed values | Meaning | Set by |
|---------------|------|---------|----------------|---------|--------|
| `config_page` | int  | `0`     | `0`–`3`        | Selected CONFIG page: `0`=VOICE, `1`=AGC, `2`=BBE, `3`=LIMITER. Out-of-range values are clamped to `0` at startup. | GUI (CONFIG switch) |

### Log

| JSON key    | Type   | Default  | Allowed values | Meaning | Set by |
|-------------|--------|----------|----------------|---------|--------|
| `log_level` | string | `"info"` | `debug` \| `info` \| `warn` \| `error` \| `fatal` (also `off`/`warning` accepted by the parser) | Minimum log severity, applied via `setMinSeverity`. Unparseable values fall back to `info`. | `--log-level` CLI flag (overrides on startup) / GUI (live) |

The severity strings are parsed by `severity_from_string` (`engine/util/log.h`):
`debug`, `info`, `warn`/`warning`, `error`, `fatal` (case-insensitive). `Off`
exists as an internal severity that suppresses all output, and the documented set
written by the CLI usage text is `debug | info | warn | error | fatal`.

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

The following is a valid v4 file matching the exact key order and format emitted
by `saveState`. (JSON does not allow comments; the trailing `//` notes are for
documentation only — remove them if you paste this into a real file.)

```json
{
  "schema_version": 4,                                  // always 4
  "bank_search_dir": "/Users/me/banks",                 // dir scanned by bank dropdown
  "bank_path": "/Users/me/banks/grand.bank",            // bank loaded at startup
  "midi_port_name": "MPK mini",                          // substring-matched MIDI port
  "log_level": "info",                                   // debug|info|warn|error|fatal
  "midi_channel": -1,                                    // -1 = OMNI, 0..15 = channel
  "master_gain_db": 0,                                   // -60 .. 6 dB
  "resonance_strength": 0.5,                             // 0 .. 1
  "release_ms": 200,                                     // 50 .. 2000 ms
  "excite_decay_ms": 5000,                               // 500 .. 30000 ms
  "max_resonance_voices": 32,                            // 1 .. 64 (init-only)
  "window_x": 100,                                       // px (off-screen clamped)
  "window_y": 100,                                       // px (off-screen clamped)
  "window_w": 1280,                                      // px
  "window_h": 820,                                       // px
  "agc_enabled": false,                                  // AGC stage off by default
  "agc_target": 0.15,                                    // 0.01 .. 0.5 RMS
  "agc_release_ms": 200,                                 // 10 .. 2000 ms
  "agc_floor": 0.05,                                     // 0 .. 1 gain
  "bbe_enabled": false,                                  // BBE stage off by default
  "bbe_definition": 0,                                   // 0 .. 12 dB
  "bbe_bass": 0,                                          // 0 .. 10 dB
  "limiter_enabled": false,                              // limiter off by default
  "limiter_threshold_db": 0,                             // -40 .. 0 dB
  "limiter_release_ms": 200,                             // 10 .. 2000 ms
  "config_page": 0                                       // 0=VOICE 1=AGC 2=BBE 3=LIMITER
}
```

The values above are exactly the built-in defaults (with example bank/MIDI
strings filled in). Booleans are written as `true`/`false`; floats are written by
the default `<<` formatting (e.g. integral floats such as `200` print without a
decimal point, as shown).

## 7. Notes & Caveats

- **DSP defaults = all stages off.** `agc_enabled`, `bbe_enabled` and
  `limiter_enabled` all default to `false`, so a fresh config produces no change
  in audio behavior from the DSP chain.
- **`config_page` index meaning:** `0` = VOICE, `1` = AGC, `2` = BBE,
  `3` = LIMITER. Values outside `0`–`3` are reset to `0` at startup.
- **Off-screen window clamp.** At startup the GUI restores `window_x`/`window_y`,
  then checks whether at least a 100×100 px region of the window overlaps any
  connected monitor. If not (e.g. a monitor was unplugged since the last save),
  the window falls back to position `(100, 100)` and that fallback is written
  back into the state, so the corrected position is persisted on the next save.
- **`max_resonance_voices` is init-only.** It is applied to the engine at init via
  `EngineConfig`; the VOICE page "MAX RESONANCE" control is read-only. Changing it
  requires editing the file (or it being changed by some future control) and
  restarting.
- **Bank and MIDI are best-effort.** A non-loadable `bank_path` or an unmatched
  `midi_port_name` only logs a warning; the GUI still starts. A matched MIDI port
  has its exact label written back into `midi_port_name`.
- **Tracked-for-debounce vs. saved fields.** The debounce change-detection in
  `main.cpp` watches the bank/MIDI/voice/DSP/log/config-page fields (not the
  `window_*` fields and not `bank_search_dir`/`max_resonance_voices`). Window
  geometry and any CLI override are still persisted because the full state is
  written on exit (and whenever a debounce save fires for any reason).
```

The default ranges in `persistence.h` and the DSP `Param` tables **agree** — see
section 4 (no discrepancies found).
