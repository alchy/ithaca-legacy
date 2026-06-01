# Ithaca Sample-Bank Format — LEGACY (Reference)

> Status: **implemented and current**. This is the only bank format the engine
> can load today (`loadLegacyBank`). For a forward-looking, manifest-based
> alternative see the **[proposed format](bank-format-proposed.md)** — note that
> the proposed format is a design document only and has **no loader in code**.

This document describes the legacy sample-bank format used by the Ithaca
streaming piano sampler. Everything below is grounded in the engine source; the
key parsing logic is cited by `file:line`.

---

## 1. Overview

A legacy bank is **just a directory of WAV files**. There is no manifest, no
index file, and no per-bank metadata. All structural information (which MIDI
note, which velocity layer, the recorded sample rate) is **encoded in each
file's name**. The directory name itself becomes the bank name
(`sample_store.cpp:21`: `bank.name = path(dir).filename()`).

The reference bank shipped for development is
`/Users/j/SoundBanks/Ithaca/vi-ravenscroft`:

- 88 notes (MIDI 21..108) × 8 velocity layers (`vel0`..`vel7`) = **704 WAV files**.
- Every file matches `m<midi>-vel<N>-f48.wav`, e.g. `m021-vel0-f48.wav`.
- No other files are present in the directory.

---

## 2. Directory layout

```
vi-ravenscroft/                ← bank directory (its name = bank name)
├── m021-vel0-f48.wav          ← MIDI 21 (A0), velocity layer 0, recorded @48 kHz
├── m021-vel1-f48.wav
├── ...
├── m021-vel7-f48.wav
├── m022-vel0-f48.wav
├── ...
└── m108-vel7-f48.wav          ← MIDI 108 (C8), velocity layer 7
```

The bank directory is scanned **non-recursively** — only regular files in the
top level are considered (`bank_index.cpp:79-82`,
`fs::directory_iterator`; subdirectories and non-regular files are skipped).

---

## 3. Filename grammar

The legacy filename is parsed by a single case-insensitive regular expression in
`parseLegacyName()` at **`engine/sample/bank_index.cpp:14-18`**:

```cpp
// engine/sample/bank_index.cpp:15
static const std::regex re(R"(m(\d{3})-vel(\d)-f(\d{2,3})\.wav)",
                           std::regex::icase);
```

### Grammar (EBNF-ish)

```
legacy-name = "m" , midi , "-vel" , vel , "-f" , sr , ".wav"
midi        = 3 DIGIT          ; exactly three digits, e.g. 021, 060, 108
vel         = 1 DIGIT          ; exactly one digit, 0..9 (only 0..7 used in practice)
sr          = 2*3 DIGIT        ; two or three digits — sample-rate tag, e.g. 48, 44, 96
```

Matching is **case-insensitive** (`std::regex::icase`), so `M021-VEL0-F48.WAV`
parses identically. The match must be a *full* match (`std::regex_match`,
`bank_index.cpp:31`), so any extra prefix/suffix or differing separators fail to
parse and the file is skipped.

### Token semantics

| Token | Field in `ParsedName` | Meaning | Notes |
|-------|----------------------|---------|-------|
| `m<NNN>` | `midi` (int) | MIDI note number | Parsed `bank_index.cpp:35`. Regex forces 3 digits, so the usable range is `000`..`999`, but the loader clamps to valid MIDI (see §4). |
| `vel<N>` | `vel` (int) | Velocity layer index | Parsed `bank_index.cpp:36`. One digit `0..9`. Conventionally `0` = softest, `7` = loudest. |
| `f<SS>` | `sr_tag` (int) | Recorded sample-rate tag in kHz | Parsed `bank_index.cpp:37`. `f48` = 48 kHz, `f44` = 44.1 kHz, `f96` = 96 kHz. **Advisory only — see §6.** |

The integer conversions are wrapped in `try/catch` (`bank_index.cpp:34-40`):
even though the regex bounds the digit counts, a parse failure yields an
unparseable `ParsedName` rather than throwing out of `scanBank`.

### Concrete examples

