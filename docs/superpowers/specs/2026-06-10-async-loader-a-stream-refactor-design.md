# Async loader banky + StreamedSampleReader refactor — design

Datum: 2026-06-10 · Větev: `feat/async-loader-a-stream-refactor` · Stav: schváleno uživatelem

Navazuje na merge `fix/revize-2026-06-10` (revize `docs/review-2026-06-10.md`).
Dvě nezávislé části v jedné větvi; část A přistane první, část B následuje.
Každá část samostatně commitovaná a testovaná.

---

## Část A: Async reload banky + paralelní loader + progress

### Cíl

Výběr/reload banky dnes blokuje GUI vlákno na sekundy až desítky sekund
(synchronní `engine.reloadBank()` v render smyčce: disk load hlav + ~stovky MB
rezonanční cache). Cíl: GUI nikdy nezamrzne, uživatel vidí průběh, načítání je
3–6× rychlejší díky paralelnímu ingestu.

### Rozhodnutí (schválená uživatelem)

- **UX: modální overlay.** Přes celé okno ztmavený overlay s názvem banky,
  progress barem a popisem fáze; interakce blokované do dokončení. Engine je
  během loadu stejně tichý (bank_loading_), hrát nelze — modal je nejjednodušší
  a vylučuje souběžné akce (druhý reload, změna bufferu, layer slider).
- **Progress přes obě fáze.** Jeden bar 0–100 %: načtení hlav samplů (per
  soubor, váha ~60 %) + stavba rezonanční cache (per nota, váha ~40 %).
  Bez druhé fáze by bar skočil na 100 % a pak sekundy „visel".
- **Vlákno vlastní GUI** (ne engine): `AppContext` spustí `std::thread`, který
  volá stávající `engine.reloadBank()`. Engine API se mění jen o předání
  progress struktury. CLI async nepotřebuje (zůstává synchronní bez progress).

### Komponenty

**1. `BankLoadProgress`** (nová struktura, `engine/sample/sample_store.h`):

```cpp
struct BankLoadProgress {
    // phase: 0 = scan, 1 = heads (done/total = soubory),
    //        2 = rezonancni cache (done/total = noty), 3 = hotovo
    std::atomic<int> phase{0};
    std::atomic<int> done{0};
    std::atomic<int> total{0};
};
```

Plní ji `ithaca::loadBank(...)` a `buildResonanceCache(...)` — obě dostanou
nový volitelný parametr `BankLoadProgress* progress = nullptr` (nullptr =
chování beze změny; CLI a stávající testy se nemění). `Engine::loadBank` /
`reloadBank` dostanou tentýž volitelný parametr a předají ho dál. Engine sám
progress nedrží — vlastníkem je AppContext (žije déle než load thread).

**2. `AppContext` async reload** (`app/gui/app_context.{h,cpp}`):

- `void requestBankReload(const std::string& dir)` — guard
  `std::atomic<bool> reload_in_progress_` (druhé volání no-op); resetuje
  progress; spawne `reload_thread_` (member `std::thread`, join předchozího
  před spawnem a ve `shutdown()`).
- Worker volá `engine.reloadBank(dir, &load_progress_)`; po návratu zapíše
  výsledek (`reload_ok_`, atomic) a shodí `reload_in_progress_`.
- Po dokončení (detekuje GUI v render smyčce na hraně true→false):
  aplikace layer heuristiky („1/3 rozsahu banky" — přesun z `initFromState`,
  protože banka už při initu načtená není), zápis `state.bank_path`,
  log výsledku. Vše na GUI vlákně — žádné GUI akce z workeru.
- Volající místa: `panel_bank.cpp` (Selectable + RELOAD tlačítko) volají
  `ctx.requestBankReload(...)` místo `ctx.engine.reloadBank(...)`.

**3. Startovní load:** `initFromState` banku **nenačítá synchronně** — jen si
zapamatuje cestu; po vytvoření okna a první iteraci render smyčky GUI zavolá
`requestBankReload(state.bank_path)`. Okno se ukáže okamžitě, startovní load
jede stejnou cestou se stejným overlay. (Layer heuristika se tím přirozeně
přesouvá do completion handleru — viz výše; tím se mimochodem opraví i nález
H-střední z revize: heuristika dřív běžela bez rebuildu cache.)

