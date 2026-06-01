# Ithaca Sample-Bank Format — NEW / FOLDER FORMAT (DESIGN PROPOSAL)

> ## ⚠️ PROPOSAL — NOT YET IMPLEMENTED
>
> **This format does not exist in code.** There is no loader, no parser, and no
> data model for it. This document is a **design proposal** intended as a basis
> for future work. Nothing here is binding: every "would", "should", and
> "proposed" item is open to revision by whoever implements it.
>
> The only hint of this format in the current codebase is a TYPE badge and a
> FUTURE comment in `app/gui/panel_bank.cpp:67-68`:
> *"FUTURE: `ctx.engine.bankType()` once there is a folder-type loader +
> autodetection; for now we only support legacy."* and the hardcoded `LEGACY ·
> auto` badge.
>
> For the format that **is** implemented today, see
> **[bank-format-legacy.md](bank-format-legacy.md)**.

---

## 1. Motivation — why a new format

The [legacy format](bank-format-legacy.md) encodes all structure in WAV
filenames (`m<midi>-vel<N>-f<sr>.wav`) and carries no metadata of its own. Its
documented limitations (legacy doc §12) motivate this proposal:

| Legacy limitation | Proposed improvement |
|-------------------|----------------------|
| No bank-level metadata | A `bank.json` manifest that describes the bank |
| Layer count / order inferred by measuring RMS | Explicit `velocity_layers` count + velocity→layer mapping |
| Sample rate inferred from headers / advisory `fSS` | Explicit `sample_rate` in the manifest |
| No per-sample loop/gain/tuning | Optional per-entry `gain_db`, `loop`, `tuning_cents` |
| One `"stereo"` mic only | Optional named `mic_layers` |
| Fixed filename grammar | Filenames become arbitrary; the manifest maps them |
| No version field | `version` for forward compatibility |

**Design principle:** the manifest is the source of truth; WAV filenames become
opaque references. This is additive — legacy banks keep working unchanged via
autodetection (§5).

---

## 2. Proposed directory layout

```
my-piano-bank/                 ← bank directory (name overridable in manifest)
├── bank.json                  ← REQUIRED manifest (presence selects new loader)
└── samples/                   ← all WAV files live here (path is manifest-relative)
    ├── A0_p.wav
    ├── A0_mf.wav
    ├── A0_ff.wav
    ├── ...
    ├── C4_p_close.wav         ← optional mic-layer variants
    ├── C4_p_room.wav
    └── ...
```

Notes (all non-binding):

- The manifest filename **would** be `bank.json`. `manifest.json` **could** be
  accepted as an alias (open question §7).
- Sample paths in the manifest **would** be resolved relative to the bank
  directory, so a `samples/` subfolder is a convention, not a requirement.
- Filenames are free-form; the manifest's `entries` table is what binds a file
  to a (note, layer, mic).

---

## 3. Proposed `manifest.json` schema

Annotated schema. Optional fields are marked `// optional`. Types are
informal.

