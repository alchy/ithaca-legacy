# RAM cache rezonance pro cílovou vrstvu (var. B) — Design

**Goal:** Sympatická rezonance má dohrát z RAM bez streamování (konec underrunů), cachováním delšího rezonančního okna (6 s) **jen pro per-notu cílovou velocity vrstvu** (tu, kterou vybírá `nearestSlotByRms(layer_target_db)`). Streaming rings zůstávají jako fallback. Při změně slideru „Resonance Layer" se cache na pozadí přebuduje; mezitím se streamuje.

**Proč:** Dnes se `preload_resonance` (500 ms) staví pro **všech 16** velocity vrstev (~258 MB), ale rezonance používá jen jednu → 15/16 mrtvé. Přesunem na 6 s × jen cílová vrstva: ~198 MB (**net −60 MB**) a rezonance plně z RAM. Rezonance je buzena slabě a tlumí se (excite tau ~5 s + damping) → 6 s pokryje drtivou většinu; zbytek streamuje.

**Navazuje na:** `nearestSlotByRms` (resonance velocity-layer selection), dual stream pool, `preload_resonance`/`resonance_start_frame` model (`ResonanceVoice`).

---

## Rozhodnutí (z brainstormingu)
- **Var. B:** cache jen per-notu cílová vrstva, okno **6 s** (fixní konstanta).
- **Swap při změně slideru = fade-out aktivních + per-notě ready guard** (ne double-buffer).
- **b2:** během rebuildu nové rezonance streamují z rings; rebuild běží na pozadí.
- Drop load-time 500 ms cache pro všechny vrstvy (nahrazena target-layer 6 s).
- Debounce slideru ~400 ms. Žádný nový config prvek.

## Architektura

**Per-notě ready flag (ne v MicLayer!).** `std::atomic<bool>` **nelze** dát do `MicLayer` — `VelocitySlot`/`SampleAsset`/`MicLayer` se při `sortBankSlotsByRms` movují. Ready flag proto žije ve stabilním poli v `ResonanceEngine`:
```cpp
std::array<std::atomic<bool>, 128> reso_cache_ready_{};  // per nota
```

**Loader rozdělen na dvě fáze:**
1. `loadSampleFile` (per slot, beze změny pořadí) — naplní `preload_head`, změří `rms_db`, spočítá `resonance_start_frame` (= attack_end) pro **všechny** Streamed mics, ale `preload_resonance` **NEplní** (zůstane prázdný).
2. **Post-sort pass** `buildResonanceCache(Bank&, float target_db, int window_ms)` (v `sample_store`, bez vazby na ResonanceEngine) — po `sortBankSlotsByRms`: pro každou notu N s nahranými sloty zjisti `si = nearestSlotByRms(slots, target_db)`; pro mic(y) slotu `si` načti `window_ms` (6000) od `resonance_start_frame` z disku (`readWavRange(mic.file.path, …)`) do `preload_resonance`, nastav `resonance_frames`; ostatní (dříve cílové) mics vyčisti (`preload_resonance.clear()`, `resonance_frames=0`). Vrátí `std::array<bool,128>` (true = nota má naplněnou cache). **Caller** (`Engine`) pak nastaví `reso_cache_ready_[N].store(result[N], release)`. Tím sample_store nezná atomic pole. Funkci sdílí `loadBank` i runtime rebuild.

**ResonanceVoice — cache vs stream mód (volba při startu):**
- `ResonanceEngine::onPlayedNoteOn` po výběru slotu přečte `reso_cache_ready_[N]` (acquire):
  - `true` → `slot->start(..., use_cache=true)`: hraj z `preload_resonance` pak ring (dnešní cesta).
  - `false` → `slot->start(..., use_cache=false)`: **stream mód** — ignoruj `preload_resonance`, streamuj z `resonance_start_frame` přes ring rovnou.
- `ResonanceVoice` dostane bool `use_cache_`; v `process()` při `!use_cache_` přeskočí preload větve a jde rovnou na ring (request od `resonance_start_frame`). Aktivní hlas drží svou volbu po celý život → realloc `preload_resonance` se ho nedotkne.

**Runtime rebuild (změna slideru), b2:**
1. GUI debounce ~400 ms po ustálení „Resonance Layer" → `Engine::rebuildResonanceCache(target_db)` (GUI vlákno):
   - `reso_cache_ready_[*].store(false, release)` (nové hlasy → stream mód),
   - nastav `recache_fade_request_` (atomic) — audio vlákno na začátku příštího `resonance_->processBlock` fadene všechny aktivní rezonanční hlasy (`fadeAllForRecache`) a request vynuluje,
   - spusť **background vlákno**.
