# Plán a nedodělky (`plan2do.md`)

> Jedno místo pro všechno, co je **navržené, ale ještě nepostavené**, nebo
> vědomě **odložené**. Vzniklo konsolidací roztroušených „future / phase 6 /
> odloženo" poznámek z bývalých `bank-format-proposed.md`, `resampling.md`,
> `review-2026-06-10.md` a dokončených plánů ve `superpowers/` (ty už byly
> smazány — žijí v git historii).
>
> **Jak číst položku:** *Co* (cíl) → *Proč* → *Kde v kódu* (kotva, kam to
> zapadá) → *Stav / odhad zásahu*. Kotvy `soubor:řádek` jsou orientační (kód se
> mění); spolehlivější je název symbolu.

Legenda stavu: 🟢 model/základ hotový, chybí dotáhnout · 🟡 návrh existuje,
nepostaveno · 🔴 jen nápad / otevřená otázka · ⚪ vědomě odloženo (možná nikdy).

---

## Část A — Formát banky

### A1. Multi-mic mixing („fáze 6")

🟢 *Datový model hotový; chybí disk layout, párování v loaderu, mixer a GUI.*

**Co.** Banka může nést víc **mikrofonních pozic** (perspektiv) jedné a téže
nahrávky: jednu **main** pozici a volitelně další (`room`, `close`, `ambient`).
Když jich je víc, GUI je míchá fadery do výsledného zvuku.

**Proč.** Tohle je hlavní plánované rozšíření formátu. Vzniká přirozeně ze
stereo nahrávání víc pozicemi (např. blízká + vzdálená dvojice mikrofonů u
piana) — místo jedné statické perspektivy si hráč namixuje vlastní prostor.

**Kde v kódu (osa už existuje):**
- `SampleAsset.mics` je `std::vector<MicLayer>` (`engine/sample/sample_types.h:64`);
  `MicLayer.mic_name` má nést `"main"`/`"micpos-A"`/… (`sample_types.h:45`).
- Loader dnes plní **přesně jednu** pozici (`"stereo"`, `sample_store.cpp:60,133`).
- Přehrávání čte **jen `mics[0]`**: `voice.cpp:99` (`mic_ = &asset->mics[0]`),
  `resonance_engine.cpp:138`; `voice.h:5` to říká natvrdo:
  *„perspektivu (mics[0]); multi-mic mix je faze 6"*.

**Co chybí (4 kroky):**
1. **Disk layout.** Jedna pozice = WAVy přímo v `m###/<hash>.wav` (implicitní
   `main`). Víc pozic = podsložka na pozici:
   ```
   m060/
   ├── main/    ← řídí velocity vrstvení
   │   ├── 3f9a1c2b.wav
   │   └── a7e0445d.wav
   ├── room/
   └── close/
   ```
2. **Loader.** `main` řídí velocity vrstvení (RMS měření → řazení → mapování
   0–127, jako u jednomic). Každá další pozice nese **stejné takes**; načti je,
   seřaď podle peak RMS a **spáruj na main vrstvy podle RMS-seřazeného indexu**
   (i-tý nejhlasitější main ↔ i-tý nejhlasitější jiné pozice). Výsledek =
   další `MicLayer` v témž `SampleAsset.mics[]`, **main na indexu 0**.
3. **Přehrávání.** Hlas by sčítal aktivní pozice s gainy:
   `out = Σ gain_k · mics[k]`.
4. **GUI.** Mic-mix fadery **jen když má banka >1 pozici** (jeden fader na
   pozici, `main` vždy). Gainy persistovat do `state.json` jako ostatní parametry.

**Otevřená otázka.** Nesoulad počtu takes mezi pozicemi → nejspíš warn + použít
počet z main, přebytky ignorovat.

**Stav.** Banka s víc pozicemi je dnes **zpětně kompatibilní** — zní jako její
`main`, dokud mixování nepřistane.

### A2. Round-robin clustering

🟡 *Výběr variant hotový; shlukování near-equal-RMS takes není.*

**Co.** Víc takes téže noty+vrstvy se při opakovaném úhozu střídá (round-robin),
aby nevznikal „kulometný" efekt identického vzorku.

**Proč.** Realističtější repetice; dnes je sice **výběr** variant hotový
(`patch_manager.cpp:selectVoice`), ale **clustering** (rozpoznání, které takes
patří do téže vrstvy) implementovaný **není**.

