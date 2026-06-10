# Pakovaná soundbanka `soundbank.ithaca` — design

Datum: 2026-06-10 · Větev: `feat/packed-soundbank` · Stav: schválený návrh (před implementací)

Motivace: komerční distribuce banky jako jediného souboru. Uživatel si dál může
nahrát vlastní adresářovou banku (fixní i dynamickou — beze změny); „verifikovaná"
banka se distribuuje jako `soundbank.ithaca` v adresáři banky. V1 řeší **packing
a abstrakci čtení**; šifra, podpis a licenční klíč jsou v2 — formát na ně nechává
místo (flagy + rezervovaný blok v hlavičce), takže se nebude lámat.

---

## Rozhodnutí (schválená uživatelem)

- **V1 bez šifry.** Jen jednosouborový formát + abstrakce čtení + bake nástroj.
  Hlavička má `flags` (encrypted/signed) a 256 B rezervu pro v2 (cipher id,
  fingerprint klíče, podpis, user-info blok s identitou kupce).
- **Předpočítaná analýza v indexu.** Per soubor: frames, sample rate, peak RMS dB,
  attack-end, offset PCM dat. Loader u .ithaca přeskočí analýzu i třídění —
  baked hodnoty jsou autoritativní.
- **Analýzu počítá C++ CLI, ne python.** Nový příkaz `--dump-bank-index` vypíše
  JSON; `bake_soundbank.py` hodnoty jen převezme. Jediný zdroj pravdy — žádné
  riziko divergence pořadí velocity vrstev mezi adresářovou a pakovanou bankou.
- **Varianta A: blob = doslovné WAV soubory.** Bake nezasahuje do audio dat
  (zřetězí + zarovná); abstrakce čtení je na úrovni bajtů. Round-trip je
  ověřitelný bit-exact. Překódování na kanonické PCM (varianta B) zamítnuto —
  nulový přínos, když index nese offset PCM dat; rozbalení do temp adresáře
  (varianta C) zamítnuto — popírá smysl jednoho souboru.
- **Vstup bake: jen dynamický formát** (per-nota podadresáře). Fixní banka se
  nejdřív převede (`tools/make_dynamic_bank.sh`).
- Vše v nové nezávislé větvi `feat/packed-soundbank`.

---

## 1. Formát souboru

Binární, little-endian. Čtyři sekce: hlavička → metadata → index (+ tabulka
jmen) → blob.

### Hlavička (pevných 408 B)

| offset | typ  | pole |
|--------|------|------|
| 0      | 8 B  | magic `"ITHACABK"` |
| 8      | u32  | `version` = 1 |
| 12     | u32  | `flags` — bit0 encrypted, bit1 signed (v1 vždy 0) |
| 16     | u64  | `metadata_offset` |
| 24     | u64  | `metadata_size` |
| 32     | u64  | `index_offset` |
| 40     | u64  | `index_size` |
| 48     | u64  | `names_offset` |
| 56     | u64  | `names_size` |
| 64     | u64  | `blob_offset` |
| 72     | u64  | `blob_size` |
| 80     | u32  | `entry_count` |
| 84     | u32  | rezerva (0) |
| 88     | 32 B | `sha256_index` — přes sekce metadata + index + names |
| 120    | 32 B | `sha256_payload` — přes blob |
| 152    | 256 B| rezerva pro v2 (0) |

`sha256_index` se ověřuje **při každém načtení** (sekce jsou malé).
`sha256_payload` se při načtení neověřuje (u multi-GB souboru by brzdil start) —
kontroluje ho jen `bake_soundbank.py --verify`.

### Metadata (UTF-8 JSON)

`bank_name`, `created_at`, `bake_tool_version`, `analysis_preload_ms`
(s jakým preload oknem byla počítána RMS/attack analýza), `source_format`
(`"dynamic"`). Jen informativní pole — loader z nich nic nederivuje.

### Index (pevných 64 B na záznam)

| offset | typ | pole |
|--------|-----|------|
| 0  | u16 | `midi` (0–127) |
| 2  | u16 | `channels` |
| 4  | u32 | `sample_rate` |
| 8  | u64 | `entry_offset` — absolutní offset WAV souboru v .ithaca |
| 16 | u64 | `entry_size` — délka WAV souboru v bajtech |
| 24 | u32 | `pcm_data_offset` — relativně k `entry_offset`; první bajt vzorků data chunku |
| 28 | u16 | `sample_format` — 1=PCM16, 2=PCM24, 3=float32 |
| 30 | u16 | rezerva (0) |
| 32 | i64 | `frames` |
| 40 | f32 | `rms_db` — peak RMS (autoritativní pro pořadí vrstev) |
| 44 | u32 | `attack_end` — frame index v rámci analyzovaného preload okna |
| 48 | u32 | `name_offset` — do tabulky jmen; `0xFFFFFFFF` = bez jména |
| 52 | 12 B| rezerva (0) |