2. Background vlákno: počká ~120 ms (doběh fade ramp), zavolá `buildResonanceCache(bank_, target_db, 6000)` (realloc/naplní nové cílové, vyčistí staré) a z návratu nastaví `reso_cache_ready_[N]` per nota (release).
3. Během kroků 1–2 nové rezonance streamují (`ready_=false`), aktivní dohasly fadem. Realloc je bezpečný — žádný hlas `preload_resonance` nečte (cache-mód hlasy faded; stream-mód hlasy ho ignorují).

**Lifetime:** `preload_resonance` cílového mic se realokuje až po doběhu fade aktivních cache-mód hlasů; nové hlasy do dokončení rebuildu jsou stream-mód (buffer nečtou). Žádný double-buffer ani shared_ptr.

## Engine API
- `Engine::rebuildResonanceCache(float target_db)` — orchestruje fade-request + background build. Volá GUI při debounced změně slideru. (Init build dělá `loadBank` přímo přes `buildResonanceCache`.)
- `ResonanceEngine`: `reso_cache_ready_` pole, `std::atomic<bool> recache_fade_request_`, `void fadeAllForRecache(float engine_sr)` (loop fadeOut přes aktivní), čtení `reso_cache_ready_[N]` v `onPlayedNoteOn`.
- `loadBank`: po sortu zavolá `buildResonanceCache(bank_, cfg_.resonance_layer_db, 6000)` a nastaví `reso_cache_ready_` z návratu.

## Threading
- Build čte z disku (`readWavRange`) ~88 not × 6 s ≈ ~100 MB → sub-sekunda na SSD, na background vlákně (ne GUI/audio).
- Audio vlákno: jen atomic load `reso_cache_ready_[N]` (acquire) + fade na fade-request. Žádné alokace v audio threadu.
- GUI vlákno: spustí background vlákno (detached/joinable; engine ho při shutdown joinne, pokud běží).

## Edge cases
- Nota bez nahrávky / prázdné sloty → `nearestSlotByRms` = -1 → bez cache, `ready_=false`, žádná rezonance (jako dnes).
- Rezonance přežije 6 s → po vyčerpání `preload_resonance` pokračuje ring (dnešní seam preload→ring). Funguje pro cache-mód.
- Rychlé opakované tahání slideru → debounce 400 ms sloučí; nový rebuild request během běžícího buildu: nech doběhnout aktuální, pak spusť poslední (coalescing — drž „pending target"; po dokončení, liší-li se, rebuild znovu).
- Reload banky během rebuildu: `reloadBank` má `bank_loading_` guard; rebuild vlákno musí být před `loadBank` joinnuto/zrušeno (engine to ošetří).
- CLI (`ithaca-cli`): `loadBank` postaví cache z `cfg_.resonance_layer_db`; CLI rebuild за běhu neřeší (žádný slider).

## Soubory
- **Modify:** `engine/sample/sample_store.{h,cpp}` (rozdělení loaderu + `buildResonanceCache`; `resonance_start_frame` pro všechny Streamed mics), `engine/voice/resonance_voice.{h,cpp}` (`use_cache_` flag + stream mód v `process`/`start`), `engine/resonance/resonance_engine.{h,cpp}` (`reso_cache_ready_`, `recache_fade_request_`, `fadeAllForRecache`, čtení ready v `onPlayedNoteOn`, předání `use_cache` do `start`), `engine/engine.{h,cpp}` (`rebuildResonanceCache` + background vlákno + join při shutdown; `loadBank` volá `buildResonanceCache`; konstanta okna 6000 ms), `app/gui/resonance_page.h` nebo `main.cpp` (debounce → `rebuildResonanceCache` při změně `resonance_layer_db`).
- **Reference docs:** E-resonance, F-loader, C-buffers (méně streamování), A-core (nová metoda) průběžně.

## Testy
- `buildResonanceCache` — postav malou banku (≥2 sloty s různým rms), target_db → ověř, že cílová vrstva má `preload_resonance` naplněný (`resonance_frames>0`) a ostatní prázdné; `reso_cache_ready_[N]==true`. Změna target → správný slot.
- `nearestSlotByRms` integrace (už pokryto).
- Stream-mód fallback: `ResonanceVoice` s `use_cache=false` produkuje samply z ringu (lze pokrýt rozšířením `test_resonance_stream_sr`).
- RT/rebuild race a fade-coordination = inherentně časové → ověřit smoke (sluchově: tahání slideru bez pádů/dlouhých výpadků; metr MAIN/RESO RINGS).

## Mimo rozsah
- Konfigurovatelná délka okna (fixních 6 s).
- int16 RAM cache (2× úspora) — možná future.
- Double-buffer/shared_ptr swap (zvolen fade+guard).
- Plný tail (>6 s) v RAM.
