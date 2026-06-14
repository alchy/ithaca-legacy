# Ithaca Sample-Bank Format — PACKED (`soundbank.ithaca`)

> ## ✅ IMPLEMENTED (v1 — packing only)
>
> A whole **dynamic-velocity** bank packed into a **single file**
> `soundbank.ithaca` for distribution. The user can still drop a plain
> directory bank (fixed- or dynamic-velocity) as before; a directory that
> contains `soundbank.ithaca` is loaded through this packed path instead.
>
> - ✅ **Autodetection** — `scanBank` returns `BankFormat::PackedIthaca` when the
>   bank directory contains `soundbank.ithaca` (priority over everything else;
>   `bank_index.cpp`).
> - ✅ **Loading** — `loadBank` → `loadPackedBank` fills the bank skeleton
>   straight from the index (no directory scan, no RMS analysis, no sort — see
>   below); preload heads are read in parallel, streaming reads come from the
>   same blob (`sample_store.cpp`, `io/sample_read.cpp`).
> - ✅ **Baking** — `tools/bake_soundbank.py` packs a dynamic-velocity directory
>   into `soundbank.ithaca` (analysis in numpy).
> - ✅ **Integrity** — SHA-256 over the index sections (verified on every load)
>   and over the blob (verified by `bake --verify`).
>
> **v2 (not yet built):** encryption, signing, a licence key file with buyer
> identity, blob compression. The header reserves `flags` bits and a 256-byte
> block for them; v1 requires `flags == 0`.

---

## 1. Why a single file

A "verified / licensed" bank ships as one `soundbank.ithaca` instead of hundreds
of loose WAVs. Benefits: one download artifact, one file descriptor at load time
(no per-file `fopen`), no directory scan, and pre-computed analysis baked into
the index so loading skips the per-sample RMS/attack measurement the directory
loader does. The audio data inside the blob is **verbatim WAV** — packing is
byte-exact and reversible (verified by `bake --verify`).

## 2. How to bake one

**Step 1 — Prerequisite: numpy.** The baker computes the per-sample analysis in
numpy. Install it once:

```sh
python3 -m pip install --user numpy
```

**Step 2 — Have a dynamic-velocity bank.** The input must be a *dynamic-velocity*
bank: per-note subfolders `m<NNN>/*.wav` (see
[dynamic-velocity reference](bank-format-proposed.md)). A fixed-velocity (flat)
`m###-vel#-f##.wav` bank must first be converted:

```sh
bash tools/make_dynamic_bank.sh path/to/flat-bank path/to/dynamic-bank
```

**Step 3 — Bake.** Run the packer; the output directory will receive a single
`soundbank.ithaca`:

```sh
python3 tools/bake_soundbank.py \
    --source-soundbank-dir      path/to/dynamic-bank \
    --destination-soundbank-dir path/to/output-dir \
    --verify
```

Flags:
- `--preload-ms N` — preload window (ms) the analysis is measured over; **must
  match** the engine's `preload_ms` (config.json) so the baked RMS/attack agree
  with directory loading (default 150 = engine default).
- `--verify` — after writing, read the file back, check both SHA-256 hashes, and
  bit-exact compare every entry against its source WAV. **Recommended for any
  bank you distribute.**
- `--force` — overwrite an existing `soundbank.ithaca`.

**Step 4 — Use it.** Put the output directory anywhere under the configured
`bank_search_dir` (config.json / GUI bank panel). A directory containing
`soundbank.ithaca` shows up in the bank dropdown like any other bank and loads
through the packed path automatically — no config change needed. Verify it loaded
with `ithaca-cli --inspect path/to/output-dir` (it should report
`Format: packed-ithaca`).

### Worked example

```sh
python3 -m pip install --user numpy
python3 tools/bake_soundbank.py \
    --source-soundbank-dir      ~/SoundBanks/Ithaca/sp-customgrand-dynamic \
    --destination-soundbank-dir ~/SoundBanks/Ithaca/packed-sp-customgrand-dynamic \
    --verify
# → Zapsano: .../packed-sp-customgrand-dynamic/soundbank.ithaca (~6.0 GB)
# → Verify OK (hash indexu, hash blobu, bit-exact extrakce)
./build/ithaca-cli --inspect ~/SoundBanks/Ithaca/packed-sp-customgrand-dynamic
# → Format: packed-ithaca   Celkem samplu: 1408
```

The baker computes `rms_db` and `attack_end` itself, replicating the engine's
algorithm (`engine/sample/sample_loader.cpp`): a 50 ms sliding RMS window (hop =
half window), mono mix `0.5*(L+R)`, `20*log10(max_rms)` floored at −120 dB,
measured only over the preload head. This keeps the velocity-layer ordering
identical to loading the same bank as a directory.

## 3. File layout

Little-endian. Four sections after the header: metadata → index → names → blob.

