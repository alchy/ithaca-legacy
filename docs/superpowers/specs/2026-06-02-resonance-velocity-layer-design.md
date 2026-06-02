# Resonance velocity-layer selection + Gain + Config reorg — Design

**Goal:** Sympatická rezonance má brát vhodnější velocity vrstvu (ne natvrdo nejtišší `slots[0]`, která je u banky skoro neslyšitelná a šumí) a tu utlumit uživatelsky řízeným gainem. Zároveň přeskupit CONFIG panely a vyjasnit ovládání.

**Proč:** Nejtišší vrstva = špatný SNR. Rezonance startuje za attackem (`resonance_start_frame`), takže výběr hlasitější vrstvy nepřináší attack artefakty. Výběr vrstvy řídí uživatel cílovou hlasitostí (dB), útlum samostatným gainem (dB).

**Spec navazuje na:** partial-coincidence model (`harmonic_proximity`) a streaming rezonance (`ResonanceVoice`, `preload_resonance`).

---

## Rozhodnutí (z brainstormingu)

1. **RESONANCE (0..1 `strength`) se ruší**, nahradí ho `Resonance Gain` (dB, záporný): `excite = vel_norm × harm × 10^(gain_db/20)`. `vel_norm` (síla úhozu = energie buzení) zůstává.
2. **Výběr vrstvy** = slider `Resonance Layer` (cílové dB). Pro každou rezonující strunu N se vybere slot, jehož `rms_db` je **nejblíž** cíli. `variants[0]` (deterministicky). RMS slouží jen k výběru, žádná normalizace.
3. **Rozsah slideru `Resonance Layer`** je dynamický: po načtení banky se projedou všechny `slots[].rms_db`, min/max určí limity slideru. Engine si min/max cachuje při `loadBank`.
4. **Enable toggle** na RESONANCE stránce (`hasEnable=true`) — čisté zapnutí/vypnutí nezávislé na gainu.
5. **CONFIG pořadí:** MASTER | RESONANCE | AGC | BBE | LIMITER.
   - **MASTER** (nová): `MASTER` (gain dB, přesun z VOICE) + `RELEASE`.
   - **RESONANCE** (přejmenovaná VOICE): `RESONANCE LAYER` (dyn. rozsah) + `RESONANCE GAIN` + `EXCITE DECAY` + `MAX RESONANCE`; enable toggle.
6. **CLI:** `--resonance` přemapovat na gain dB; přidat `--resonance-layer` dB.
7. **Single-velocity banka:** slider zůstane aktivní (no-op; nearest = ten jediný slot).
8. **Defaulty:** `resonance_enabled=true`, `resonance_gain_db=-12`, `resonance_layer_db` = `bank_min + (bank_max−bank_min)/3` při prvním loadu (fallback −30 dB bez banky / bez persistované hodnoty), clamp do rozsahu banky.

---

## A) Engine — výběr vrstvy + gain + enable (`resonance_engine.{h,cpp}`)

`onPlayedNoteOn`:
- **Enable gate:** na začátku `if (!enabled_.load()) return;` (místo dnešního `if (str<=0) return;`).
- **Gain:** `const float gain_lin = gain_lin_.load();` `excite = vel_norm * harm * gain_lin;` (práh `kResonanceExciteMinGain` ponechán → velmi nízký gain ⇒ žádné spawny).
- **Výběr slotu** (nahrazuje natvrdo `ns.slots[0]`). Výběr vyčlenit jako **čistou funkci** (přímo unit-testovatelnou) — `engine/resonance/resonance_layer_select.{h,cpp}` (nebo free funkce v `resonance_engine`):
  ```cpp
  // Vrati index slotu, jehoz rms_db je nejbliz target_db. Pri shode nizsi index.
  // Prazdne slots → -1.
  int nearestSlotByRms(const NoteSlots& ns, float target_db);
  ```
  Použití v `onPlayedNoteOn`:
  ```cpp
  const NoteSlots& ns = bank.notes[N];
  const float target = layer_target_db_.load(std::memory_order_relaxed);
  const int si = nearestSlotByRms(ns, target);
  if (si < 0) continue;
  const VelocitySlot& vs = ns.slots[(size_t)si];
  if (vs.variants.empty() || vs.variants[0].mics.empty()) continue;
  const SampleAsset& a = vs.variants[0];
  const MicLayer*    m = &a.mics[0];
  ```
  Skip-attack zajišťuje `resonance_start_frame` zvoleného mic (platí pro `Streamed`; krátké `FullyLoaded` čtou `preload_head` od začátku — přijatelné, rezonančně relevantní samply jsou dlouhé). Žádný `slots[0]` fallback netřeba — zdroj existuje pro každý nahraný slot.