Záznamy jsou **předřazené podle (midi, rms_db vzestupně)** — loader commituje
v pořadí indexu a `sortBankSlotsByRms()` se pro packed banku nevolá.

### Tabulka jmen

Length-prefixed UTF-8 řetězce (u16 délka + bajty): původní jméno souboru
(hash z dynamické banky). Jen pro debugging a `--verify` extrakci; loader ji
nečte.

### Blob

Doslovné WAV soubory za sebou, každý zarovnán na 4096 B (mezery vyplněné
nulami). Žádná transformace dat.

---

## 2. C++ čtecí vrstva

**`engine/io/file_handle.{h,cpp}`** — nová minimalistická abstrakce:

```cpp
struct IFileHandle {
    virtual ~IFileHandle() = default;
    virtual bool readAt(uint64_t off, void* buf, size_t n) const = 0;  // pread
    virtual uint64_t size() const = 0;
};
class PosixFileHandle : public IFileHandle { /* open()/pread(); Windows:
    CreateFile/ReadFile s OVERLAPPED offsetem */ };
```

`readAt` je bezstavový (žádný sdílený seek kurzor) → paralelní loader workery
i streaming worker čtou týž fd bez zámků. `Bank` vlastní jeden otevřený handle
(`std::shared_ptr<IFileHandle>`); lifetime je bezpečný — výměna banky probíhá
až po quiesce audia a hard-stopu hlasů (stávající reload handshake).

**`SampleFile` (sample_types.h)** dostane lokátor pro packed zdroj:
`shared_ptr<IFileHandle> blob` (null = běžný WAV soubor na `path`),
`uint64_t pcm_offset` (absolutní offset prvního vzorku v .ithaca),
`sample_format`, `channels`. Pole `path` u packed banky nese cestu
k .ithaca (pro logy).

**Dispatcher `readSampleRange(const SampleFile&, frame_off, frame_count)`**
(engine/io): pro běžný soubor deleguje na stávající `readWavRange(path, …)`;
pro packed spočítá bajtový rozsah (`pcm_offset + frame_off * channels *
bytes_per_sample`), provede `readAt` a dekóduje přes existující
`sampleToFloat`. Vrací `WavData` se stejnou sémantikou jako `readWavRange`
(EOF: `valid=true, frames=0`; clamp na konec vzorku; identické chování
převodu kanálů mono→stereo). Tři dotyková místa přejdou z
`readWavRange` na dispatcher: `prepareSampleFile()`, `buildResonanceCache()`,
stream worker (`stream_engine.cpp`). `peekWavInfo` packed banka nepotřebuje —
vše je v indexu.

---

## 3. Integrace do loaderu

- **`BankFormat::PackedIthaca`** (sample_types.h) + větev v `bankFormatName()`.
- **Detekce:** `scanBank()` dostane stupeň 0 — existuje-li
  `dir/soundbank.ithaca`, formát je `PackedIthaca` (priorita nade vším,
  ostatní obsah adresáře se ignoruje).
- **`engine/sample/ithaca_bank.{h,cpp}`** — parser + validátor: magic, verze,
  `sha256_index`, rozsahy sekcí, `entry_offset + entry_size <= blob konec`,
  midi ≤ 127. Vrátí seznam záznamů + otevřený handle.
- **Load větev v `loadBank()`:** pro packed se přeskočí directory scan, RMS
  analýza i třídění. Z indexu se rovnou plní kostra `Bank` (filtr
  `midi_from/to` beze změny); fáze „heads" čte preload okna paralelně přes
  dispatcher (rozhodnutí FullyLoaded vs. Streamed zůstává runtime — porovnání
  baked `frames` s aktuálním `preload_frames`); rezonanční cache, progress
  atomics, OOM guard — vše beze změny.
- **GUI:** dropdown bank listuje podadresáře `bank_search_dir`; adresář se
  `soundbank.ithaca` je dál obyčejný adresář, takže se objeví automaticky.
  Pokud panel kandidáty filtruje na „vypadá jako banka", přidá se podmínka
  „obsahuje soundbank.ithaca". Jinak beze změn.