| Filename | midi | vel | sr_tag | Result |
|----------|------|-----|--------|--------|
| `m021-vel0-f48.wav` | 21 | 0 | 48 | OK (A0, softest, 48 kHz) |
| `m108-vel7-f48.wav` | 108 | 7 | 48 | OK (C8, loudest) |
| `m060-vel3-f96.wav` | 60 | 3 | 96 | OK (middle C, 96 kHz tag) |
| `m21-vel0-f48.wav`  | — | — | — | **Skipped** (midi not 3 digits) |
| `m021-vel0.wav`     | — | — | — | **Skipped** (no `-fSS` token) |
| `piano_C4.wav`      | — | — | — | **Skipped** (does not match) |

---

## 4. MIDI range

The grammar allows any 3-digit note number, but **the loader filters to valid
MIDI** at `sample_store.cpp:49`:

```cpp
if (p.midi < 0 || p.midi > 127) continue;          // out of MIDI range → skipped
```

In addition, `loadLegacyBank` accepts a `[midi_from, midi_to]` inclusive window
(default `0..127`) and skips notes outside it (`sample_store.cpp:50`). The
engine passes `cfg_.midi_from` / `cfg_.midi_to` (`engine.cpp:55`); the CLI
`inspect` path uses the defaults (full range). This window exists purely to load
a subset quickly for testing/rendering without paging in a multi-GB bank
(`sample_store.h:18-19`).

The reference bank uses MIDI 21 (A0) .. 108 (C8), the 88 keys of a standard
piano.

---

## 5. Velocity layering

The convention is `vel0` (softest) .. `vel7` (loudest), 8 layers per note in the
reference bank. **However, the loader does not trust the numeric tag for
ordering.** Instead:

1. Each WAV file becomes one `VelocitySlot` containing a single `SampleAsset`
   with a single `MicLayer` (`sample_store.cpp:130-139`).
2. The slot's representative loudness is the **measured peak RMS in dBFS**
   (`measurePeakRmsDb`, `sample_store.cpp:101`, `slot.rms_db = rms`).
3. After all files load, the slots of each note are **sorted ascending by
   measured RMS** (softest → loudest) at `sample_store.cpp:152-158`:

   ```cpp
   std::sort(slots.begin(), slots.end(),
             [](const VelocitySlot& a, const VelocitySlot& b) {
                 return a.rms_db < b.rms_db;
             });
   ```

