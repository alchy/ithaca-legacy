# Pakovaná soundbanka v2 — zabezpečení (šifra + licence) — design

Datum: 2026-06-14 · Větev: `feat/packed-soundbank-v2-security` · Stav: schválený návrh (před implementací)

Navazuje na hotové v1 (`docs/bank-format-packed.md`, merge `4d1d20a`). v1 formát
nechal háčky: `flags` bity (bit0 encrypted, bit1 signed) + 256 B rezervu v hlavičce
+ integrity hashe. Tato v2 přidává **důvěrnost** (šifra blobu) + **vynucenou
dohledatelnost** (licenční soubor s identitou kupce, kryptograficky vázaný do banky).

Kanonický koncept je v `docs/bank-format-packed.md` §7; tento spec je detailní
návrh implementace.

---

## 1. Threat model (rozhodnuto)

**Odrazení + dohledatelnost úniku, NE tvrdé DRM.** Master tajemství je v binárce
(reverzním inženýrstvím získatelné), dešifrované audio je v RAM, kopie celé složky
(banka + licence) na jiný stroj funguje. Cíl není zabránit odhodlanému útočníkovi,
ale (a) znemožnit triviální zkopírování loóse WAVů a (b) **vázat identitu kupce do
banky tak, že ji nelze odstranit a banku dál používat** → únik je dohledatelný.

## 2. Rozhodnutí (schválená uživatelem 2026-06-14)

- **Bez Ed25519.** Integrita/„podpis" = symetrický **HMAC-SHA256** (model je
  symetrický, tajemství v appce; asymetrie by měla smysl jen pro ověření třetí
  stranou bez tajemství — nepotřebujeme).
- **Šifra = SHA-256 CTR keystream** (HMAC-SHA256 based) — **nula nových závislostí**
  (SHA-256 už máme). Dešifrují se jen čtené bajty (preload + streaming chunky),
  random-access; propustnost není úzké hrdlo.
- **MAC pokrývá index + license, NE blob** — ověřeno při každém načtení (malé
  sekce, milisekundy). Úprava blobu dá šum v audiu (pro útočníka k ničemu), proto
  se kryptograficky neověřuje (jako v1 `sha256_payload`).
- **Master tajemství** = jeden gitignored soubor, idempotentně generovaný buildem
  (viz §6).
- **`license.ithaca` = plaintext JSON** vedle banky; jeho bajty krmí KDF + MAC →
  needitovatelné.

## 3. Odvození klíčů (vše HMAC-SHA256)

```
master_secret : 32 B (build-time, viz §6)
license_bytes : přesné bajty souboru license.ithaca (jak na disku)

bank_key = HMAC-SHA256(master_secret, "ithaca-enc-v2" ‖ license_bytes)
mac_key  = HMAC-SHA256(master_secret, "ithaca-mac-v2" ‖ license_bytes)
```

Doménová separace prefixem zajistí, že `bank_key != mac_key`.

**Šifra blobu (CTR keystream):**
```
// p = pozice bajtu RELATIVNĚ k začátku blobu (p = absolutni_offset - blob_offset)
keystream_block(i) = HMAC-SHA256(bank_key, nonce ‖ u64_le(i))   // 32 B na blok i
cipher[p]          = plain[p] XOR keystream_block(p / 32)[p mod 32]
```
Random access: pro libovolný `p` spočítám blok `i = p/32` nezávisle. Čtení, které
nezačíná/nekončí na hranici 32 B, prostě použije správné `p` per bajt (handle si
spočítá bloky pro celý dotčený rozsah). `nonce` (32 B, náhodný per bake) je v
hlavičce. Klíčová vlastnost: keystream závisí jen na (`bank_key`, `nonce`, `p`),
takže streaming worker dešifruje libovolný seeknutý rozsah bez znalosti okolí.

**Integrita:**
```
hmac_tag = HMAC-SHA256(mac_key, metadata ‖ index ‖ names ‖ license_bytes)
```
Edit license → jiný `bank_key` i `mac_key` i `hmac_tag` → fail. Edit index/header →
`hmac_tag` fail. Identitu nelze odstranit.

## 4. Změny formátu (v2)

Žádná změna velikosti hlavičky (zůstává 408 B) ani indexu (64 B/záznam). Mění se
jen `flags` a využití 256 B rezervy (offset 152):

| offset | typ | pole |
|--------|-----|------|
| 12 | u32 | `flags` — **bit0 = 1** (encrypted+licensed); bit1 (signed) zůstává 0 |
| 152 | u8  | `cipher_id` — 1 = sha256-ctr-v2 |
| 153 | u8  | `reserved` (0) |
| 154 | 32 B| `nonce` (náhodný per bake) |
| 186 | 32 B| `hmac_tag` |
| 218 | 190 B| zbytek rezervy (0) |

- Šifruje se **jen blob** `[blob_offset, blob_offset+blob_size)`. Hlavička + metadata
  + index + names zůstávají **plaintext** (loader podle nich seekuje).
- Stávající `sha256_index` (offset 88) i `sha256_payload` (120) zůstávají beze
  změny (korupce-check, unkeyed). `hmac_tag` je navíc (tamper-check, keyed).