```jsonc
{
  // --- Forward-compat / identity ---
  "format": "ithaca-bank",        // fixed discriminator string
  "version": 1,                   // integer; loader rejects versions it doesn't know

  "name": "VI Ravenscroft 275",   // human bank name (overrides directory name)
  "description": "...",           // optional, free text
  "credits": "...",               // optional
  "license": "...",               // optional

  // --- Global audio properties ---
  "sample_rate": 48000,           // Hz; authoritative declared rate
  "channels": 2,                  // 1 = mono, 2 = stereo (matches WAV reader output)

  // --- MIDI coverage ---
  "midi_range": { "from": 21, "to": 108 },   // inclusive; informational + validation

  // --- Velocity layering ---
  "velocity_layers": 8,           // explicit layer count (replaces RMS-inferred ordering)

  // velocity -> layer index mapping. Each row: incoming MIDI velocity range
  // [lo,hi] (1..127) maps to layer index (0-based, 0 = softest).
  // The loader would use THIS instead of measuring RMS at load time.
  "velocity_map": [
    { "vel": [1,   16],  "layer": 0 },
    { "vel": [17,  32],  "layer": 1 },
    { "vel": [33,  48],  "layer": 2 },
    { "vel": [49,  64],  "layer": 3 },
    { "vel": [65,  80],  "layer": 4 },
    { "vel": [81,  96],  "layer": 5 },
    { "vel": [97,  112], "layer": 6 },
    { "vel": [113, 127], "layer": 7 }
  ],

  // --- Optional mic layers (close/room/...) ---
  // optional; if absent, a single implicit "stereo" mic is assumed (legacy-like).
  "mic_layers": [                 // optional
    { "id": "close", "name": "Close",  "default_gain_db": 0.0 },
    { "id": "room",  "name": "Room",   "default_gain_db": -3.0 }
  ],

  // --- Sample entries ---
  // The core mapping table. One entry per (note, layer[, mic]). `file` is
  // relative to the bank dir. Optional per-entry overrides cover the metadata
  // legacy cannot express.
  "entries": [
    {
      "midi": 21,                 // MIDI note this sample plays
      "layer": 0,                 // velocity layer index (0-based)
      "mic": "close",             // optional; references mic_layers[].id; omit if no mics
      "file": "samples/A0_p_close.wav",

      "gain_db": 0.0,             // optional per-sample trim, applied at playback
      "tuning_cents": 0.0,        // optional pitch offset in cents
      "loop": {                   // optional sustain loop (frames, source SR)
        "start": 132000,
        "end":   180000,
        "mode":  "forward"        // "forward" | "ping-pong" (proposed)
      },
      "rms_db": -24.3             // optional precomputed peak RMS (skips load-time measure)
    }
    // ... one entry per sample ...
  ],

  // --- Optional explicit resonance/streaming hints ---
  "preload_ms": 150,              // optional; default falls back to engine config
  "resonance_window_ms": 500      // optional; default falls back to engine config
}
```

### Realistic minimal example

A small bank without mic layers, two notes, two velocity layers:

```json
{
  "format": "ithaca-bank",
  "version": 1,
  "name": "Tiny Demo Piano",
  "sample_rate": 48000,
  "channels": 2,
  "midi_range": { "from": 60, "to": 61 },
  "velocity_layers": 2,
  "velocity_map": [
    { "vel": [1, 64],   "layer": 0 },
    { "vel": [65, 127], "layer": 1 }
  ],
  "entries": [
    { "midi": 60, "layer": 0, "file": "samples/C4_soft.wav" },
    { "midi": 60, "layer": 1, "file": "samples/C4_loud.wav", "gain_db": -1.5 },
    { "midi": 61, "layer": 0, "file": "samples/Cs4_soft.wav" },
    { "midi": 61, "layer": 1, "file": "samples/Cs4_loud.wav" }
  ]
}
```

---

## 4. How this maps to the existing data model

The proposed manifest maps cleanly onto the existing
`Bank → NoteSlots[128] → VelocitySlot → SampleAsset → MicLayer` hierarchy
(see legacy doc §9, defined in `engine/sample/sample_types.h`), so a folder
loader **would not require a new in-memory model**:

- `velocity_layers` / `velocity_map` → ordering and count of `VelocitySlot`s
  (no RMS sort needed; `rms_db` taken from manifest or measured if absent).
- `mic_layers` + per-entry `mic` → multiple `MicLayer`s under one `SampleAsset`
  (the multi-mic axis that legacy leaves degenerate).
- per-entry `loop` / `gain_db` / `tuning_cents` → **new fields** that would need
  adding to `MicLayer` / `SampleAsset` (these do not exist today; an open
  question is whether to extend the structs or thread the metadata separately).
- `BankFormat` already has a third enum value usable for this format:
  `BankFormat::Extended` (`sample_types.h:16`). The proposal could reuse it or
  add a dedicated `BankFormat::Folder`.

---

## 5. Autodetection rule

Proposed selection logic, run when a bank directory is opened:

```
if (exists(dir / "bank.json") || exists(dir / "manifest.json"))
    → NEW folder format   → loadFolderBank(dir, ...)
else
    → fall back to LEGACY → loadLegacyBank(dir, ...)   // unchanged
```

This is purely additive: a directory with no manifest behaves exactly as today.
Banks that contain both a manifest and legacy-named WAVs would prefer the
manifest.

### Proposed engine API (parallel to legacy)

For structural parallelism with the implemented
`loadLegacyBank` (`engine/sample/sample_store.h:23-27`), a folder loader
**would** have a matching signature:

```cpp
// PROPOSED — does not exist.
Bank loadFolderBank(const std::string& dir, log::Logger& logger,
                    int cache_budget_mb = 0,
                    int midi_from = 0, int midi_to = 127,
                    int preload_ms = 150,            // overridden by manifest if present
                    int resonance_window_ms = 500);  // overridden by manifest if present
```

