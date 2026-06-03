# DSP

Post-mix DSP řetězec zpracovává stereo buffer *po* aplikaci master gainu uvnitř
`Engine::processBlock()` (krok 5 z 6). Řetězec tvoří tři stupně pevného pořadí:
**AGC → ENHANCER → Limiter**; každý stupeň je instancí abstraktní třídy `DspStage` a
implementuje i GUI-facing rozhraní `IParamPage`, takže jej GUI může přímo ovládat
generickým panelem parametrů. Parametry každého stupně jsou uloženy v `std::atomic`
proměnných — GUI vlákno (nebo libovolný off-RT kód) je zapisuje bez zámku, audio
vlákno je čte v `process()`. Přepočet odvozených koeficientů (biquad, decay, lin
threshold) se provádí přímo v `process()` lazy-copy vzorem: interní `last_*`
sentinel se porovná s atomicky přečtenou hodnotou, a teprve při změně se koeficient
přepočítá. Disabled stupeň je celý přeskočen (`DspChain::process()` volá
`s->process()` jen když `s->enabled()` vrátí `true`).

---

## Implementováno v souborech

| Soubor | Odpovědnost | Klíčové typy |
|--------|-------------|--------------|
| `engine/dsp/dsp_stage.h` | Generické rozhraní parametrické stránky (GUI) a DSP stupně (audio) | `Param`, `IParamPage`, `DspStage` |
| `engine/dsp/dsp_math.h` | Stateless DSP primitiva: dB/lin konverze, biquad (DF-II), RBJ shelving, envelope smoothing | `BiquadCoeffs`, `BiquadState` + volné inline funkce |
| `engine/dsp/dsp_chain.h` | Kontejner tří pevně zapojených stupňů; iteruje jen enabled | `DspChain` |
| `engine/dsp/dsp_chain.cpp` | Kotva překladové jednotky, záměrně prázdná | — |
| `engine/dsp/agc.h` | Deklarace RMS AGC stupně | `AGC` |
| `engine/dsp/agc.cpp` | Implementace `process()` a `applyParams_()` pro AGC | — |
| `engine/dsp/enhancer.h` | Deklarace Enhancer (ex-BBE) stupně | `Enhancer` |
| `engine/dsp/enhancer.cpp` | Implementace `process()`/`computeCoeffs_()` pro Enhancer (3-band+dynamic+exciter+all-pass) | — |
| `engine/dsp/limiter.h` | Deklarace stereo peak limiteru | `Limiter` |
| `engine/dsp/limiter.cpp` | Implementace `process()` a `applyParams_()` pro Limiter | — |

---

## Soubory — detailní popis

### `engine/dsp/dsp_stage.h`

Definuje tři typy tvořící generický základ celého DSP systému.

#### `struct Param`

Deskriptor jednoho parametru pro GUI vykreslení a persistenci. Všechna pole jsou
compile-time konstanty (typicky `static const Param kParams[]` ve třídě stage).

| Pole | Typ | Popis |
|------|-----|-------|
| `id` | `const char*` | Stabilní klíč pro persistenci (JSON), např. `"threshold_db"` |
| `label` | `const char*` | Popisek v UI, velká písmena, např. `"THRESHOLD"` |
| `min`, `max`, `def` | `float` | Rozsah a výchozí hodnota; `set()` klampuje do `[min,max]` |
| `fmt` | `const char*` | Printf formát pro `DecoSlider`, např. `"%.1f dB"` |
| `readonly` | `bool` | Pokud `true`, GUI slider nezobrazí editaci (zatím nevyužito) |

#### `struct IParamPage`

GUI-facing rozhraní. Implementují ho `DspStage` (AGC, ENHANCER, Limiter) i param stránky
(VOICE stránka v GUI). Volá ho generický renderer `renderParamPage()` a config panel
`renderConfigPanel()`.