- **Stav:** nahradit `std::atomic<float> strength_{0.5f}` za `std::atomic<float> gain_lin_{…}` + `std::atomic<float> layer_target_db_{-30.f}` + `std::atomic<bool> enabled_{true}`.
- **Settery:** `setGainDb(float db)` (uloží `10^(db/20)`), `setLayerTargetDb(float db)`, `setEnabled(bool)`. (Nahrazuje `setStrength`/`strength`.)

## B) Engine façade (`engine.{h,cpp}`)
- `EngineConfig`: nahradit `float resonance_strength = 0.5f;` za:
  ```cpp
  bool  resonance_enabled   = true;
  float resonance_gain_db   = -12.f;
  float resonance_layer_db  = -30.f;   // fallback; GUI default = 1/3 rozsahu banky
  ```
- `init`: místo `resonance_->setStrength(cfg.resonance_strength)` volat `setGainDb/setLayerTargetDb/setEnabled` z cfg.
- Settery façady: `setResonanceGainDb(float)`, `setResonanceLayerDb(float)`, `setResonanceEnabled(bool)` (wrap ResonanceEngine). Odstranit `setResonanceStrength`.
- **Bank RMS range cache:** v `loadBank` po naplnění banky projet `bank.notes[*].slots[*].rms_db`, uložit `bank_peak_rms_min_db_` / `bank_peak_rms_max_db_` (členy). Když banka prázdná, nech default (−60/0). Gettery `float bankPeakRmsMinDb() const`, `float bankPeakRmsMaxDb() const`.

## C) CLI (`app/cli/main.cpp`)
- `--resonance <db>` → `cfg.resonance_gain_db` (clamp např. −60..0). Aktualizovat help text.
- Přidat `--resonance-layer <db>` → `cfg.resonance_layer_db`.
- (Volitelně `--no-resonance` → `cfg.resonance_enabled=false`. YAGNI — vynecháme, default enabled.)

## D) GUI config (`voice_page.h` → rozdělit; `app_context.cpp`; config panel)
- **`MasterPage`** (`name()="MASTER"`, `hasEnable=false`): 2 params — `MASTER` (master_gain_db, −60..6 dB) + `RELEASE` (release_ms, 50..2000).
- **`ResonancePage`** (`name()="RESONANCE"`, `hasEnable=true` → `enabled()`/`setEnabled()` mapují na `state.resonance_enabled` + `engine.setResonanceEnabled`): params — `RESONANCE LAYER` (resonance_layer_db, **dyn. min/max**) + `RESONANCE GAIN` (resonance_gain_db, např. −60..0) + `EXCITE DECAY` (excite_decay_ms) + `MAX RESONANCE` (max_resonance_voices).
  - **Dyn. rozsah:** `ResonancePage` drží `Param` jako **nestatické** členy; v `param(i)` (nebo lazy update v `get`) nastaví min/max RESONANCE LAYER z `ctx_.engine.bankPeakRmsMinDb()/MaxDb()`. Ověřit, že `renderParamPage`/`DecoSlider` čtou `param(i).min/max` živě každý frame (předpoklad; ověřit v implementaci). Persistovaný `resonance_layer_db` clampnout do rozsahu při změně (cheap, v `get`/`set`).
- **Pořadí stránek** v `main.cpp` (pole `IParamPage*`): `MasterPage, ResonancePage, AgcPage(0), BbePage(1), LimiterPage(2)` → indexy `config_page` 0..4. Config panel + `renderParamPage` beze změny mechaniky.
- `app_context.cpp` `initFromState`: `cfg.resonance_gain_db/_layer_db/_enabled` ze `state`; po initu zavolat settery (jako `setMaxResonanceVoices`). **Default layer při prvním loadu:** pokud `state.resonance_layer_db` chybělo (default z persistence) a banka načtená, nastavit `= min + (max−min)/3` a propsat do `state`.

## E) Persistence (`persistence.{h,cpp}`)
- `GuiState`: odstranit `float resonance_strength = 0.5f;`; přidat:
  ```cpp
  bool  resonance_enabled  = true;
  float resonance_gain_db  = -12.f;
  float resonance_layer_db = -30.f;
  ```