```
[ header 408 B ][ metadata JSON ][ index 64 B/entry ][ names ][ blob ]
```

### Header (fixed 408 B)

| offset | type  | field |
|--------|-------|-------|
| 0      | 8 B   | magic `"ITHACABK"` |
| 8      | u32   | `version` = 1 |
| 12     | u32   | `flags` — bit0 encrypted, bit1 signed (v1: must be 0) |
| 16     | u64   | `metadata_offset` |
| 24     | u64   | `metadata_size` |
| 32     | u64   | `index_offset` |
| 40     | u64   | `index_size` |
| 48     | u64   | `names_offset` |
| 56     | u64   | `names_size` |
| 64     | u64   | `blob_offset` |
| 72     | u64   | `blob_size` |
| 80     | u32   | `entry_count` |
| 84     | u32   | reserved (0) |
| 88     | 32 B  | `sha256_index` — over metadata + index + names |
| 120    | 32 B  | `sha256_payload` — over blob |
| 152    | 256 B | reserved for v2 (0) |

`sha256_index` is verified on **every load** (the sections are small).
`sha256_payload` is **not** checked on load (it would stall start-up on a
multi-GB file) — only `bake --verify` checks it.

### Metadata (UTF-8 JSON)

Informational only — the loader derives nothing from it: `bank_name`,
`created_at`, `bake_tool_version`, `analysis_preload_ms` (the window the baked
RMS/attack were measured with), `source_format` (`"dynamic"`).

### Index (fixed 64 B per entry)

| offset | type | field |
|--------|------|-------|
| 0  | u16 | `midi` (0–127) |
| 2  | u16 | `channels` (1 or 2) |
| 4  | u32 | `sample_rate` |
| 8  | u64 | `entry_offset` — absolute offset of the WAV file inside `soundbank.ithaca` |
| 16 | u64 | `entry_size` — length of the WAV file in bytes |
| 24 | u32 | `pcm_data_offset` — relative to `entry_offset`; first sample byte of the data chunk |
| 28 | u16 | `sample_format` — 1 = PCM16, 2 = PCM24, 3 = float32, 4 = PCM32 |
| 30 | u16 | reserved (0) |
| 32 | i64 | `frames` |
| 40 | f32 | `rms_db` — peak RMS (authoritative for layer order) |
| 44 | u32 | `attack_end` — frame index within the analyzed preload window |
| 48 | u32 | `name_offset` — into the names table; `0xFFFFFFFF` = no name |
| 52 | 12 B | reserved (0) |

Entries are **pre-ordered by `(midi, rms_db ascending)`** — the loader commits in
index order and never re-sorts, so velocity layers come out lowest-RMS first.

### Names table

Length-prefixed UTF-8 strings (`u16` length + bytes): the original source file
name. For debugging / `--verify` extraction only; the loader does not read it.

### Blob

The verbatim WAV files concatenated, each aligned to 4096 B (gaps zero-filled).
No transformation of the audio data.

## 4. How the engine loads it

`scanBank` checks for `soundbank.ithaca` first (stage 0) and returns
`PackedIthaca`. `loadBank` dispatches to `loadPackedBank`, which:

1. `openIthacaBank` opens the file via a positioned-read handle (`pread`, no
   shared cursor → parallel workers and the streaming worker share one fd
   lock-free) and validates: magic, version, `flags == 0`, `sha256_index`,
   section ranges, and per-entry ranges (midi ≤ 127, known `sample_format`,
   `frames > 0`, `sample_rate` in range, PCM data within `entry_size`, entry
   within blob).
2. Fills the bank skeleton straight from the index — **no** directory scan,
   **no** RMS analysis, **no** sort. Baked `rms_db`/`attack_end` are
   authoritative. The `midi_from`/`midi_to` filter applies as usual.
3. Reads preload heads in parallel through the read dispatcher
   (`readSampleRange`), which decodes the requested frame range straight from
   the blob. FullyLoaded vs. Streamed stays a runtime decision (baked `frames`
   vs. the current `preload_ms`).
4. Streaming during playback reads the rest of each Streamed sample from the
   same blob handle (`StreamRequest` carries the `SampleFile` locator).

## 5. Error handling

A bad magic, unsupported version, non-zero `flags`, mismatched `sha256_index`,
`entry_count == 0`, or any entry pointing outside the blob → an ERROR is logged
and the bank loads **empty** (same behaviour as an unrecognised directory bank);
the application does not crash. A truncated blob during streaming follows the
existing EOF/underrun path (the voice fades out).

`bake_soundbank.py` refuses up front (no output file) on: a non-existent or
non-dynamic source directory, an unsupported WAV format, a 0-frame/corrupt WAV,
or an existing output without `--force`.

## 6. Limitations (v1)

- `flags` must be 0 — encryption and signing are v2.
- The bake input must be a dynamic-velocity directory (convert flat banks first).
- Only the dynamic-velocity model is supported in the packed format.