**Kde v kódu.** `sample_types.h:6-8`, pozn. `sample_types.h:69` („Legacy: 1");
výběr `patch_manager.cpp:51-63`.

**Jak.** Shlukování podle tolerance peak RMS: `TOL_DB ≈ 1.5 dB`, kotva clusteru =
začátek clusteru vs. běžící průměr (k rozhodnutí). Clustering je potřeba tak jako
tak, i kdyby se varianty nakonec nestřídaly.

### A3. Nelineární velocity křivka

🔴 *Otevřená otázka — nechat lineární, nebo přidat warp?*

**Co.** Volitelné ne-uniformní (křivkou vážené) mapování velocity → vrstva,
hustší v určitém pásmu dynamiky. Dnes lineární `slotIndexForVelocity`
(`patch_manager.cpp:18-25`).

### A4. `bank.json` metadata side-file

⚪ *Záměrně mimo základní návrh.*

**Co.** Volitelný soubor vedle složek banky pro metadata, která se nedají
odvodit z názvů: display name ≠ název složky, licence, default master trim,
per-sample loop pointy / gain offsety, pojmenované mic vrstvy. Přítomen → bohatší
metadata; chybí → vše se odvodí jako dnes.

### A5. Komprese blobu (packed)

⚪ *Jediný neuzavřený bod z „odloženo do v2".*

**Co.** Komprimovat audio blob v `soundbank.ithaca` (zstd/FLAC). Zatím řeší
vnější komprese archivu; hlavička má rezervu pro rozšíření.

### A6. Asymetrický podpis (packed)

⚪ *Rezervováno, neimplementováno.*

**Co.** Podpis pakované banky (Ed25519), `flags` bit1 — dnes rezervovaný; banka
s bit1 je odmítnuta (`ithaca_bank.cpp:51-52`). Spolu s tím (v2.1+): per-záznam
MAC tagy, key rotation/versioning, machine binding.

### A7. Otevřené otázky k formátu

🔴

- **Hash spec:** algoritmus / délka zkrácení; řeší se kolize, nebo se
  předpokládá unikátnost v rámci složky?
- **`bankType()` placement:** nový enum hodnota vs. reuse `BankFormat::Extended`
  (`sample_store.cpp:32-37`, dnes odmítán).
- **Extended flat formát** (`m##-MIC-HASH.wav`) je dosud **odmítán** („faze 7",
  `sample_store.cpp:369-373` → prázdná banka). Buď doimplementovat, nebo formát
  oficiálně opustit.

---

## Část B — Sample rate / resampling

### B1. Kvalitní runtime SRC

🟡 *Větší zásah; zatím řešeno doporučením resamplovat offline.*

**Co.** Nahradit runtime `pos_inc` **lineární interpolaci** polyfázovým/sinc FIR
downsamplerem ve `Voice` i `ResonanceVoice`.

**Proč.** Dnešní cesta při 96→48 kHz (`pos_inc = 2.0`) je čistá decimace **bez
anti-aliasingu** → aliasing + lehce zakalené výšky. Kvalitní SRC by umožnil
provozovat 96 kHz banku **přímo**, bez offline kroku.

**Kde v kódu.** `voice.cpp:109-110` (`pos_inc_`), interpolace `voice.cpp:198-206`;
`resonance_voice.cpp:41-42`.

### B2. Varování na SR mismatch + vynucení jednotného SR

🔴 *Dnes mixed SR projde tiše.*

**Co.** Při loadu zalogovat, kolik samplů má `sample_rate ≠ engine SR` (upozorní
autora, že běží runtime konverze). Volitelně warn/reject při **mixu** různých SR
v jedné bance.

### B3. Transpoziční resampling (chybějící nota)

⚪ *Kód odložený mimo build.*

**Co.** Pitch-shift sousední noty, když přesná chybí. Kód je v
`engine/voice/_reserved_resampling.h` (`ithaca::reserved::nearestRecordedNote` /
`semitonePitchRatio`). Dnes se chybějící nota řeší tichem
(`patch_manager.cpp:39,66`, `pitch_ratio=1.0`).

---

## Část C — DSP / výkon

### C1. Partitioned-FFT convolver

🟡 *Jediná zbývající výkonová rezerva.*

**Co.** Uniformně-partitioned FFT konvoluce místo per-sample FIR. Convolver je
**jediná drahá stage** řetězce (zvlášť pro delší IR / ARM).
`engine/dsp/convolver.{h,cpp}`.

### C2. Zvuk convolveru / limiteru — odložené nálezy

🟡

- Energetická normalizace IR (`convolver.cpp:37-38`).
- Limiter **lookahead** (`limiter.cpp:22-29`).
- Stale-state po re-enable stage (`dsp_chain.h:20`).
- MIX smoothing / IR crossfade při přepnutí.
- WAV IR loader **existuje, ale není zapojen do dropdownu** (jen 2 modal IR);
  načítání IR z disku = future (`ir_wav`).

---

## Část D — Loader / robustnost

### D1. WAV format whitelist + robustnost

🟡

**Co.** Explicitně odmítat 8-bit / A-law / `WAVE_FORMAT_EXTENSIBLE`
(`wav_reader.cpp:40-85`); ošetřit degenerovaný WAV, výjimky v `scanBank`,
ignorované chyby `fwrite` ve `wav_writer`.

### D2. Stream / `requestRead` dluhy

🟡

- `StreamRequest::path` jako `std::string` se **kopíruje na audio vlákně** →
  přejít na `const char*` do Bank.
- Head fast-path pro `pos_inc==1 / frac==0` a env-větvení blokující
  autovektorizaci — otevřené.
- WAV chunk read = fopen+parse+fseek po každém požadavku → FUTURE: cache
  `FILE*`/`pread`.

---

## Část E — RT / multithreading (technické dluhy)

### E1. RT fallback na Linuxu

🟡 *Kód má jen `pthread_setschedparam`.*

**Co.** `rt_priority.cpp` má na Linuxu **jen** `pthread_setschedparam`; při
selhání rovnou `Failed`. Chybí **RTKit** integrace (desktop distra) i poslední
fallback `setpriority(PRIO_PROCESS, 0, -10)`.

**Pozn.** Bývalá `rt-thread-priority.md` tento fallback **popisovala jako by
existoval** (i v testovacím plánu) — v nové kapitole `reference/J-rt-priorita.md`
je to srovnáno na realitu a uvedeno zde jako budoucí práce.

### E2. Drobné RT dluhy

⚪

- `steady_clock::now()` na audio vlákně (formální RT porušení, benigní).
- `cfg_.release_ms` non-atomic (vědomě akceptováno).
- `activeMidiNotes` data race (UB, benigní).
- Blok 512 v `renderNotes` ignoruje `engine.blockSize()`; mrtvý parametr
  `engine_sr` v `processBlock` (lze zjednodušit).

---

## Část F — Testy / CI

### F1. TSan / stress v CI

🟡 Spustit ThreadSanitizer + zátěžový test v CI (build-tsan už existuje lokálně).

### F2. Mezery v testech (z review §4)

🟡 Voice stealing; pedál × pool integrace; rezonanční budget/steal/half-pedal;
chybové dráhy streamingu; malformed vstupy loaderu; chybové dráhy persistence;
regresní test convolver MIX early-out.

---

## Část G — K rozebrání: nálezy v KÓDU (ne v dokumentaci)

> Tyto věci **nejsou doc problém** — dokumentace je teď srovnaná na realitu kódu.
> Jsou to skutečné nesrovnalosti v repu, které jsme chtěli probrat zvlášť.

### G1. `config.json` v rootu nikdo nečte

⚠️ Soubor `config.json` (root) obsahuje `cache_budget_mb`, `max_voices`,
`stream_threads`, `render_threads`, … ale **žádný kód ho neotevírá**. `ithaca-cli`
staví `EngineConfig` inline (`app/cli/main.cpp`), nastavuje jen `block_size`,
`resonance_*`, `rt_priority` z CLI flagů; GUI čte jiný soubor (`state.json`).
→ Buď je `config.json` **mrtvý soubor** (smazat), nebo **chybí loader**, který ho
má číst. K rozhodnutí.

### G2. RT fallback chybí vs. původní doc

⚠️ Viz **E1** výše. Původní dokumentace slibovala chování (`setpriority(-10)`,
RTKit), které kód nemá. Dokumentaci jsem srovnal na realitu; **doplnění do kódu**
je otevřené rozhodnutí.