So the `velN` token only determines *which file* exists; the **measured RMS is
authoritative** for layer ordering (`sample_store.cpp:150-151` comment: "Legacy
vel tag is usually already sorted, but RMS is authoritative"). Velocity→layer
*selection at play time* is performed downstream in the voice/engine layer using
these RMS-sorted slots; this loader only produces the sorted layer table.

There is no requirement that all 8 layers be present — see §7.

---

## 6. Sample-rate token vs. real sample rate

The `fSS` token (`sr_tag`) is **parsed but never used during loading**. The
actual sample rate of each sample comes from the WAV header via
`peekWavInfo()` (`sample_store.cpp:52,62`: `mic.file.sample_rate =
info.sample_rate`). All preload/resonance frame math derives from
`info.sample_rate`, not from `sr_tag` (e.g. `sample_store.cpp:67-68`,
`111`). The token is therefore **advisory/documentary** — useful for humans and
tooling, ignored by the engine. A mismatch between `fSS` and the WAV header has
no effect on playback.

---

## 7. Channel handling

The WAV reader **always returns interleaved stereo float** in `[-1, 1]`
(`engine/io/wav_reader.h:4,17`). Mono source files are up-mixed by duplicating
the single channel into both L and R (`wav_reader.cpp:115`,
`R = (channels >= 2) ? ... : L`). Consequently:

- The `MicLayer` is always labelled `"stereo"` (`sample_store.cpp:60`).
- All in-RAM buffers (`preload_head`, `preload_resonance`) are interleaved
  stereo regardless of the source file's channel count.
- Supported source widths are mono (1) and stereo (2); the reader reads the
  channel count from `fmt.channels` in the WAV header.

---

## 8. Preload, resonance, and streaming

The bank is **not fully resident** by default. For each sample the loader keeps
only the regions needed in RAM and leaves the rest on disk for streaming
(`sample_store.cpp:44-46`). Two parameters control this:

- `preload_ms` (default 150, `sample_store.h:26` / `engine.h:33`): how many ms
  of the **head** of each sample to keep in RAM.
- `resonance_window_ms` (default 500, `sample_store.h:27` / `engine.h:34`):
  length of the **resonance** region kept in RAM for streamed samples.

### Head / mode decision (`sample_store.cpp:66-76`)

```cpp
const int preload_frames        = preload_ms * sample_rate / 1000;
const int short_threshold_frames = preload_frames * 2;
if (info.frames <= short_threshold_frames) {
    mic.mode = MicLayerMode::FullyLoaded;   // short sample: hold entire file
    mic.head_frames = info.frames;
} else {
    mic.mode = MicLayerMode::Streamed;      // long sample: hold only the head
    mic.head_frames = preload_frames;
}
```

- **FullyLoaded**: a "short" sample (fits in `2 × preload_ms`) is held entirely
  in `preload_head`; no streaming is needed.
- **Streamed**: a "long" sample keeps only `preload_ms` worth of head in
  `preload_head`; the tail is streamed from `file` (the on-disk WAV) at play
  time.

The head region `[0 .. head_frames)` is read via `readWavRange`
(`sample_store.cpp:79`). If the file is shorter than requested (truncated),
`head_frames` is reduced to what was actually read and the mode may fall back to
`FullyLoaded` (`sample_store.cpp:85-95`).

### Resonance region (`sample_store.cpp:109-128`)

For **Streamed** mics with `resonance_window_ms > 0`, the loader also preloads a
resonance region used later by resonance voices (skip-attack sustain source):

- It starts at `resonance_start_frame = attack_end_frame` — the approximate
  attack/sustain boundary located by `findAttackEnd` (peak short-window RMS
  position; `sample_store.cpp:103,110`).
- It spans up to `resonance_window_ms`, clamped to the frames the file actually
  contains (`sample_store.cpp:111-115`).
- For **FullyLoaded** mics no separate resonance buffer is needed (the whole
  sample is already in `preload_head`; `sample_store.cpp:107-108`).

`resident_frames` and `total_bytes` accumulate only the **resident** regions
(head + resonance), giving a RAM estimate (`sample_store.cpp:143-146`).

---

## 9. In-memory data model

The loader builds the structure defined in `engine/sample/sample_types.h`:

```
Bank
 ├─ name, path, format (= Legacy)
 ├─ notes[128] : NoteSlots
 │    ├─ recorded : bool        (true if any sample loaded for this note)
 │    └─ slots[]  : VelocitySlot   (sorted ascending by rms_db)
 │         ├─ rms_db
 │         └─ variants[] : SampleAsset
 │              ├─ peak_rms_db, attack_end_frame
 │              └─ mics[] : MicLayer
 │                   ├─ mic_name ("stereo" for legacy)
 │                   ├─ file (path, frames, sample_rate)
 │                   ├─ mode (FullyLoaded | Streamed)
 │                   ├─ preload_head[]      + head_frames
 │                   └─ preload_resonance[] + resonance_start_frame, resonance_frames
 └─ resident_frames, total_bytes, loaded_samples   (diagnostics)
```

For legacy banks the round-robin and multi-mic axes are degenerate: each
`VelocitySlot` has exactly one `SampleAsset`, and each `SampleAsset` has exactly
one `MicLayer` named `"stereo"`.

---

## 10. How it's loaded — `loadLegacyBank` summary

Signature (`sample_store.h:23-27`):

```cpp
Bank loadLegacyBank(const std::string& dir, log::Logger& logger,
                    int cache_budget_mb = 0,
                    int midi_from = 0, int midi_to = 127,
                    int preload_ms = 150,
                    int resonance_window_ms = 500);
```

Behaviour (`sample_store.cpp:14-173`):

1. Set `bank.path = dir`, `bank.name = directory name` (lines 20-21).
2. `scanBank(dir)` classifies the directory and returns the parsed files
   (line 23). Format is chosen by majority of recognized files
   (`bank_index.cpp:99-112`): if `legacy_count >= extended_count` the bank is
   `Legacy`. Unrecognized files are counted in `scan.skipped`.
3. If format is `Unknown` → warn and return an empty bank (lines 26-31).
   If format is `Extended` → warn "extended not yet supported (phase 7)" and
   return an empty bank (lines 32-37). **Only `Legacy` proceeds.**
4. For each parsed file: filter by MIDI validity and `[midi_from, midi_to]`
   (lines 49-50); `peekWavInfo` for header (line 52); build a `MicLayer`; decide
   FullyLoaded/Streamed; read head; measure RMS and attack end; optionally read
   resonance region; append a `VelocitySlot` to `notes[midi]` and mark it
   `recorded` (lines 59-147).
5. Sort each note's slots ascending by RMS (lines 152-158).
6. Log totals; if `cache_budget_mb > 0` and the resident estimate exceeds it,
   emit a WARNING only — the loader never evicts or refuses to load
   (lines 160-171, and `sample_store.h:16-17`).

### Invocation sites

- `Engine::loadBank` (`engine/engine.cpp:52-58`) calls `loadLegacyBank` with
  `cache_budget_mb = 0` and the engine's configured `midi_from/midi_to/
  preload_ms/resonance_window_ms`. Returns success when `loaded_samples > 0`.
- `Engine::reloadBank` (`engine.cpp:60+`) performs a graceful drain → silence →
  reload → resume around `loadBank`, so banks can be swapped at runtime
  (used by the GUI RELOAD button / bank dropdown, `app/gui/panel_bank.cpp`).
- CLI `play`/`render` go through `Engine::loadBank`
  (`app/cli/main.cpp:164,253`); CLI `inspect` calls `loadLegacyBank` directly
  with defaults (`app/cli/main.cpp:268`).

---

## 11. Required vs. optional

| Aspect | Status |
|--------|--------|
| Directory of WAV files matching the grammar | **Required** |
| 3-digit MIDI token in filename | **Required** (else skipped) |
| `velN` token | **Required** by grammar (else skipped); numeric value advisory |
| `fSS` token | **Required** by grammar to match; value advisory/ignored |
| Full 88-note coverage | Optional — missing notes simply have `recorded = false` |
| All 8 velocity layers per note | Optional — a note may have any number of slots |
| Stereo vs. mono source | Either; mono is up-mixed |
| Manifest / metadata file | **Not used** (none exists; extra files are skipped, counted in `skipped`) |

Missing files / partial ranges: there is no validation that a note has any
layers or that layers are contiguous. Any note with no matching files simply
remains `recorded = false`; gaps are silently tolerated.

---

## 12. Known limitations

- **No bank-level metadata file.** Bank name, sample rate, note range, and layer
  count are all inferred from filenames and WAV headers; the bank cannot
  describe itself, version itself, or carry credits/licensing.
- **Everything is naming-encoded.** A typo in a filename silently drops a sample
  (skipped, no hard error).
- **The `fSS` token is unverified and ignored** by the loader (§6); it can
  disagree with the actual WAV header with no consequence.
- **No per-sample metadata.** No loop points, no gain trim, no tuning offset, no
  per-layer crossfade hints. Loudness ordering is reconstructed by measuring RMS
  at load time.
- **No mic layers.** Exactly one `"stereo"` mic per sample; close/room/etc.
  perspectives cannot be expressed.
- **No round-robin.** One sample per (note, layer); repeated notes reuse the same
  recording.
- **Fixed conventions baked into a regex.** The 3-digit MIDI / 1-digit velocity /
  2–3-digit SR shape is hard-coded in `bank_index.cpp:15`; deviating filename
  schemes are not loadable.
- **No eviction / streaming budget enforcement.** `cache_budget_mb` only logs a
  warning (`sample_store.cpp:165-171`).

For a design that addresses these limitations, see
**[bank-format-proposed.md](bank-format-proposed.md)** (proposal only — not
implemented).