- `loadState`: odstranit `std::stof(... "resonance_strength")` (required read). Přidat **defensive** `readB("resonance_enabled", …)`, `readF("resonance_gain_db", …)`, `readF("resonance_layer_db", …)` (stará data → defaulty). Schema zůstává v4.
- `saveState`: odstranit řádek `resonance_strength`; přidat 3 nové klíče (pozor na čárky / poslední bez čárky).
- `main.cpp` debounce: nahradit `resonance_strength` ve `changed` za `resonance_enabled || resonance_gain_db || resonance_layer_db`.
- **RESET** (`panel_topbar.cpp`): místo `resonance_strength=0.5 / setResonanceStrength(0.5)` nastavit `resonance_enabled=true`, `resonance_gain_db=-12`, `resonance_layer_db` na 1/3 rozsahu banky (nebo −30 fallback) + příslušné settery.

## Testy
- **Nový** `test_resonance_layer_select` (doctest, `ithaca_core`): přímý test čisté funkce `nearestSlotByRms`. Postav `NoteSlots` s `rms_db` {−40,−25,−10}; ověř: target −25 → index 1; target −38 → 0; target −5 → 2; target přesně mezi → nižší index; prázdné slots → −1; 1 slot → 0 nezávisle na targetu.
- **Nový** test bank RMS range: po `loadBank` (malá fixtura) `bankPeakRmsMinDb() <= bankPeakRmsMaxDb()` a odpovídají min/max slotů.
- **Update** `test_resonance_engine.cpp`: `setStrength(1.f)` → `setEnabled(true)` + `setGainDb(0.f)`; ověřit excite math (×1.0).
- **Update** `test_engine_diagnostics.cpp`: `cfg.resonance_strength` → nové cfg pole; `setResonanceStrength` → `setResonanceGainDb` (+`setResonanceEnabled`).
- **Update** `test_persistence.cpp`: round-trip `resonance_strength` → nahradit novými poli.
- **Update** `test_resonance_stream_sr.cpp`: `cfg.resonance_strength=0.5` → `cfg.resonance_gain_db` (přepočet komentáře `excite = vel×harm×gain_lin`); přizpůsobit očekávané excite (volba layeru ovlivní, který sample, ne excite math — ověřit, že test nezávisí na slots[0] konkrétně, případně upravit fixturu).

## Edge cases
- Banka s 1 vrstvou: nearest = ten slot; slider no-op (aktivní). bank_min==max → degenerovaný rozsah slideru (zobrazí se, prakticky bez efektu) — akceptováno.
- Cíl mimo rozsah noty (nota má jen hlasité vrstvy): nearest = nejbližší krajní slot. Graceful.
- Disabled rezonance: `onPlayedNoteOn` early-return; existující hlasy přirozeně doznívají (decay), nezhasínáme je tvrdě.
- Nota bez `recorded`/prázdné slots/variants/mics: skip (jako dnes).
- `FullyLoaded` krátká vrstva zvolená pro rezonanci: hraje z `preload_head` (bez skip-attack) — přijatelné.

## Soubory
- **Create:** `engine/resonance/resonance_layer_select.{h,cpp}` (čistá fce `nearestSlotByRms`), `app/gui/master_page.h` + `app/gui/resonance_page.h` (z `voice_page.h`), `tests/test_resonance_layer_select.cpp`.
- **Modify:** `engine/resonance/resonance_engine.{h,cpp}`, `engine/engine.{h,cpp}`, `engine/CMakeLists.txt` (nový .cpp do `ithaca_core`), `app/cli/main.cpp`, `app/gui/main.cpp` (pole stránek + debounce), `app/gui/app_context.cpp`, `app/gui/persistence.{h,cpp}`, `app/gui/panel_topbar.cpp` (RESET), `tests/CMakeLists.txt` (+ nový test), `tests/{test_resonance_engine,test_engine_diagnostics,test_persistence,test_resonance_stream_sr}.cpp`.
- **Delete/replace:** `app/gui/voice_page.h` (rozdělen na master/resonance page).
- Reference docs (E-resonance, H-gui, A-core) průběžně aktualizovat.

## Mimo rozsah
- Per-nota RMS normalizace (zamítnuto — gain na surový vzorek).
- Round-robin variant v rezonanci (deterministicky variants[0]).
- Změna výběru za běhu noty (výběr jen při spawnu, jako dnes).