- **Očekávání (změříme):** load packed banky rychlejší než adresářové — jeden
  fd místo stovek `fopen`, žádný directory scan, žádná analýza, sekvenčnější
  čtení hlav.

---

## 4. CLI: `--dump-bank-index`

Nový příkaz v `app/cli/main.cpp`: `--dump-bank-index <dir> [--preload-ms N]`
(default 150 = engine default). Provede stávající scan + per-soubor analýzu
(bez rezonanční cache) a vypíše JSON na stdout:

```json
{ "bank_format": "dynamic", "preload_ms": 150,
  "files": [ { "path": "m021/ab12….wav", "midi": 21, "frames": 480000,
               "sample_rate": 48000, "rms_db": -23.4, "attack_end": 1234 }, … ] }
```

Python z něj přebírá `midi`, `frames`, `sample_rate`, `rms_db`, `attack_end`.

---

## 5. Python nástroj: `tools/bake_soundbank.py`

Jen stdlib (`struct`, `hashlib`, `json`, `argparse`, `wave`-free vlastní lehký
RIFF parser). Argumenty:

- `--source-soundbank-dir <path>` — dynamická banka (jiný formát = chyba
  s odkazem na make_dynamic_bank.sh),
- `--destination-soundbank-dir <path>` — vznikne v něm `soundbank.ithaca`
  (existující soubor přepíše jen s `--force`),
- `--engine-cli <path>` — binárka pro `--dump-bank-index`,
- `--verify` — po zápisu přečte zpět, ověří oba SHA-256, rozbalí každý
  záznam a bit-exact porovná se zdrojovými WAVy.

Postup: validace zdroje → `--dump-bank-index` → vlastní RIFF parse každého
WAVu (`pcm_data_offset`, `sample_format`, `channels`; `frames`/`sample_rate`
křížově zkontroluje proti JSON z CLI — neshoda = chyba) → seřazení (midi,
rms) → zápis sekcí (hlavička se dopíše nakonec i s hashi) → volitelně verify.

---

## 6. Chybové stavy

- Špatný magic / nepodporovaná verze / nesouhlasící `sha256_index` / záznam
  ukazující za konec souboru / `entry_count == 0` → ERROR log + prázdná banka
  (stejné chování jako dnes u `Unknown` formátu). GUI ukáže prázdnou banku,
  aplikace nepadá.
- Useknutý blob při streamingu → stávající EOF/underrun cesta (reader vrátí
  `frames=0`, hlas fade-out).
- Bake: chybějící/nespustitelná CLI binárka, nedynamický zdroj, WAV neshoda
  CLI×python → chyba s jasnou hláškou, žádný výstupní soubor.

---

## 7. Testy (TDD)

- **Unit (C++):** parser hlavičky/indexu nad syntetickými bloby (validní,
  poškozený magic/hash/rozsahy — test helper pro zápis malých .ithaca);
  `readSampleRange` nad mini blobem (offsety, EOF, formáty 16/24/float32);
  detekce `PackedIthaca` ve `scanBank`.
- **Round-trip (skript):** fixture dynamická banka → bake → load z adresáře
  i z .ithaca → shodné pořadí slotů, frames, RMS, bit-exact preload data.
  Realizace: porovnání výstupů `--dump-bank-index`/`--inspect` nad oběma
  variantami + extrakce z `--verify`.
- **Perf:** orientační měření load času adresář vs. packed na reálné bance
  (log do PR/dokumentace).

---

## 8. Odloženo do v2

- **Šifra:** keystream s náhodným přístupem (AES-CTR nebo ChaCha20 — fakticky
  „rychlý XOR", ~GB/s, seek zadarmo; vhodný kandidát knihovny: monocypher,
  single-file C). Flag bit0 + rezerva v hlavičce.
- **Podpis** (Ed25519, veřejný klíč v aplikaci) — flag bit1.
- **Licenční klíčový soubor** vedle banky s identitou kupce (uuencoded
  user-info blok ~1204 B: email, číslo transakce) — dohledatelnost úniku.
- **Komprese blobu** (zstd/FLAC), pokud bude tlačit velikost downloadu —
  zatím řeší vnější komprese distribučního archivu.

## 9. Dokumentace (součást implementace)

Nový `docs/bank-format-packed.md` (popis formátu pro autory bank) +
aktualizace `docs/reference/F-loader.md` (detekce, packed load path) a
`docs/config-file.md` (zmínka u `bank_path`/`bank_search_dir`, že banka může
být `soundbank.ithaca`).