And the engine would gain a dispatcher plus a type query to back the GUI badge
(`panel_bank.cpp:67`):

```cpp
// PROPOSED.
BankFormat Engine::bankType() const;        // returns bank_.format
bool       Engine::loadBank(const std::string& dir) {
    bank_ = (detectBankFormat(dir) == BankFormat::Legacy)
              ? loadLegacyBank(dir, ...)
              : loadFolderBank(dir, ...);
    return bank_.loaded_samples > 0;
}
```

The GUI's TYPE badge (`app/gui/panel_bank.cpp:69-71`, currently hardcoded
`LEGACY`) would then read `ctx.engine.bankType()` and the `· auto` suffix would
reflect that detection actually happened.

---

## 6. Migration: wrapping an existing legacy bank

An existing legacy bank can be upgraded **without moving or renaming any WAV
files** by dropping a generated `bank.json` next to them that points at the
existing `m###-vel#-f##.wav` files:

```json
{
  "format": "ithaca-bank",
  "version": 1,
  "name": "vi-ravenscroft",
  "sample_rate": 48000,
  "channels": 2,
  "midi_range": { "from": 21, "to": 108 },
  "velocity_layers": 8,
  "velocity_map": [
    { "vel": [1, 16],    "layer": 0 },
    { "vel": [17, 32],   "layer": 1 },
    { "vel": [33, 48],   "layer": 2 },
    { "vel": [49, 64],   "layer": 3 },
    { "vel": [65, 80],   "layer": 4 },
    { "vel": [81, 96],   "layer": 5 },
    { "vel": [97, 112],  "layer": 6 },
    { "vel": [113, 127], "layer": 7 }
  ],
  "entries": [
    { "midi": 21, "layer": 0, "file": "m021-vel0-f48.wav" },
    { "midi": 21, "layer": 1, "file": "m021-vel1-f48.wav" },
    // ... 704 entries, one per legacy WAV ...
    { "midi": 108, "layer": 7, "file": "m108-vel7-f48.wav" }
  ]
}
```

A small converter tool **could** generate this by running the existing
`scanBank()` / `parseLegacyName()` logic (`bank_index.cpp:28-44`, `71-114`) over
the directory and emitting one entry per parsed file, using the `vel` tag as the
`layer` index and `fSS` as `sample_rate`. Because the WAV files are untouched,
the bank still loads as legacy when the manifest is removed.

---

## 7. Open questions for the implementer

These are explicitly **unresolved**; the implementer decides:

1. **Manifest filename.** `bank.json` only, or also accept `manifest.json`? The
   GUI scan in `panel_bank.cpp:17-26` treats any subdirectory as a bank
   candidate, so detection must be cheap.
2. **`BankFormat` enum.** Reuse `BankFormat::Extended`
   (`sample_types.h:16`, currently rejected by `loadLegacyBank` at
   `sample_store.cpp:32-37`) or add `BankFormat::Folder`?
3. **Struct extension vs. side table.** `loop`, `gain_db`, `tuning_cents` have no
   home in `MicLayer`/`SampleAsset` today. Extend the structs, or carry the
   metadata in a parallel map keyed by sample?
4. **Velocity selection authority.** Legacy reorders by measured RMS
   (`sample_store.cpp:152-158`). The new format declares `velocity_map`
   explicitly — should the loader still measure RMS as a fallback when
   `rms_db` is omitted, or require it?
5. **Mic-layer playback.** Multiple mics imply summing/mixing logic in the voice
   path that does not exist yet; what is the mixing/gain model?
6. **Loop support in the streaming engine.** The current streaming model
   (head + resonance window, legacy doc §8) has no loop concept; sustain loops
   would need ring-buffer/loop-point support.
7. **Path resolution & security.** Should sample paths be restricted to within
   the bank directory (no `..` escapes)?
8. **Validation policy.** On a malformed/partial manifest, fail hard or
   degrade gracefully the way legacy silently skips bad files?
9. **Sample-rate mismatch.** If a WAV header disagrees with the manifest
   `sample_rate`, warn, resample, or reject?

---

## 8. Status recap

Nothing in this document is implemented. The implemented format is
[legacy](bank-format-legacy.md). This proposal exists so that future folder-bank
work has a concrete starting point rather than starting from the bare GUI hint
in `app/gui/panel_bank.cpp`.