- `sha256_payload` se počítá nad **zašifrovaným** blobem (aby `bake --verify` ověřil
  zapsaná data na disku beze znalosti klíče).
- v1 banky (`flags bit0 == 0`) se načítají beze změny.

## 5. `license.ithaca` (plaintext JSON)

Vedle `soundbank.ithaca` ve stejném adresáři. Pole (návrh, upravitelný — vlastník
je vidí, ale nemůže editovat bez rozbití banky):

```json
{ "bank_name": "sp-customgrand", "owner_email": "jan@example.com",
  "owner_name": "Jan Novak", "transaction_id": "TX-2026-0042",
  "issued_at": "2026-06-14T18:30:00" }
```

Do KDF/MAC jdou **přesné bajty souboru** (ne reparsovaný JSON) — jakákoliv změna
(i whitespace) klíč rozbije. Python píše soubor deterministicky a tytéž bajty
použije při bake.

## 6. Master tajemství — generace a injekce (rozhodnuto)

**Jeden zdroj pravdy:** `secret/bank_secret.key` (32 náhodných bajtů), v `.gitignore`.

**Generátor `tools/gen-bank-secret.py`** (python3, cross-platform):
- Pokud `secret/bank_secret.key` **neexistuje** → vytvoří přes `secrets.token_bytes(32)`
  + vypíše **hlasité varování** ("VYGENEROVAN NOVY bank secret — zazalohuj; drive
  vydane licensed banky s nim nepujdou otevrit").
- Pokud **existuje** → no-op (NIKDY nepřepíše). Idempotentní.

**Spouštějí ho dvě cesty (idempotence → žádná dvojí generace):**
- **Makefile** — `configure`/`build` mají prerekvizitu `secret/bank_secret.key`
  (pravidlo volá generátor). `make <cokoliv>` na čistém klonu nejdřív zajistí secret.
- **CMake** — při configure `execute_process` zavolá tentýž generátor (funguje i
  přímý `cmake` build bez make).

**Injekce do C++:** CMake ze souboru vygeneruje `bank_secret_generated.h` (32 B jako
`constexpr uint8_t[32]`) do build adresáře → zkompiluje se do appky. Header je v
build dir (gitignored automaticky).

**Python bake** čte `secret/bank_secret.key` přímo (default cesta; `--secret-file`
override). Stejný soubor = stejný klíč jako v appce na tomtéž stroji.

**Produkční důsledek (dokumentovat):** secret je per-stroj náhodný → dev klony jsou
self-consistent (zabalíš licensed banku, tvůj build ji otevře). **Produkční secret
je dlouhožijící aktivum** — zálohovat; ztráta/přegenerování znehodnotí dříve vydané
licensed banky (nový build appky je neotevře). Dev build neotevře produkční licensed
banky (jiný secret) — správně.

## 7. Bake strana (`tools/bake_soundbank.py`)

Rozšíření, stdlib `hashlib`/`hmac`/`secrets` (žádná nová závislost; numpy už je):
- Nový režim **`--license`** — interaktivní prompt na pole (§5) NEBO `--license-json
  <path>` pro neinteraktivní. Bez `--license` = plaintext v1 (beze změny).
- Postup licensed bake:
  1. Sestaví `license.ithaca` (deterministický JSON) a zapíše ho vedle výstupu.
  2. Načte `master_secret` z `secret/bank_secret.key` (chybí → chyba s odkazem na
     `make`/generátor).
  3. Odvodí `bank_key`, `mac_key` (§3) z `license_bytes`.
  4. Vyrobí náhodný `nonce` (32 B).
  5. Při zápisu blobu **šifruje** každý WAV CTR keystreamem (offset-aware), počítá
     `sha256_payload` nad zašifrovanými daty.
  6. Spočítá `hmac_tag` (§3), zapíše `flags bit0`, `cipher_id`, `nonce`, `hmac_tag`.
- **`--verify`** licensed banky: ověří oba SHA-256, `hmac_tag`, a navíc **dešifruje**
  každý záznam a bit-exact porovná se zdrojovým WAVem.

Sdílená krypto helper modul `tools/ithaca_crypto.py` (KDF, keystream, MAC) — používá
ho bake i testy; zrcadlí C++ implementaci.

## 8. Read strana (C++)

**`engine/io/file_handle.h` — `DecryptingFileHandle`:** obaluje `IFileHandle`
(reálný `PosixFileHandle`). Drží `bank_key`, `nonce`, `blob_offset`, `blob_size`.
`readAt(off, buf, n)`:
- bajty mimo `[blob_offset, blob_offset+blob_size)` → pass-through (plaintext
  hlavička/index/names),
- bajty v blobu → přečte přes vnitřní handle, pak XOR keystreamem pro daný absolutní
  offset (counter = (off-blob_offset)/32, s korektním zarovnáním na hranice bloků).

Nic nad `readAt` se nemění — `readSampleRange`, preload, streaming worker, voice,
audio vlákno čtou plaintext. (Přímý zisk z v1 abstrakce.)

**`engine/sample/ithaca_bank.cpp` — `openIthacaBank`:**
- `flags bit0 == 0` → v1 plaintext cesta (beze změny).
- `flags bit0 == 1`:
  1. Načti `license.ithaca` z adresáře banky (chybí → `LicenseInvalid`).
  2. Odvoď `bank_key`, `mac_key` z kompilovaného `master_secret` + `license_bytes`.
  3. Ověř `hmac_tag` (§3). Neshoda → `LicenseInvalid`.
  4. Obal handle do `DecryptingFileHandle(bank_key, nonce, blob_offset, blob_size)`.
  5. Zbytek (parsování indexu, validace rozsahů) beze změny — čte přes (de)šifrující
     handle? NE: index/header jsou plaintext, čtou se před zabalením. Blob lokátory
     (`SampleFile.blob`) dostanou dešifrující handle.
- Dev/jiný secret → `hmac_tag` neshoda → `LicenseInvalid` (dev build neotevře cizí
  licensed banku).

**Výsledek loaderu:** nový distinct stav **`LicenseInvalid`** (vedle stávajícího
„prázdná banka"). Propaguje se do `BankLoadProgress`/výsledku reloadu.

## 9. Overlay UX (jediný zásah do playeru)

`LicenseInvalid` → existující modální load overlay:
- ukáže varování: **"Licence banky je neplatná nebo poškozená"** + krátký důvod
  (chybí licence / poškozená / nekompatibilní build),
- **podrží ~10 s** (čas na přečtení; konstanta, upravitelná),
- pak se zavře, banka zůstane **nenačtená** (stávající v1 chování při selhání),
  uživatel může vybrat jinou banku.

Mění se **jen** text + timed-hold v overlay renderu + propagace `LicenseInvalid`
stavu. Nic jiného v playeru (engine, voice, DSP, panely).

## 10. Chybové stavy

| stav | chování |
|------|---------|
| v1 plaintext banka | beze změny |
| licensed, vše OK | dešifruje, načte |
| chybí `license.ithaca` (u flags bit0) | `LicenseInvalid` → overlay |
| editovaná `license.ithaca` | `hmac_tag`/klíč fail → `LicenseInvalid` |
| editovaný index/header | `hmac_tag` fail → `LicenseInvalid` |
| editovaný blob | dešifruje na šum (audio); tag se needit (akceptováno) |
| dev/jiný build (jiný secret) | `hmac_tag` fail → `LicenseInvalid` |
| bake bez `secret/bank_secret.key` | chyba bake, žádný výstup |

## 11. Testy

- **Python (`tests/test_ithaca_crypto.py`, rozšíření `test_bake_soundbank.py`):**
  KDF/keystream/MAC proti pevným vektorům; zápis deterministického `license.ithaca`;
  licensed bake (flags/nonce/tag/cipher_id v hlavičce); round-trip dešifrování;
  `--verify` licensed.
- **C++ (`tests/test_decrypting_handle.cpp`, rozšíření `test_ithaca_bank.cpp`):**
  `DecryptingFileHandle` proti známému keystreamu (blob XOR, pass-through mimo blob);
  `openIthacaBank` licensed happy path s **test secretem**; tamper (edit license /
  edit index / chybí license → `LicenseInvalid`); plaintext v1 stále funguje.
- **Cross-language parita (silnější než v1 RMS — deterministická krypto):** python
  zašifruje s testovacím `ITHACA_BANK_SECRET`, C++ se stejným zkompilovaným test
  secretem dešifruje **bit-exact**. Realizace: C++ test (nebo round-trip skript)
  s pevným test secretem v build configu.
- **Round-trip skript** rozšířen o licensed variantu (bake licensed → engine
  `--inspect` načte → správný počet samplů; + tamper → `LicenseInvalid`).

## 12. Komponenty (hranice)

| jednotka | zodpovědnost | závisí na |
|----------|--------------|-----------|
| `tools/gen-bank-secret.py` | idempotentní generace secret souboru | python3 stdlib |
| `tools/ithaca_crypto.py` | KDF + keystream + HMAC (python, zrcadlí C++) | hashlib/hmac |
| `engine/util/ithaca_crypto.{h,cpp}` | KDF + keystream + HMAC (C++) | sha256 |
| `DecryptingFileHandle` (file_handle) | transparentní dešifrování blob rozsahu | IFileHandle, ithaca_crypto |
| `openIthacaBank` rozšíření | license load + key derive + tag verify + handle wrap | ithaca_bank, ithaca_crypto |
| bake `--license` rozšíření | license JSON + šifrování + tag | ithaca_crypto.py |
| CMake/Makefile secret injekce | generace + kompilace `bank_secret_generated.h` | gen-bank-secret.py |
| overlay `LicenseInvalid` | varování + 10s hold | app/gui (stávající overlay) |

## 13. Mimo rozsah (případné v2.1+)

- Ed25519 podpis (autenticita pro třetí strany) — model to nepotřebuje.
- Per-záznam MAC tagy (tamper konkrétního samplu) — blob tamper není hrozba.
- Komprese blobu, key rotation/versioning, machine binding.
