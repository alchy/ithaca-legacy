# Ithaca Sample-Bank Format — PACKED (`soundbank.ithaca`)

> ## ✅ IMPLEMENTED (v1 packing + v2 encryption)
>
> A whole **dynamic-velocity** bank packed into a **single file**
> `soundbank.ithaca` for distribution. The user can still drop a plain
> directory bank (fixed- or dynamic-velocity) as before; a directory that
> contains `soundbank.ithaca` is loaded through this packed path instead. A bank
> is either **plaintext** (anyone can repack it) or **encrypted + licensed** (v2,
> buyer-bound) — same format, one header bit apart (see §3).
>
> - ✅ **Autodetection** — `scanBank` returns `BankFormat::PackedIthaca` when the
>   bank directory contains `soundbank.ithaca` (priority over everything else;
>   `bank_index.cpp`). The header `flags` then says plaintext vs. encrypted.
> - ✅ **Loading** — `loadBank` → `loadPackedBank` fills the bank skeleton
>   straight from the index (no directory scan, no RMS analysis, no sort — see
>   below); preload heads are read in parallel, streaming reads come from the
>   same blob (`sample_store.cpp`, `io/sample_read.cpp`). Encrypted banks decrypt
>   transparently under `readAt` (§4).
> - ✅ **Baking** — `tools/bake_soundbank.py` packs a dynamic-velocity directory
>   into `soundbank.ithaca` (analysis in numpy); `--license` makes it encrypted +
>   buyer-bound (§2).
> - ✅ **Integrity** — SHA-256 over the index sections (every load) and over the
>   blob (`bake --verify`); v2 adds blob encryption + an HMAC tamper tag.
>
> **v2 security (encryption + licence)** is implemented — see §7. Still future:
> asymmetric **signing** (`flags` bit1, reserved) and blob compression.

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

### Licensed (encrypted) bake — v2

To produce a protected, buyer-bound bank, add `--license` (interactive prompt for
owner fields) or `--license-json <path>` (non-interactive). The blob is encrypted
and a plaintext `license.ithaca` is written next to `soundbank.ithaca`:

```sh
python3 tools/bake_soundbank.py \
    --source-soundbank-dir      path/to/dynamic-bank \
    --destination-soundbank-dir path/to/output-dir \
    --license --verify
# → output-dir/soundbank.ithaca (encrypted) + output-dir/license.ithaca (plaintext)
```

- The encryption key is derived from the **build-time master secret**
  (`secret/bank_secret.key`, see §7.5) + the exact bytes of `license.ithaca`. The
  same secret must be compiled into the player that opens the bank (it is, when
  built from the same checkout). `--secret-file <path>` overrides the secret path.
- Editing `license.ithaca` (or the bank) changes the derived key / breaks the
  integrity tag → the engine refuses to load it and the GUI shows
  *"Soundbank is corrupted or license file is invalid. Sampler is unable to load
  the bank."* (click to continue). The buyer's identity therefore cannot be
  stripped while keeping the bank usable. Full design + threat model: §7.

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
| 152    | 1 B   | `cipher_id` — 0 = plaintext, 1 = sha256-ctr (v2) |
| 153    | 1 B   | reserved (0) |
| 154    | 32 B  | `nonce` — random per bake (v2); 0 when plaintext |
| 186    | 32 B  | `hmac_tag` — HMAC-SHA256 over metadata+index+names+license (v2); 0 when plaintext |
| 218    | 190 B | reserved (0) |

`sha256_index` is verified on **every load** (the sections are small).
`sha256_payload` is **not** checked on load (it would stall start-up on a
multi-GB file) — only `bake --verify` checks it.

**Plaintext vs. encrypted is one bit.** A v1/plaintext bank has `flags == 0`,
`cipher_id == 0`, zeroed `nonce`/`hmac_tag`, and **no** `license.ithaca`. An
encrypted (v2) bank has `flags` bit0 set, `cipher_id == 1`, a random `nonce`, a
real `hmac_tag`, and a companion `license.ithaca` (see below). The byte layout is
otherwise identical — the same loader reads both; only the blob bytes differ
(ciphertext vs. plaintext WAVs) and the v2 reserve fields are populated.

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

Plaintext bank: the verbatim WAV files concatenated, each aligned to 4096 B (gaps
zero-filled), no transformation. Encrypted bank: the **same** byte layout, but the
entire blob region `[blob_offset, blob_offset+blob_size)` (data + alignment
padding) is XORed with a SHA-256 CTR keystream — `keystream_block(i) =
HMAC-SHA256(bank_key, nonce ‖ u64le(i))`, block `i = blob_relative_pos / 32`. The
header/metadata/index/names stay plaintext so the loader can seek.

### `license.ithaca` (encrypted banks only)

A separate **plaintext UTF-8 JSON** file next to `soundbank.ithaca` in the same
directory. Holds the buyer identity, e.g.:

```json
{"bank_name":"sp-customgrand","issued_at":"2026-06-15T00:00:00",
 "owner_email":"buyer@example.com","owner_name":"Buyer","transaction_id":"TX-42"}
```

Its **exact bytes** feed key derivation and the integrity tag:
`bank_key = HMAC-SHA256(master_secret, "ithaca-enc-v2" ‖ license_bytes)`,
`mac_key = HMAC-SHA256(master_secret, "ithaca-mac-v2" ‖ license_bytes)`,
`hmac_tag = HMAC-SHA256(mac_key, metadata ‖ index ‖ names ‖ license_bytes)`. So
editing the license (or removing it) changes the key / breaks the tag and the
bank no longer opens — the buyer's identity is cryptographically bound and cannot
be stripped. The owner can read it (transparency) but not alter it. Distribute the
**whole directory** (`soundbank.ithaca` + `license.ithaca`); the `master_secret`
(§7.5) stays with the publisher and is never shipped.

## 4. How the engine loads it

`scanBank` checks for `soundbank.ithaca` first (stage 0) and returns
`PackedIthaca`. `loadBank` dispatches to `loadPackedBank`, which:

1. `openIthacaBank` opens the file via a positioned-read handle (`pread`, no
   shared cursor → parallel workers and the streaming worker share one fd
   lock-free) and validates: magic, version, `flags` (bit0 = encrypted is
   allowed; any other bit, e.g. the unimplemented signed bit1, is rejected),
   `sha256_index`, section ranges, and per-entry ranges (midi ≤ 127, known
   `sample_format`, `frames > 0`, `sample_rate` in range, PCM data within
   `entry_size`, entry within blob).
2. **If `flags` bit0 (encrypted):** reads `license.ithaca` from the same
   directory (missing → `LicenseInvalid`), derives `bank_key`/`mac_key` from the
   compiled `master_secret` + license bytes, verifies `hmac_tag` (mismatch →
   `LicenseInvalid`), and wraps the file handle in a `DecryptingFileHandle` that
   transparently decrypts the blob range under `readAt`. A `LicenseInvalid`
   result → the bank loads empty and the GUI shows *"Soundbank is corrupted or
   license file is invalid. Sampler is unable to load the bank."* (click to
   continue). For a **plaintext** bank (bit0 == 0) this whole step is skipped —
   no `license.ithaca` needed, no decryption.
3. Fills the bank skeleton straight from the index — **no** directory scan,
   **no** RMS analysis, **no** sort. Baked `rms_db`/`attack_end` are
   authoritative. The `midi_from`/`midi_to` filter applies as usual.
4. Reads preload heads in parallel through the read dispatcher
   (`readSampleRange`), which decodes the requested frame range straight from
   the blob (decrypting transparently for encrypted banks). FullyLoaded vs.
   Streamed stays a runtime decision (baked `frames` vs. the current `preload_ms`).
5. Streaming during playback reads the rest of each Streamed sample from the
   same blob handle (`StreamRequest` carries the `SampleFile` locator). Only the
   bytes actually read are decrypted (random-access keystream), off the audio
   thread — no RT impact.

## 5. Error handling

A bad magic, unsupported version, non-zero `flags`, mismatched `sha256_index`,
`entry_count == 0`, or any entry pointing outside the blob → an ERROR is logged
and the bank loads **empty** (same behaviour as an unrecognised directory bank);
the application does not crash. A truncated blob during streaming follows the
existing EOF/underrun path (the voice fades out).

`bake_soundbank.py` refuses up front (no output file) on: a non-existent or
non-dynamic source directory, an unsupported WAV format, a 0-frame/corrupt WAV,
or an existing output without `--force`.

## 6. Limitations

- The bake input must be a dynamic-velocity directory (convert flat banks first).
- Only the dynamic-velocity model is supported in the packed format.
- Encryption is implemented (v2, §7); asymmetric **signing** (`flags` bit1) is
  reserved but not implemented — a bank with bit1 set is rejected.

## 7. Security & encryption (v2 — IMPLEMENTED)

This section is the canonical design note for the protection of licensed banks.
**v2 is implemented** (branch `feat/packed-soundbank-v2-security`): blob
encryption (SHA-256 CTR keystream), a plaintext `license.ithaca` cryptographically
bound into the bank, and an `HMAC-SHA256` integrity tag. v1 plaintext banks
(`flags == 0`) load unchanged. Implementation detail spec:
`docs/superpowers/specs/2026-06-14-packed-soundbank-v2-security-design.md`. To
bake a licensed bank see §2 (the `--license` flow).

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
- `openIthacaBank` accepts `flags == 0` (plaintext) and bit0 (encrypted, v2);
  any other bit (e.g. the unimplemented signed bit1) is rejected. A pre-v2
  binary rejected `flags != 0` entirely, so it could never misread an encrypted
  file as plaintext.