| Metoda (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Popis |
|--------------------|--------|----------------|---------|-------------|-------|
| `name() → const char*` | libovolné | — → název stage | GUI panely | — | Název stage pro záhlaví stránky (`"AGC"`, `"ENHANCER"`, `"LIMITER"`) |
| `paramCount() → int` | libovolné | — → počet parametrů | `renderParamPage` | — | Počet prvků v `kParams[]` |
| `param(i) → const Param&` | libovolné | index → deskriptor | `renderParamPage` | — | Vrací konstantní deskriptor i-tého parametru |
| `get(i) → float` | libovolné | index → aktuální hodnota | `renderParamPage`, persistence | `atomic::load(relaxed)` | Atomické čtení uložené hodnoty parametru |
| `set(i, v)` | GUI | index + nová hodnota | `renderParamPage`, persistence, `app_context.cpp` | `atomic::store(relaxed)` po clamp | Klampuje `v` do `[min,max]`, uloží atomicky; audio vlákno přečte příště v `process()` |
| `hasEnable() → bool` | libovolné | — → bool | `renderConfigPanel` | — | DSP stage: `true`; VoicePage: `false` |
| `enabled() → bool` | libovolné | — → bool | `DspChain::process`, `renderConfigPanel` | `atomic::load(relaxed)` | Vrací stav enable flagu |
| `setEnabled(on)` | GUI | bool | `renderConfigPanel`, `app_context.cpp` | `atomic::store(relaxed)` | Přepíná bypass stage bez zámku |
| `meter(value&, label&) → bool` | GUI | — → (hodnota, popis) nebo false | diagnostický panel | konkrétní `atomic::load` | AGC vrací aktuální gain (lineární), Limiter vrací gain reduction (dB), Enhancer vrací `false` |

#### `struct DspStage : IParamPage`

Přidává tři audio-thread metody k `IParamPage`. Implementují je AGC, Enhancer, Limiter.

| Metoda (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Popis |
|--------------------|--------|----------------|---------|-------------|-------|
| `prepare(sr, max_block)` | off-RT / init | sample rate, max block velikost | `DspChain::prepare`, `Engine::prepare` | `applyParams_(true)`, `reset()` | Inicializuje odvozené koeficienty při start nebo device změně; volá `reset()` pro čistý stav |
| `reset()` | off-RT / init | — | `DspChain::reset` | — | Vymaže audio-thread stav (aktuální gain, biquad delay linka) |
| `process(L, R, n)` | audio | in-place stereo buffer, délka `n` | `DspChain::process` | `applyParams_(false)`, biquad/gain rutiny | Zpracuje jeden blok; nejprve lazy přepočet koeficientů, pak DSP smyčka |

---

### `engine/dsp/dsp_math.h`

Stateless, inline, RT-safe primitiva. Nedrží žádný stav — veškerý stav předává
volající.

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|--------------------|--------|----------------|---------|-------------|-----------|------------|
| `db_to_lin(db) → float` | libovolné | dB → lineární amplituda | `Limiter::applyParams_` | `std::pow` | `db`: hodnota v dB (0 dB → 1.0) | Standardní konverze: `10^(db/20)` |
| `lin_to_db(lin) → float` | libovolné | lineární amplituda → dB | `Limiter::process` (pro GR metr) | `std::log10`, `std::max` | `lin`: amplituda; clamp na `1e-9f` aby log nehavaroval | Standardní konverze: `20·log₁₀(lin)` |
| `decay_coeff(tau_s, sr) → float` | libovolné / off-RT | čas zotavení [s], sample rate → koeficient | `Limiter::prepare`, `Limiter::applyParams_` | `std::exp` | `tau_s`: časová konstanta; `sr`: vzorkovací kmitočet; tau·sr clamp na ≥1 | Multiplikativní decay koeficient `exp(-1/(tau_s·sr))` pro IIR 1-pólový filtr; Limiter jej používá pro attack (fixní 1 ms) i release |
| `biquad_tick(x, c, s) → float` | audio | vzorek + koeficienty + stav → výstup | `Enhancer::process` | — | `x`: vstupní vzorek; `c`: `BiquadCoeffs`; `s`: `BiquadState` (mutable) | DF-II transponovaná forma; aktualizuje `s.s1`, `s.s2` in-place; jedna iterace je 5 MAC |
| `rbj_high_shelf(fc, gain_db, sr) → BiquadCoeffs` | off-RT / lazy v process | střed [Hz], boost/cut [dB], sr → koeficienty | `Enhancer::computeCoeffs_` | `std::pow`, `std::cos`, `std::sin`, `std::sqrt` | `fc`/`gain_db` shelving — pozn.: Enhancer používá `rbj_lowpass`/`rbj_highpass` (crossovery), high/low-shelf jsou k dispozici pro budoucí stage |
| `rbj_low_shelf(fc, gain_db, sr) → BiquadCoeffs` | off-RT / lazy v process | střed [Hz], boost/cut [dB], sr → koeficienty | `Enhancer::computeCoeffs_` | `std::pow`, `std::cos`, `std::sin`, `std::sqrt` | Audio EQ Cookbook RBJ low-shelf (k dispozici; Enhancer crossovery jsou LP/HP) |
| `gain_envelope_smooth(current, target, atk, rel) → float` | audio | aktuální gain, cílový gain, attack koef, release koef → nový gain | `Limiter::process` | — | `atk`: koeficient pro pokles (≈0 = rychlý); `rel`: koeficient pro vzestup (≈1 = pomalý) | Asymetrické IIR vyhlazení: pokud `target < current` (pokles), použije rychlejší `atk`; jinak pomalejší `rel`. Implementace: `coeff·current + (1−coeff)·target` |

`BiquadCoeffs` — POD struct s pěti normalizovanými koeficienty `b0, b1, b2, a1, a2`
(a0 = 1, tedy nevyskytuje se). Výchozí hodnota = jednotkový průchod (passthrough).

`BiquadState` — POD struct s delay elementy `s1, s2`. Výchozí hodnota nuly = tichý
start. `reset()` v každé stage ji vynuluje.

---

### `engine/dsp/dsp_chain.h` + `dsp_chain.cpp`

`DspChain` je tenký kontejner. Veškerá logika je inline v hlavičce; `.cpp` je kotva
překladové jednotky (záměrně prázdná).

| Metoda (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|--------------------|--------|----------------|---------|-------------|-----------|------------|
| `prepare(sr, max_block)` | off-RT | sample rate, max block | `Engine::prepare` (implicitně přes `dsp_`) | `DspStage::prepare` na každý stupeň | — | Propaguje `prepare` na AGC, Enhancer, Limiter v pořadí `stages_[0..2]` |
| `reset()` | off-RT | — | `Engine::reset` (pokud existuje) | `DspStage::reset` na každý stupeň | — | Vymaže stav všech tří stupňů |
| `process(L, R, n)` | audio | in-place stereo buffer, délka n | `Engine::processBlock` (krok 3b/5) | `DspStage::process` jen pro `enabled()` stupně | — | Iteruje `stages_[0..2]`; disabled stupeň přeskočí (no-op passthrough) |
| `stageCount() → int` | libovolné | — → 3 | GUI (`main.cpp`) | — | Konstanta; vrací vždy 3 |
| `stage(i) → DspStage&` | libovolné | index (0=AGC, 1=ENHANCER, 2=LIMITER) → reference | GUI (`main.cpp`, `app_context.cpp`), testy | — | Umožňuje GUI přistupovat k `IParamPage` rozhraní stage bez znalosti konkrétního typu |

Privátní pole: `agc_`, `enhancer_`, `lim_` (konkrétní instance), `stages_[3]` (pole
ukazatelů; pořadí fixní).

---

### `engine/dsp/agc.h` + `agc.cpp`

RMS-following **downward** AGC. Měří per-blok RMS přes oba kanály, spočítá cílový
gain `target_rms / rms`, ořízne ho na `[gain_floor, 1.0]` (nikdy nezesiluje), a
plynule sleduje cíl exponenciálním IIR per-vzorek (rychlý attack = pokles gainu,
pomalý release = návrat).

#### Parametry (kParams)

| Index | id | Label | Rozsah | Výchozí | Formát |
|-------|----|-------|--------|---------|--------|
| 0 | `target_rms` | TARGET | 0.01 – 0.5 | 0.15 | `"%.3f"` |
| 1 | `release_ms` | RELEASE | 10 – 2000 ms | 200 ms | `"%.0f ms"` |
| 2 | `gain_floor` | GAIN FLOOR | 0.0 – 1.0 | 0.05 | `"%.2f"` |

Attack time je fixní 5 ms (vypočten v `prepare()`): `atk_ = 1 − exp(−1/(0.005·sr))`.

#### Metody

| Metoda (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|--------------------|--------|----------------|---------|-------------|-----------|------------|
| `prepare(sr, max_block)` | off-RT | sample rate | `DspChain::prepare` | `applyParams_(true)`, `reset()` | — | Uloží `sr_`, vypočte fixní `atk_`, vynutí přepočet release, resetuje stav |
| `reset()` | off-RT | — | `DspChain::reset` | `cur_gain_.store(1.f)` | — | Nastaví `gain_ = 1.f` a atomický metr na 1.f |
| `get(i) / set(i,v)` | GUI / libovolné | index, hodnota | `renderParamPage`, persistence | `atomic::load/store(relaxed)` | i=0 target, i=1 release, i=2 floor | `set()` klampuje do `[min,max]` před uložením; triviální gettery |
| `enabled() / setEnabled(on)` | libovolné / GUI | — / bool | `DspChain::process`, `renderConfigPanel` | `atomic::load/store(relaxed)` | — | Bezámkový bypass flag; výchozí `false` (AGC je standardně vypnuté) |
| `meter(value&, label&) → true` | GUI | — → (aktuální gain lin, `"CURRENT GAIN"`) | diagnostický panel | `cur_gain_.load(relaxed)` | — | Čte atomický metr posílaný z audio threadu |
| `applyParams_(force)` | audio (z process) | bool force | `process()`, `prepare()` | `atomic::load(relaxed)`, `std::exp` | `force`: přepočítat vždy bez ohledu na `last_rel_ms_` | Lazy přepočet `rel_` z `release_ms_`; porovnává `rel_ms != last_rel_ms_`; volá `std::exp` jen při změně |
| `process(L, R, n)` | audio | in-place stereo buffer | `DspChain::process` | `applyParams_(false)`, `cur_gain_.store` | — | Viz níže |

**`AGC::process()` — podrobně:**

1. Pokud `!enabled_` → return (double-guard: `DspChain` již zkontroloval, ale stage
   to kontroluje znovu pro případ přímého volání).
2. `applyParams_(false)` — lazy přepočet release koeficientu.
3. Atomické načtení `target_rms` a `gain_floor`.
4. Per-blok RMS: `sqrt(sum(L²+R²) / (n·2))` — agregát přes celý blok, ne per-vzorek.
5. `target_gain = target_rms / rms`, klip na `[gain_floor, 1.0]`; pokud
   `rms ≤ 1e-6f`, `target_gain = 1.f` (ticho → beze změny).
6. Volba koeficientu: `target_gain < gain_` → attack (`atk_`), jinak release (`rel_`).
7. Per-vzorek IIR sledování: `gain_ += (target_gain − gain_) · coeff`, aplikace na
   L[i], R[i]. Tím se gain pohybuje plynule per-vzorek, i když `target_gain` je
   per-blok konstanta.
8. `cur_gain_.store(gain_, relaxed)` — metr pro GUI.

---

### `engine/dsp/enhancer.h` + `enhancer.cpp`

**Enhancer** (ex-BBE) — originální piano enhancer (NE klon BBE), hybrid enhancing
technik podložený teorií BBE. **Parallel-boost model:** pásma se PŘIČÍTAJí jako
boosty na (fázově zarovnaný) dry → při unity ziscích je výstup ploché aligned dry
(žádný comb notch). Komponenty: 3-pásmový split (LOW=LP250, MID=BP250–3k,
HIGH=HP3k→LP11k), **dynamický boost-when-loud** na HIGH (PROCESS, škálováno broadband
peak-monitorem), **konstantní** bass contour (CONTOUR), mid presence (MID), jemný
**harmonický exciter** (sudá nelinearita `high²` → 2. harmonická, high-passed ~4,5 kHz,
navázaný na PROCESS+scale), a fázové zarovnání **1.-řádovým all-passem** (~700 Hz,
magnitudově ploché). Viz spec 2026-06-03.

#### Parametry (kParams)

| Index | id | Label | Rozsah | Výchozí | Formát | Pozn. |
|-------|----|-------|--------|---------|--------|-------|
| 0 | `process` | PROCESS | 0 – 12 dB | 0 dB | `"%.1f dB"` | HIGH, **dynamický** (boost-when-loud) + řídí exciter |
| 1 | `contour` | CONTOUR | 0 – 12 dB | 0 dB | `"%.1f dB"` | LOW, konstantní |
| 2 | `mid` | MID | −6 – +6 dB | 0 dB | `"%.1f dB"` | MID presence (±) |

#### Metody

| Metoda (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Vysvětlení |
|--------------------|--------|----------------|---------|-------------|------------|
| `prepare(sr, max_block)` | off-RT | sr | `DspChain::prepare` | `computeCoeffs_()`, `reset()` | Přepočet crossover/all-pass koeficientů + peak attack/release; vynuluje stav |
| `reset()` | off-RT | — | `DspChain::reset` | — | Vynuluje biquad stavy (6/kanál), all-pass stav, peak env |
| `get(i)/set(i,v)` | GUI | index, dB | `renderParamPage`, persistence | atomic | i=0 process, 1 contour, 2 mid; `set()` klampuje |
| `enabled()/setEnabled` | GUI | bool | `DspChain::process`, config panel | atomic | Výchozí `false` |
| `meter(...) → false` | GUI | — | — | — | Enhancer nemá metr |
| `computeCoeffs_()` | off-RT | — | `prepare` | `rbj_lowpass/highpass` | LP250/HP250/LP3k/HP3k/LPcap11k + all-pass koef `(tan−1)/(tan+1)` @700 Hz |
| `process(L,R,n)` | audio | in-place stereo | `DspChain::process` | `biquad_tick`, `smoothstep` | Viz níže |

**`Enhancer::process()` — podrobně (per vzorek, oba kanály):**

1. Pokud `!enabled_` → return.
2. Broadband peak env (linkovaný `max(|L|,|R|)`, attack ~5 ms / release ~80 ms) → `scale = smoothstep(env, 0.05, 0.35)`.
3. Band-filtry z dry: `low=LP250(x)`, `mid=LP3k(HP250(x))`, `high=LPcap(HP3k(x))`.
4. Exciter: `exc = HP4.5k(high + 0.5·high²)`; `excite_amt = kExcite·(PROCESS/12)·scale`.
5. All-pass aligned dry: `xa = ap_c·x + x₋₁ − ap_c·y₋₁`.
6. Dynamický HIGH zisk: `gHi = 1 + (10^(PROCESS/20) − 1)·scale`.
7. **Parallel boost:** `out = xa + (gLow−1)·low + (gMid−1)·mid + (gHi−1)·high + exc·excite_amt`. Při všech parametrech 0 → `out = xa` (ploché, jen fázové zarovnání).

Celkem 4 biquad ticky (20 MAC) na vzorek, 2 na kanál, 2 filtry v sérii.

---

### `engine/dsp/limiter.h` + `limiter.cpp`

Stereo peak limiter. Per-vzorek počítá peak = `max(|L|, |R|)`, z toho okamžitý
cílový gain `thr_lin / peak` (nebo 1.0 pokud pod prahem), a plynule sleduje gain
přes `gain_envelope_smooth` (asymetrická IIR). Gain je vždy ≤ 1.0 (limituje jen
dolů). Exportuje gain reduction v dB jako GUI metr.

#### Parametry (kParams)

| Index | id | Label | Rozsah | Výchozí | Formát |
|-------|----|-------|--------|---------|--------|
| 0 | `threshold_db` | THRESHOLD | −40 – 0 dB | 0 dB | `"%.1f dB"` |
| 1 | `release_ms` | RELEASE | 10 – 2000 ms | 200 ms | `"%.0f ms"` |

Attack time je fixní 1 ms (vypočten jako `decay_coeff(0.001, sr)` v `prepare()`).
Na rozdíl od AGC používá `decay_coeff` (exponenciální klouzavý průměr), ne `1−exp`.

#### Metody

| Metoda (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|--------------------|--------|----------------|---------|-------------|-----------|------------|
| `prepare(sr, max_block)` | off-RT | sample rate | `DspChain::prepare` | `decay_coeff`, `applyParams_(true)`, `reset()` | — | Uloží `sr_`, fixní `atk_ = decay_coeff(0.001, sr)`, vynutí přepočet release a threshold, resetuje stav |
| `reset()` | off-RT | — | `DspChain::reset` | `gr_db_.store(0.f)` | — | `gain_ = 1.f`, GR metr na 0 dB |
| `get(i) / set(i,v)` | GUI / libovolné | index, hodnota | `renderParamPage`, persistence | `atomic::load/store(relaxed)` | i=0 threshold dB, i=1 release ms | Triviální; `set()` klampuje |
| `enabled() / setEnabled(on)` | libovolné / GUI | — / bool | `DspChain::process`, `renderConfigPanel` | `atomic::load/store(relaxed)` | — | Výchozí `false` |
| `meter(value&, label&) → true` | GUI | — → (GR v dB, `"GAIN REDUCTION"`) | diagnostický panel | `gr_db_.load(relaxed)` | — | Záporná nebo nulová hodnota (0 dB = bez redukce) |
| `applyParams_(force)` | audio (z process) | bool force | `process()`, `prepare()` | `db_to_lin`, `decay_coeff` | `force`: vždy přepočítat | Lazy přepočet: `thr_lin_ = db_to_lin(thr_db)` při změně `thr_db_`; `rel_ = decay_coeff(rel_ms·0.001, sr)` při změně `rel_ms_` |
| `process(L, R, n)` | audio | in-place stereo buffer | `DspChain::process` | `applyParams_(false)`, `gain_envelope_smooth`, `lin_to_db`, `gr_db_.store` | — | Viz níže |

**`Limiter::process()` — podrobně:**

1. Pokud `!enabled_` → `gr_db_.store(0.f, relaxed)` a return. (Nuluje metr při
   bypasu — GUI tak vidí 0 dB GR, ne starou hodnotu.)
2. `applyParams_(false)` — lazy přepočet `thr_lin_` a `rel_`.
3. Per-vzorek smyčka:
   - `peak = max(|L[i]|, |R[i]|)` — skutečný stereo peak.
   - `target = (peak > thr_lin_ && peak > 1e-9f) ? thr_lin_ / peak : 1.f` —
     cílový gain; pokud je pod prahem nebo ticho, target = 1 (bez zásahu).
   - `gain_ = gain_envelope_smooth(gain_, target, atk_, rel_)` — asymetrická IIR
     (rychlý pokles = attack, pomalý vzestup = release).
   - `gain_ = min(gain_, 1.f)` — clamp: limiter nesmí zesilovat.
   - Aplikace na L[i], R[i].
4. `gr_db_.store(lin_to_db(max(gain_, 1e-9f)), relaxed)` — metr: 0 dB při
   gain=1.0, záporné hodnoty při kompresi.

---

## Křížové odkazy

**Core — `Engine::processBlock`** (`engine/engine.cpp`, řádky ~218–219):
Volá `dsp_.process(out_l, out_r, n_samples)` jako krok 3b po master gainu a
po rezonanci. `DspChain` je přímý člen `Engine` (`dsp_`); přístup pro GUI přes
`Engine::dspChain()` → `DspChain::stage(i)`.

**GUI — parametrický panel** (`app/gui/panel_params.cpp`, `panel_config.cpp`,
`main.cpp`): `renderParamPage(ctx, page)` přijímá `IParamPage&`; GUI předává
reference na stage jako `IParamPage*` (pole `pages[4]` v `main.cpp`: VOICE, AGC,
ENHANCER, LIMITER). `renderConfigPanel` zobrazuje LED enable toggle — volá
`setEnabled()`. Hodnoty se načítají/ukládají přes `get()`/`set()` — atomické,
bez zámku, bezpečné přes vlákna.

**Multithreading — atomické parametry**: Všechny user-facing parametry
(`target_`, `release_ms_`, `floor_`, Enhancer `p_[3]`, `thr_db_`, `rel_ms_`,
`enabled_`) jsou `std::atomic<float/bool>` s `memory_order_relaxed` na obou
stranách. Přepočet odvozených koeficientů (biquad, decay, threshold lin) probíhá
výhradně na audio threadu v `process()` — koeficienty tedy nejsou sdíleny, proto
pro ně `atomic` není potřeba. Metrické atomiky (`cur_gain_`, `gr_db_`) jsou psány
audio threadem a čteny GUI threadem, opět s `relaxed` (jde o best-effort zobrazení,
ne o synchronizaci dat).

---

## Nálezy revize

**1. Dvojitý enabled-guard v `process()` (minor, záměrné).**
`DspChain::process()` kontroluje `s->enabled()` a volá `process()` jen při `true`.
Přesto `AGC::process()` i `Enhancer::process()` začínají `if (!enabled_) return`. Toto
je obranné programování — stage lze volat i přímo (testy, budoucí použití) bez
`DspChain`. Není to chyba, ale kód lze sjednotit odstraněním vnitřního guardu pokud
kontraktem bude „nikdo nevolá process() na disabled stage". Stávající stav je
bezpečnější.

**2. Limiter nuluje GR metr při disabled, AGC nenuluje `cur_gain_`.**
`Limiter::process()` při disabled zapíše `gr_db_ = 0.f` — GUI tak vždy vidí 0 dB.
`AGC::reset()` nastaví `cur_gain_ = 1.f`, ale `AGC::process()` při disabled jen
vrátí — metr zůstane na poslední hodnotě z doby, kdy AGC běžela. Po toggleu
enable/disable může GUI zobrazit zastaralou hodnotu gainu. Nelze to považovat za
RT-safety problém (atomic zápis by byl bezpečný), spíše UX nesourodost.

**3. AGC: per-blok target_gain, per-vzorek gain sledování.**
`target_gain` se počítá jednou za blok z blokového RMS. Koeficient (atk nebo rel)
se pak volí jednou za blok porovnáním `target_gain < gain_`. Pokud gain přeběhne
`target_gain` *v průběhu* bloku (rychlý attack, malý blok), koeficient se v daném
bloku nepřepne (zůstane attack i když by měl přejít na release). Jde o benign
approximation — standardní přístup ve výkonnostně orientovaném kódu; nezpůsobuje
nestabilitu ani sluchově zjevné artefakty.

**4. RBJ shelf: pravděpodobná chyba v `al` výpočtu (přenesená z icr).**
V `rbj_high_shelf` i `rbj_low_shelf` je výraz:
```cpp
float al = sinw / 2.f * std::sqrt((A + 1.f/A) * (1.f/1.f - 1.f) + 2.f);
```
Faktor `(1.f/1.f - 1.f)` = `0.0` vždy, takže celý výraz redukuje na
`al = sinw / 2.f * sqrt(2.f)`. V Audio EQ Cookbook je parametr `S` (shelf slope,
typicky 1.0) a vzorec zní `(A + 1/A)·(1/S − 1) + 2`. Pro `S = 1` vychází totéž
(`1/1 − 1 = 0`), takže implementace odpovídá `S = 1.0` (maximálně strmý shelf) —
výsledek je správný pro zamýšlené použití, přesto je výraz matoucí a nelze z něj
odvodit, že `S` je hardcoded na 1. Kód se nemění; doporučuje se komentář nebo
nahrazení konstantou `al = sinw * 0.5f * std::sqrt(2.f)`.

**5. RT-safety biquad přepočtu.**
`Enhancer::computeCoeffs_` volá `rbj_lowpass` / `rbj_highpass`, které volají
`std::pow`, `std::cos`, `std::sin`, `std::sqrt` — tyto standardní knihovní funkce
nejsou formálně RT-safe (mohou alokovat nebo zamykat v některých implementacích).
V praxi na macOS/Linux jsou RT-safe, ale jde o mlčky přijaté předpoklady.
Přepočet nastane jen při změně parametru (lazy sentinel), takže v ustáleném stavu
k němu nedochází. Stávající chování je v praxi přijatelné.
