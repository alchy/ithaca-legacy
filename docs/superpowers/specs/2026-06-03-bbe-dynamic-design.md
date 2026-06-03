# Dynamic Enhancer (ex-BBE) — Design

**Naming:** Modul se jmenuje **Enhancer** (ex-„BBE") — všude: třída `Enhancer`, soubory `enhancer.{h,cpp}`, GUI label „ENHANCER", persistence klíče `enhancer_*` (migrace ze starých `bbe_*`). „BBE" v tomto specu = referenční hardware, ne náš modul.

**Goal:** Nahradit statické 2-shelf BBE **hybridním** modelem podle reálného obvodu BBE Sonic Stomp / Lumin (3-pásmová sumační topologie: PROCESS/CONTOUR/MID) **plus** programově závislá dynamika (boost-when-loud) na PROCESS pásmu — aby zapnutí/vypnutí dělalo slyšitelnou změnu a model odpovídal měřením i schématu.

## Reference (web + schéma)
- **AionFX Lumin / BBE Sonic Stomp schéma** (uživatelem dodané): 3× TL072, 3 ovladače **PROCESS / CONTOUR / MID** (50kB), sumační mix. HIGH pásmo (IC2A): caps 3n3/1n5 @ 22k → rohy ≈ **2,2 / 4,8 kHz** (HIFREQ switch). LOW pásmo (IC3A): caps 47n/22n @ 15k/10k → ≈ **226/339/482 Hz** (LOFREQ switch). Výstup R17 56k + C6 100p. **Statická** verze (bez VCA).
- Naganov 282i/802: HF boost ~4–5 kHz dynamický dle úrovně; rolloff po ~10 kHz; bass ~150 Hz konstantní; group delay ~2,5 ms low / ~0,5 ms mid / 0 high; level-delta @ 6 kHz ≈ 4 dB.
- DOA 482i + GroupDIY: state-variable split (LP/BP/HP) + mix; HF dynamika = VCA řízený **peak detektorem** (rackmount, čip NJM2150AD).

## Rozhodnutí (brainstorming)
- **Hybrid:** 3-pásmová topologie + hodnoty ze schématu **+** dynamika boost-when-loud na PROCESS (HIGH) pásmu.
- **3 parametry:** `PROCESS` (HIGH, 0..12 dB, **dynamický**), `CONTOUR` (LOW, 0..12 dB, konstantní), `MID` (−6..+6 dB, konstantní, default 0).
- Dynamika = **peak** level-monitor vstupu (rychlý attack, compressor-like) škáluje PROCESS pásmo: víc boostu když silný signál, míň/nic v tichu.
- HF pásmo shora omezené ~10 kHz (rolloff dle měření).
- Zachovat `DspStage` rozhraní + `enabled` gate.

## Architektura (per blok, alloc-free)

```
input ─┬─ [broadband PEAK level monitor] (attack ~5ms / release ~80ms) → scale∈[0,1]
       │
       └→ 3-pásmový split (crossover):
            LOW  (< ~250 Hz)        → × CONTOUR_gain (konstantní)        → delay ~2,5 ms
            MID  (~250 Hz–3 kHz)    → × MID_gain     (konstantní)        → delay ~0,5 ms
            HIGH (> ~3 kHz, cut ~10 kHz) → × PROCESS_gain × scale (DYNAMICKÝ) → delay 0
          → sum(LOW,MID,HIGH) → out
```

**Komponenty:**
1. **3-pásmový split** — crossovery ~**250 Hz** (low/mid) a ~**3 kHz** (mid/high), Linkwitz-Riley 2. řádu (nebo SVF LP/BP/HP). HIGH navíc 1-pól LP ~**10–12 kHz** (rolloff dle měření). Per kanál.
2. **Per-pásmové zisky:** `LOW × 10^(CONTOUR/20)`, `MID × 10^(MID/20)`, `HIGH × 10^(PROCESS/20) × scale`. Zisk 1.0 (0 dB) = pásmo prochází neutrálně → součet ≈ originál (transparentní při všech 0).
3. **Dynamika HIGH (boost-when-loud):** broadband **peak** detektor vstupu → smoothed env → `scale∈[0,1]` (`scale = smoothstep(env, lo, hi)`). HIGH zisk se škáluje `scale` per-sample (smoothed, bez zipperu). Silný signál → plný PROCESS boost; ticho → boost mizí (šetří šum). Linkovaný detektor (max L/R) → stabilní stereo.
4. **Fázové/group-delay zarovnání:** per-pásmové zpoždění **LOW ~2,5 ms / MID ~0,5 ms / HIGH 0** (integer-sample delay linky, alokované v `prepare`). Přímo reprodukuje Naganovovu naměřenou group-delay křivku. (Pozn.: alternativa all-pass ~700 Hz — ale s adoptovaným 3-pásmovým splitem je per-pásmové zpoždění přesnější a přímočařejší; finální realizaci potvrdí validační harness.)

**RT:** všechny crossover/filtr stavy, delay buffery (max ~2,5 ms ≈ 120 framů @48k) a smoothing stav alokované/nulované v `prepare`; `process` bez alokací. `enabled=false` → čistý bypass.

## Parametry / GUI / persistence
- `Enhancer::paramCount()=3`, `kParams`:
  - `{"process","PROCESS", 0..12 dB, def 0}` (HIGH, dynamický)
  - `{"contour","CONTOUR", 0..12 dB, def 0}` (LOW, konstantní)
  - `{"mid","MID", -6..+6 dB, def 0}` (MID, konstantní)
- `name()="ENHANCER"`, `hasEnable()=true`. GUI config stránka vykreslí 3 slidery + ON/OFF (generický `renderParamPage`).
- **Persistence (`GuiState`):** `enhancer_enabled` (bool), `enhancer_process` (float dB), `enhancer_contour` (float dB), `enhancer_mid` (float dB). Save zapíše `enhancer_*`. Load čte `enhancer_*`; **migrace:** chybí-li, čte staré `bbe_enabled`→enabled, `bbe_definition`→process, `bbe_bass`→contour (mid default 0). `app_context.cpp` mapuje state → stage settery; `main.cpp` debounce watched fields.
- Vnitřní konstanty (crossover freq, delay ms, peak attack/release, scale prahy, HF horní cut) = neexponované konstanty v `.cpp`.

## Validační harness (nahradí `tests/test_bbe_measure.cpp` → `test_enhancer_response.cpp`)
1. **Magnitudový sweep** 20 Hz–20 kHz (ustálený RMS) → `gain_dB(freq)`, při **dvou úrovních** (nízká ~−40 dBFS, vysoká ~−6 dBFS) → dvě křivky.
2. **Group delay** z impulzní odezvy (FFT → unwrap → `−dφ/dω`).
3. **CSV** `freq, gain_db_low, gain_db_high, group_delay_ms` → overlay na Naganovovy grafy.
4. **doctest asserty** (tolerance):
   - bypass (enabled=false) → gain ≈ 0 dB, group delay ≈ 0.
   - CONTOUR: gain @ 100 Hz roste s CONTOUR, **nezávislé na úrovni**; corner ~250 Hz.
   - PROCESS: gain @ ~4–6 kHz roste s PROCESS a **závisí na úrovni** (high-level > low-level; delta @ ~6 kHz ≥ ~3 dB) — autentická dynamika.
   - MID: gain @ ~1 kHz reaguje na MID (±).
   - HF horní mez: boost @ >12 kHz výrazně omezený (rolloff po ~10 kHz).
   - group delay: ~2,5/0,5/0 ms napříč pásmy ±tolerance; stabilita (žádné NaN/blow-up).

**Limit:** reference = analog HW → ověřujeme **charakteristickou shodu** (rohy, dB, ms, level-závislost), ne bit-exact.

## Soubory (vč. rename BBE → Enhancer)
- **Create/Rename:** `engine/dsp/bbe.{h,cpp}` → `engine/dsp/enhancer.{h,cpp}`; třída `BBE`→`Enhancer`; `name()="ENHANCER"`; nová hybridní implementace; `DspStage` API.
- **Modify:** `engine/dsp/dsp_chain.h` (member `bbe_`→`enhancer_`, include, komentář „AGC → ENHANCER → Limiter"); root `CMakeLists.txt` (`bbe.cpp`→`enhancer.cpp`).
- **Možná přidat:** `engine/dsp/dsp_math.h` — `smoothstep`, crossover (LR) helper, pokud chybí (jinak složit z `rbj_*` / biquad).
- **Persistence (`app/gui/persistence.{h,cpp}`):** pole `bbe_*` → `enhancer_enabled/process/contour/mid`; save `enhancer_*`; load + migrace z `bbe_*`. `app/gui/app_context.cpp` (mapování 3 paramů na `enhancer.set(0/1/2,…)` + setEnabled), `app/gui/main.cpp` (debounce: nahradit `bbe_*` za `enhancer_*` × 4 pole).
- **GUI label:** „ENHANCER" přes `name()` (ověřit, že nikde natvrdo „BBE").
- **Rewrite test:** `tests/test_bbe_measure.cpp` → `tests/test_enhancer_response.cpp` (+ registrace v `tests/CMakeLists.txt`).
- **Docs:** `docs/reference/G-dsp.md` + `H-gui.md` (BBE → Enhancer, hybridní 3-pásmová topologie).

## Edge cases
- Velmi tichý vstup → scale→0 → PROCESS (HIGH) boost ≈ 0 (žádné zesílení šumu); CONTOUR/MID konstantní.
- Všechny parametry 0 (PROCESS=CONTOUR=MID=0 dB) → pásma procházejí na 0 dB → součet ≈ originál; **ale per-pásmové delaye jsou stále aktivní** → enabled Enhancer i s param=0 dělá fázové/group-delay zarovnání (jemné), to je správně. Toggle při param=0 = fázový rozdíl.
- `MID` je ±: default 0 = bez změny středů; +/− boost/cut.
- Změna SR (`prepare`) → přepočet crossover/filtr koeficientů + delay framů dle sr.
- Mono kompat: linkovaný peak detektor → bez stereo image shiftu.

## Mimo rozsah
- HIFREQ/LOFREQ přepínače rohů ze schématu (volíme fixní rohy; ne GUI switch).
- Statická-only verze (jdeme hybrid s dynamikou).
- Subharmonics / oversampling / lineárně-fázová varianta.
- Konfigurovatelné delay/crossover/prahy v GUI (interní konstanty).