**4. Overlay** (`app/gui/main.cpp` + malý helper v `widgets.h` nebo inline):
když `ctx.reloadInProgress()`, render smyčka po vykreslení pozadí přidá
fullscreen ImGui okno (ztmavení, název banky, progress bar 0–100 %, text fáze
„Načítám samply (123/704)…" / „Stavím rezonanční cache (45/88)…") a panely se
nerenderují interaktivně (skip input). ESC nic nedělá (load nelze přerušit —
zrušitelnost je YAGNI pro první iteraci).

**5. Paralelní ingest** (`engine/sample/sample_store.cpp`):

- Fáze heads: seznam souborů ze scanu se zpracuje worker poolem
  `clamp(hardware_concurrency()/2, 2, 8)` vláken; každý worker plní per-file
  výsledek (peek + readWavRange + RMS) do předalokovaného
  `std::vector<IngestResult>` na svém indexu — žádný sdílený zápis. Merge do
  `bank.notes[midi]` proběhne po joinu jednovláknově (zachová deterministické
  pořadí a RAM-budget kontrolu: budget se vyhodnocuje při merge; při překročení
  se zbylé výsledky zahodí — stejná sémantika ERROR + neúplná banka).
- Fáze cache: `buildResonanceCache` paralelizuje smyčku přes noty stejným
  poolem — každá nota čte jen svůj soubor a píše jen svůj slot (per-note
  nezávislé), atomic čítač `done`.
- Progress: `done.fetch_add(1)` po každém souboru/notě.

### Bezpečnost a chybové cesty

- Engine ochrany beze změny: `bank_loading_` + epoch handshake; reloadBank dál
  joinuje recache thread. Modal blokuje všechny GUI mutace enginu během loadu.
- Selhání loadu (neexistující/prázdná banka): worker skončí, GUI zaloguje
  Warning, overlay zmizí, engine zůstává s prázdnou bankou (stávající chování).
- Shutdown během loadu: `AppContext::shutdown()` joinuje `reload_thread_`
  PŘED zastavením audio/MIDI (load musí doběhnout — reloadBank drží
  bank_loading_ a nelze ho bezpečně přerušit; akceptované zpoždění zavření).

### Testy (část A)

- `loadBank`/`buildResonanceCache` s progress: total/done konzistentní,
  fáze monotónní, výsledná banka identická s během bez progress.
- Paralelní ingest: banka načtená paralelně == banka načtená sériově
  (stejné noty/sloty/rms/head obsah) na syntetické fixture ~20 souborů.
- RAM budget s paralelním ingestem: překročení → neúplná banka + ERROR
  (stávající sémantika).
- GUI logika (requestBankReload guard, completion handler) — netestovatelná
  bez ImGui; kompenzace: progress mapování 0–100 % jako čistá funkce s testem.

---

## Část B: StreamedSampleReader + levné výkonové výhry (bit-exact)

### Cíl

Streaming kód (lo/hi interpolační okno, seed švu, refill heuristika, underrun
stav) je ~80 řádků duplikovaných ve `Voice` a `ResonanceVoice` — při mitigacích
se každá oprava dělala dvakrát. Zároveň tři levné, měřitelné výkonové rezervy
pro RPi5. **Tvrdá podmínka (schválená): bit-exact** — refactor nesmí změnit
jediný vzorek výstupu.

### Komponenty

**1. `StreamedSampleReader`** (nový `engine/stream/streamed_reader.{h,cpp}`,
kompozice — žádné virtuály):

Zapouzdřuje: `RingHandle* ring_`, `file_request_off_`, `stream_pending_`,
lo/hi okno (`ring_lo_l/r_, ring_hi_l/r_, ring_lo_idx_`), seed švu
(poslední frame předchozího regionu → první ring pop), posun okna s detekcí
underrunu vs. čistého konce (`file_request_off_ >= total`), refill heuristiku
(prah, half-cap reset pendingu, no-advance-on-drop) a release/reset ringu.

Rozhraní (návrh, doladí plán):

