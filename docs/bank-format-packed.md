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
> block for them; v1 requires `flags == 0`. Full design note: **§7 Security &
> encryption**.

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

- `flags` must be 0 — encryption and signing are v2 (see §7).
- The bake input must be a dynamic-velocity directory (convert flat banks first).
- Only the dynamic-velocity model is supported in the packed format.

## 7. Security & encryption (v2 — planned, NOT implemented)

This section is the canonical design note for the planned protection of licensed
banks. Nothing here is built yet; v1 ships plaintext (`flags == 0`). The format
was designed so v2 slots in **without breaking v1** (reserved `flags` bits + a
256-byte header block + the existing integrity hashes).

### 7.1 Threat model

**Deterrence + leak-tracing, not hard DRM.** The symmetric key ships *beside* the
bank, so a determined user can always extract the audio — that is accepted; no
client-side scheme prevents it. The value is twofold: (a) raise the bar so casual
copying of the loose WAVs isn't trivial, and (b) embed the buyer's identity in
the key so a leaked bank is traceable to who leaked it. Anything stronger (server
activation, machine binding) was explicitly rejected as bad UX for little gain.

### 7.2 What the format already reserves (v1)

- `flags` u32 (header offset 12): bit0 `kIthacaFlagEncrypted`, bit1
  `kIthacaFlagSigned` (constants in `engine/sample/ithaca_format.h`).
- 256-byte reserved block (header offset 152) — room for cipher id, key
  fingerprint, signature, nonce/salt, format sub-version.
- `openIthacaBank` currently **rejects `flags != 0`** (the v1 gate). v2 lifts this
  only once decrypt + verify exist, so an encrypted/signed file can never be
  misread as plaintext by a v1 binary.
- Integrity hashes `sha256_index` (sections) and `sha256_payload` (blob) already
  present — integrity is solved; v2 adds *confidentiality* + *authenticity*.

### 7.3 Building blocks

1. **Blob encryption — random-access stream cipher** (ChaCha20 or AES-CTR). The
   loader does `pread` at arbitrary offsets and streams; the cipher must decrypt
   any offset independently (counter derived from the absolute byte offset),
   ~GB/s, seek-for-free. **Only the blob is encrypted**; header + index + names
   stay plaintext (the loader needs offsets/sizes to seek) and are covered by
   `sha256_index`. Non-seekable modes (CBC) are unsuitable.
2. **Key file beside the bank** — in the same directory as `soundbank.ithaca`.
   Holds the symmetric key + a uuencoded ~1204-byte info block (JSON: buyer
   email, transaction id, …) for leak traceability. Each licensed bank is baked
   "made to measure" per buyer.
3. **Signature — Ed25519** over header + index, public key embedded in the app →
   authenticity (the bank is genuinely ours and untampered). `flags` bit1.

### 7.4 Where it slots into the code

The existing read abstraction makes this localized:
- **Read side:** decryption inserts into the blob branch of `readSampleRange`
  (decrypt only the bytes returned by `IFileHandle::readAt`, keyed by absolute
  offset — hence the random-access cipher). `openIthacaBank` loads the key file
  from the bank directory, verifies the Ed25519 signature with the embedded
  public key, then clears the `flags != 0` gate. No change to the loader/stream
  logic above the dispatcher.
- **Bake side:** `bake_soundbank.py` gains `--encrypt-key` / `--sign-key`;
  encrypts the blob, writes the key file beside the output, signs the header.
- **Library candidate:** [monocypher](https://monocypher.org) (single-file C:
  ChaCha20 + Ed25519, no OpenSSL dependency). Alternative: libsodium.

### 7.5 Open questions (decide before implementing)

- Exact key-file format (binary vs text; fields beyond the uuencoded block).
- Key derivation: raw symmetric key in the file, or derived from buyer id + a
  master secret held by the bake tooling.
- Per-bank unique key vs one shared key across a buyer's banks.
- Where the app's signing public key lives, and key-rotation strategy.
- Whether names/index also get encrypted (probably not — needed for seek).
- Nonce/IV placement (header reserve) and per-bake uniqueness.

### 7.6 Implementation path

1. Vendor the crypto lib (monocypher) under `third-party/`.
2. Bake: encrypt blob + write key file + sign header (`--encrypt-key`/`--sign-key`).
3. Read: key-file load, signature verify, per-offset decrypt in the blob path.
4. Lift the v1 `flags != 0` gate in `openIthacaBank`.
5. Tests: encrypted round-trip, tamper detection, wrong/missing-key handling.

> Before building v2, run a brainstorming pass on §7.5 — those choices shape the
> key-file format and the bake/read API.
