# Ithaca Sample-Bank Format — NEW / FOLDER FORMAT (DESIGN PROPOSAL)

> ## ⚠️ PROPOSAL — NOT YET IMPLEMENTED
>
> **This format does not exist in code.** There is no loader, no parser, and no
> autodetection for it yet. This document is a **design proposal** intended as a
> basis for future work. Nothing here is binding; everything marked "would",
> "should", or "proposed" is open to revision by whoever implements it.
>
> The only hint of this format in the current codebase is a TYPE badge and a
> FUTURE comment in `app/gui/panel_bank.cpp:67-68`
> (*"FUTURE: `ctx.engine.bankType()` once there is a folder-type loader +
> autodetection; for now only legacy is supported"*).
>
> For the format that **is** implemented today, see the
> [legacy format reference](bank-format-legacy.md).

---

## 1. Overview

The new format is a **folder-per-note** layout. Each MIDI note gets its own
subfolder; inside it sits an arbitrary number of WAV samples (the velocity
layers for that note), each named by a **short content hash**. There is **no
velocity encoded in the filename** and (in the base proposal) **no manifest**:

```
<BankName>/
├── m021/
│   ├── 3f9a1c2b.wav        ← one velocity layer for MIDI 21 (A0)
│   ├── a7e0445d.wav        ← another layer
│   └── c81b9f30.wav
├── m022/
│   ├── 9d2e7a10.wav
│   └── 11ff6c8e.wav
├── ...
└── m108/
    ├── 0042ab7e.wav
    └── ...
```

The crucial idea: **the loader derives the entire velocity layering at load
time.** For each note it reads every WAV in the folder, measures loudness, sorts
the samples, and uses *that* as the note's velocity layers — so different notes
can have **different numbers of layers**, and the velocity resolution varies
across the keyboard accordingly.

This is deliberately a small step from legacy. The legacy loader *already*
builds a per-note, variable-length, RMS-sorted layer table
([legacy §5](bank-format-legacy.md#5-velocity-layering)) and the playback path
*already* maps incoming MIDI velocity onto a variable slot count
(`patch_manager.cpp:18-25`). **The new format reuses that in-memory model and
that velocity mapping unchanged** — only the on-disk layout and the loader
change.

---

## 2. Directory & file layout

### 2.1 Note folders

- One subfolder per recorded MIDI note, named `m` + **3-digit zero-padded MIDI
  number** — the same `m###` convention as legacy (`m021` = MIDI 21 = A0,
  `m108` = MIDI 108 = C8). Notes not represented simply have no folder.
- The bank name is the top-level directory name (same rule as legacy:
  `bank.name = path(dir).filename()`).
- Scanning is per-note: the loader lists `m###/` subfolders, then lists the WAV
  files inside each. (Legacy, by contrast, scans a single flat directory.)

### 2.2 Sample filenames — short content hash

Each WAV is named by a **truncated content hash** (e.g. the first 8–12 hex
characters of a SHA-256 over the file's audio data), plus the `.wav` extension:
`3f9a1c2b.wav`.

Rationale:

- **Opaque to the loader.** The loader never parses the name — ordering comes
  from measured loudness (§4), not from the filename. The hash only has to be
  **unique within its note folder**.
- **Content-addressed.** Identical recordings dedupe to the same name; a
  re-export with changed audio gets a new name automatically.
- **No naming constraints.** No velocity index, sample-rate tag, or ordering is
  baked into the name — removing the brittle, semantics-in-the-filename problem
  of the legacy format.

> A real hash is the intended convention, but the format only *requires*
> per-folder-unique WAV filenames. The hash scheme itself is not validated by the
> loader.

### 2.3 What is stored vs. derived

Nothing structural is stored on disk except **the MIDI note (folder name)**.
Everything else is derived at load time, exactly like legacy derives loudness:

| Property            | Source |
|---------------------|--------|
| MIDI note           | Folder name `m###` |
| Sample rate         | WAV header (per file), via `peekWavInfo` — not declared |
| Channels            | WAV header; mono is up-mixed to stereo (as legacy, `wav_reader.cpp:115`) |
| Number of velocity layers | **= number of WAV files in the note folder** (varies per note) |
| Layer ordering (soft→loud) | **Measured peak RMS in dBFS** (§4) |
| Velocity → layer mapping | Derived at play time from the layer count (§5) |

There is **no manifest in the base proposal** — this keeps the format as simple
as legacy while removing the filename-encoding problems. Optional bank-level
metadata (display name, license, per-sample loop/gain) is discussed as an
extension in §8.

---

## 3. In-memory model (unchanged from legacy)

The loader would produce the **same** `Bank` structure legacy produces
(`engine/sample/sample_types.h`), so the voice/engine layers need no changes:

```
Bank
 └─ notes[midi] : NoteSlots          (recorded = true if the m### folder exists)
      └─ slots[] : VelocitySlot       (sorted ascending by rms_db; length = file count)
           ├─ rms_db                  (measured peak RMS, soft→loud ordering key)
           └─ variants[] : SampleAsset
                ├─ peak_rms_db, attack_end_frame
                └─ mics[] : MicLayer  (single "stereo" mic, head + optional resonance)
```

`NoteSlots.slots` is already a variable-length vector sorted ascending by
`rms_db` (`sample_types.h:75-77`). The new loader simply fills it from a folder
instead of from `velN`-tagged flat files.

---

## 4. Loader algorithm (proposed `loadFolderBank`)

A future `loadFolderBank(dir, logger, …)` would mirror the existing
`loadLegacyBank` signature (`sample_store.h:23`) and, for each note folder:

1. **Enumerate** `<BankName>/m###/*.wav`.
2. For each WAV: build a `MicLayer` (head preload + optional resonance window),
   exactly as legacy does (`sample_store.cpp`), and **measure peak RMS** with the
   existing `measurePeakRmsDb` (`sample_loader.cpp`, sliding ~50 ms window, max →
   dBFS).
3. Wrap each into a `VelocitySlot` with `slot.rms_db = measured RMS`.
4. **Sort the note's slots ascending by `rms_db`** (softest → loudest) — the same
   `std::sort` legacy uses (`sample_store.cpp:152-158`).
5. The resulting slot count is **whatever the folder contained** — the loader
   does not require a fixed number, nor that counts match across notes.

The result is an ordinary `NoteSlots` whose `slots[]` is RMS-sorted. No velocity
information is needed on disk because **velocity → layer is resolved at play
time** (§5).

> The peak-RMS measurement, attack-end detection, FullyLoaded/Streamed decision,
> and resonance-window preload are all reused verbatim from the legacy loader —
> only the file discovery differs (per-note folder vs. flat directory).

---

## 5. Velocity → layer mapping (proportional, variable density)

At play time, an incoming MIDI velocity `v ∈ [1,127]` selects a layer. The
engine **already** does this for the variable-length, RMS-sorted slot list, in
`slotIndexForVelocity` (`patch_manager.cpp:18-25`):

```cpp
int slotIndexForVelocity(int velocity, int nslots) {
    if (nslots <= 1) return 0;
    float t   = (float)velocity / 127.f;            // 0..1
    int   idx = (int)((t * (nslots - 1)) + 0.5f);   // round to nearest layer
    return clamp(idx, 0, nslots - 1);
}
```

This is a **proportional** mapping: the velocity axis `0..127` is divided into
`nslots` bands of roughly equal width `≈ 127 / (nslots − 1)`, and the layer is
picked by rounding. Because `nslots` is **per note** (= file count in that
folder), the **velocity resolution varies across the keyboard** — exactly the
intended behaviour:

| Note folder | Files (layers) | Approx. band width | Effect |
|-------------|----------------|--------------------|--------|
| `m036` | 4 layers  | ~42 velocities | coarse dynamics |
| `m060` | 8 layers  | ~18 velocities | medium |
| `m072` | 12 layers | ~11 velocities | **dense** dynamics |

So a note recorded with more samples automatically gets finer velocity
resolution; "some ranges are denser" falls out of the per-note layer count with
no extra metadata.

### 5.1 Optional: non-uniform (curve-weighted) mapping

The baseline mapping above is **linear** in velocity. If denser resolution is
wanted in specific dynamic regions *within a note* (e.g. more layers spent
around `mf`–`f`), the mapping could be made non-linear by warping `t` through a
monotonic curve before indexing, e.g. `t' = pow(t, γ)`. This is an **optional
extension / open question** (§8) — the base proposal keeps the existing linear
`slotIndexForVelocity` and gets variable density purely from the per-note layer
count.

> Velocity *gain* shaping is separate and already perceptual-quadratic
> (`patch_manager.cpp:67-68`, `vel_gain = (v/127)²`); the mapping above only
> chooses *which sample* plays, not its gain.

---

## 6. Round-robin via peak-RMS tolerance clustering

When two samples of the **same note** have nearly equal peak RMS (within a small
dB tolerance), they represent the *same* dynamic captured as different takes —
i.e. **round-robin variants**, not distinct velocity layers. The new loader
**would cluster them into one `VelocitySlot` with multiple `variants[]`**.

### 6.1 Implementation status (verified)

- **Round-robin *selection* is implemented.** `selectVoice` already picks a
  random variant `≠` the last one played when a slot has more than one variant
  (`patch_manager.cpp:51-63`).
- **Round-robin *clustering* is NOT implemented.** The legacy loader stores
  **exactly one variant per slot** (`sample_store.cpp:137`; one `VelocitySlot`
  per WAV). Nothing groups near-equal-RMS samples. The data model documents this
  as future work (`sample_types.h:6-8` "round-robin … in later phases";
  `sample_types.h:69` "Legacy: 1").

So the playback path is **ready**; the only missing piece is populating
`variants[]` at load time. Once the folder loader clusters samples into a slot's
`variants[]`, round-robin works with **no playback-side change**.

### 6.2 Proposed clustering (and the fallback)

After measuring peak RMS for every WAV in a note folder and **sorting ascending**
(§4), walk the sorted list and group with an **anchor-bounded greedy** pass:

```
clusters = []
for sample s in rms_sorted:
    if clusters is empty OR (s.rms_db - clusters.last.anchor_rms_db) > TOL_DB:
        clusters.push(new cluster, anchor = s.rms_db)   # start new velocity layer
    clusters.last.add(s)                                 # round-robin variant
```

- Each **cluster → one `VelocitySlot`**; its members → `variants[]`.
- `slot.rms_db` = the cluster's representative RMS (anchor or mean).
- The slot count (= cluster count) feeds the velocity→layer mapping in §5.

**If round-robin is *not* implemented** (the current reality), the loader uses
the **same clustering** but keeps only the **first** sample of each cluster
(`variants = [first]`) and discards the near-duplicates — i.e. "for a given
velocity tolerance, just use the first hash". Either way the clustering pass is
new loader work; the only difference is whether extras become round-robin
variants or are dropped.

### 6.3 Tolerance — design notes

- **Unit: decibels, not linear.** RMS is already measured in dBFS
  (`measurePeakRmsDb`), and dB is logarithmic, so a *fixed dB* tolerance is
  perceptually uniform across the dynamic range — no need for a relative/linear
  threshold.
- **Default ≈ 1.5 dB.** Round-robin takes of one hit typically differ by well
  under ~1 dB, while a deliberate velocity *layer* step is usually larger. ~1.5 dB
  separates "same dynamic, different take" from "next layer". The exact value is
  tunable (open question §8).
- **Anchor-bounded, not single-linkage.** Grouping must compare each candidate to
  the **cluster's anchor**, not merely to its immediate predecessor. Single-linkage
  ("within TOL of the previous sample") would *chain* a smooth loudness ramp into
  one giant cluster. Anchor-bounding caps a cluster's span at ≤ `TOL_DB`, so a
  gradual ramp still produces several layers.
- **Order-stable & deterministic.** The pass runs over the RMS-sorted list, so
  the result is reproducible; round-robin *playback* order is randomized at run
  time (`lcgNext`, `patch_manager.cpp:60`), not baked into the bank.
- **Open edges:** anchor = cluster-start vs running mean; whether `TOL_DB` is a
  fixed constant, a per-bank value, or auto-derived from the loudness histogram
  (e.g. gaps between natural clusters). The base proposal: fixed `TOL_DB = 1.5`,
  anchor = cluster-start.

---

## 7. Autodetection (legacy vs. new)

A future `Engine::bankType()` / loader dispatch would pick the format by
inspecting the bank directory:

- If it contains **`m###/` subdirectories** → new folder format → `loadFolderBank`.
- Else if it contains flat **`m###-vel#-f##.wav`** files → `loadLegacyBank`.
- Else → empty / unrecognised bank.

This drives the GUI TYPE badge (`panel_bank.cpp`, currently hard-coded
`LEGACY`). Detection should be cheap (a single directory listing).

---

## 8. Migration & extensions

### 8.1 Converting a legacy bank

A legacy bank converts to the new layout without re-encoding audio:

```
for each  m<NNN>-vel<K>-f<SS>.wav  in  <LegacyBank>/:
    hash = first 12 hex of sha256(file)
    move/copy to  <NewBank>/m<NNN>/<hash>.wav
```

The `vel<K>` tag is simply dropped — it was advisory in legacy anyway
([legacy §5](bank-format-legacy.md#5-velocity-layering)). RMS ordering on load
reproduces the same layer order.

### 8.2 Optional metadata side-file (future)

If bank-level metadata is later needed (display name ≠ folder name, license,
default master trim, per-sample loop points or gain offsets, named mic layers),
an **optional** `bank.json` could be added *alongside* the folders without
changing the core layout — present → richer metadata, absent → everything
derived as above. This is intentionally **out of the base proposal** to keep v1
as simple as legacy.

### 8.3 Open questions for the implementer

- **Hash spec:** which algorithm / truncation length; is collision handling
  needed, or is per-folder uniqueness assumed?
- **Round-robin tolerance (§6):** the `TOL_DB` value (default ~1.5 dB), anchor =
  cluster-start vs running mean, and whether round-robin variants are kept or the
  first-only fallback is used (clustering pass is required either way).
- **Velocity curve (§5.1):** keep linear `slotIndexForVelocity`, or support a
  per-bank/per-note warp for denser bands?
- **Sample-rate consistency:** legacy reads SR per WAV header; should the folder
  loader enforce a single SR per bank, or allow mixed and resample?
- **`bankType()` placement:** new enum value vs. reuse of the existing
  `BankFormat::Extended` (`sample_store.cpp:32-37`, currently rejected).
- **Metadata side-file (§8.2):** needed for v1, or strictly deferred?

---

## 9. Comparison with legacy

| Aspect | Legacy (implemented) | New folder format (proposed) |
|--------|----------------------|------------------------------|
| Layout | Flat dir of WAVs | One `m###/` subfolder per note |
| Filename | `m###-vel#-f##.wav` (semantics encoded) | `<hash>.wav` (opaque) |
| Velocity in name | `vel#` token (advisory, ignored for ordering) | none |
| Layer count per note | = matching files (variable) | = files in folder (variable) |
| Layer ordering | Measured peak RMS (`velN` ignored) | Measured peak RMS (identical) |
| Velocity → layer | `slotIndexForVelocity` (proportional) | **same** `slotIndexForVelocity` |
| Round-robin | selection supported, but loader fills 1 variant/slot → never active | cluster within ~1.5 dB peak RMS → `variants[]` → round-robin active |
| Sample rate | WAV header (`f##` tag advisory) | WAV header (no tag) |
| Manifest | none | none (optional side-file later, §8.2) |
| In-memory model | `Bank`/`NoteSlots`/`VelocitySlot` | **identical** |
| Loader | `loadLegacyBank` (implemented) | `loadFolderBank` (proposed) |

The net change is: **a different on-disk layout and a new loader that fills the
same data model** — the velocity-layer derivation and playback selection are
already what the engine does today.