```cpp
class StreamedSampleReader {
    bool  begin(StreamEngine* se, const SampleFile& file,
                int64_t start_frame, int64_t total_frames);  // acquire + 1. request
    void  seed(float lo_l, float lo_r, int64_t lo_idx);      // sev z head/preload
    // Posune okno na cilovy index; vraci Ok / Underrun / CleanEnd.
    Advance advance(int64_t target_idx);
    float loL() const; float loR() const; float hiL() const; float hiR() const;
    void  refill(StreamEngine* se);                          // per-block heuristika
    void  release(StreamEngine* se);                         // releaseRing + reset
};
```

`Voice` ho používá pro region za headem, `ResonanceVoice` pro region za
preload_resonance. Underrun fade (hold-last + 5ms rampa) zůstává v hlasech
(liší se obálkami), reader jen hlásí stav a drží poslední vzorky.

**2. Bulk čtení ringu** (uvnitř readeru): místo `popFrame` (3 atomické operace
na vzorek) reader na začátku bloku 1× snapshotne `w_` (acquire) do lokálního
`avail`, okno posouvá lokální aritmetikou nad `buf` a na konci bloku 1×
release-store `r_`. Stejné vzorky, stejné pořadí → bit-exact; ~6× méně
atomik na streamovaný hlas. `RingHandle` dostane k tomu úzké API
(`beginRead/commitRead` nebo ekvivalent), `popFrame` zůstává pro testy.

**3. `hasActiveMainVoice` O(1)** (`voice_pool.{h,cpp}`): per-nota čítač
`uint8_t note_active_count_[128]`. Increment ve `noteOn` po `v.start()`;
decrement při každém přechodu active→inactive: (a) kolem `v.process()` v
processBlock (`bool was = v.active(); ...; if (was && !v.active()) dec;`),
(b) v `prepareDamp` větvi noteOn (damp deaktivuje), (c) `reset()` nuluje celé
pole. `hasActiveMainVoice(n)` = `note_active_count_[n] > 0`. Sémantika
identická se skenem (čítač sleduje přesně `active_` přechody řízené poolem).

**4. Vyčlenění damp bufferů** (`voice.h`, `voice_pool.cpp`):
`float damp_buf_[2*kDampMaxFrames]` (16 kB) se přesune z Voice do jednoho
souvislého `std::vector<float> damp_pool_` ve VoicePool (pool_size × 2 ×
kDampMaxFrames, alokace v konstruktoru); Voice drží `float* damp_buf_`
nastavený poolem při konstrukci. `sizeof(Voice)` klesne z ~16,5 kB na ~stovky B
→ skeny poolu (processBlock, findSlot, activeCount, čítače) přestanou krokovat
přes cache po 16 kB. Bit-exact triviálně (jen umístění paměti).

### Verifikace bit-exact

Nový regresní test `tests/test_render_regression.cpp`:
1. Deterministická scéna bez reálných worker race: syntetická banka
   (FullyLoaded asset + Streamed asset s ringem předplněným deterministicky
   přes `RingHandle::push` v testu, workery nestartují).
2. Sekvence: noteOn (više not), sustain, retrigger (damp), noteOff, underrun
   (prázdný ring), čistý konec — render N bloků, výstup se sečte do FNV-1a
   hashe per blok.
3. **Krok 1 větve B:** test se napíše PŘED refactorem a očekávané hashe se
   zafixují z aktuálního (post-mitigace) kódu. Každý další commit části B musí
   hashe zachovat.

Plus celá stávající sada (33 binárek), ASan/UBSan a TSan buildy.

### Mimo rozsah (YAGNI)

Zrušitelnost loadu, progress v CLI, SoA layout Voice polí, partitioned-FFT
convolver, vektorizace hot loopu Voice (vyžaduje ne-bit-exact volnost),
semafor místo 1ms pollingu workerů.

---

## Pořadí implementace

1. Část A (async loader) — uživatelská hodnota přistane první.
2. Část B krok 1: bit-exact regresní fixture (před refactorem!).
3. Část B krok 2–4: reader, bulk, bitmask/čítače, damp pool — po jednom commitu.
4. Dokumentace: docs/reference/{C,D,E,F,H}.md aktualizovat souběžně se změnami
   (memory pravidlo „keep reference docs current").