- Integrity hashes `sha256_index` (sections, every load) and `sha256_payload`
  (blob, `--verify` only) detect corruption; v2 adds *confidentiality* (blob
  cipher) + keyed tamper detection (`hmac_tag`).

### 7.3 Building blocks (as built)

1. **Blob encryption — SHA-256 CTR keystream.** `keystream_block(i) =
   HMAC-SHA256(bank_key, nonce ‖ u64le(i))`, byte at blob-relative position `p`
   XORed with `keystream_block(p/32)[p%32]`. Random-access (any offset decrypts
   independently → streaming seeks decrypt only the bytes read), built entirely
   on the in-house `Sha256` — **no external crypto dependency**. Only the blob is
   encrypted; header/index/names stay plaintext (loader seeks by them).
2. **`license.ithaca` bound by key derivation.** Plaintext JSON beside the bank
   (see §3). Its exact bytes derive `bank_key`/`mac_key` and feed the MAC, so it
   cannot be edited without breaking decryption — that *is* the leak-tracing
   enforcement.
3. **Integrity — HMAC-SHA256** (`hmac_tag` in the header), keyed by `mac_key`,
   over metadata+index+names+license. Symmetric (no Ed25519): the model is
   symmetric (secret in the binary), so a keyed MAC — not asymmetric signing —
   is the right tamper check. (Signed `flags` bit1 is reserved, not implemented.)

> Note: an earlier design draft considered ChaCha20 + Ed25519 + a separate
> key-file. The shipped design is the simpler all-`Sha256` scheme above (chosen
> 2026-06-14): zero new dependencies, fast enough because only read bytes decrypt.

### 7.4 Where it lives in the code

- **`engine/util/ithaca_crypto.{h,cpp}`** — HMAC-SHA256, `deriveKey`, CTR
  `keystreamXor` (+ python mirror `tools/ithaca_crypto.py`, parity-tested).
- **`engine/io/file_handle.cpp`** — `DecryptingFileHandle` wraps the real handle;
  decrypts the blob range under `readAt`, passes header/index through unchanged.
  Everything above `readAt` (loader, streaming, voices) is untouched.
- **`engine/sample/ithaca_bank.cpp`** — `openIthacaBank`: for `flags` bit0, reads
  `license.ithaca`, derives keys from the compiled `kBankSecret`, verifies
  `hmac_tag`, wraps the handle. `LicenseInvalid` on any failure.
- **`tools/bake_soundbank.py`** — `--license` / `--license-json` encrypt the blob
  + write `license.ithaca` + fill the v2 header fields.

### 7.5 Master secret — generation, storage, build

- **What:** a 32-byte symmetric `master_secret`. `bank_key`/`mac_key` derive from
  it + `license.ithaca`. The publisher's bake tool and the shipped player must use
  the **same** secret (it's compiled into the player).
- **Where stored:** one file **`secret/bank_secret.key`** at the repo root.
  **Gitignored** (`.gitignore` lists `secret/` and `bank_secret_generated.h`) —
  the secret is **never committed**. The bake tool reads this file
  (`--secret-file`, default `secret/bank_secret.key`); CMake compiles it into
  `build/generated/bank_secret_generated.h` (`constexpr uint8_t kBankSecret[32]`,
  also gitignored) which the engine links.
- **Generated by the build, all targets:** `tools/gen-bank-secret.py` creates the
  file with 32 random bytes **only if it does not exist** (idempotent — never
  overwrites). It runs from **both** the Makefile (a `configure` prerequisite, so
  `make <any-target>` ensures it) **and** CMake configure (`execute_process`, so a
  direct `cmake` build works too). On a fresh clone the file is absent → the first
  build generates it and prints a loud warning to back it up. Cross-platform
  (python3 only). So on any machine, any build target works with zero manual key
  steps.
- **Production caveat:** the secret is random per machine. A dev clone is
  self-consistent (its bake + its build agree). The **production** secret is a
  long-lived asset: back up `secret/bank_secret.key`; losing or regenerating it
  makes previously-shipped licensed banks unopenable by new player builds. A dev
  build cannot open production licensed banks (different secret) — by design.

### 7.6 Accepted limits (threat model consequences)

Deterrence + traceability, not unbreakable DRM: the master secret is in the
player binary (reverse-engineerable), decrypted audio is in RAM, and copying the
whole bank directory (`soundbank.ithaca` + `license.ithaca`) to another machine
works. What's prevented: trivial copying of loose WAVs, and stripping the buyer
identity while keeping the bank usable. Bake of a multi-GB bank is CPU-heavy
(~4 min/6 GB — one HMAC per 32 B of keystream); decrypt-at-load touches only
preloaded/streamed bytes (large-bank full load adds ~1 s, one-time, off the audio
thread). A faster cipher (ChaCha20) would cut both but adds a dependency — not
done.
