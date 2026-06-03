# Convolver — Cabinet/Body Simulation — Design

**Goal:** Doplnit **tělo nástroje** (soundboard/skříň) k blízko-snímaným strunovým vzorkům přes krátkou FIR konvoluci. Subtilní zlepšení charakteru, NE efektní transformace (žádné katedrály/reverb). IR ze dvou zdrojů: přibalené WAV (port icr soundboard) + **syntetický modální generátor** kalibrovaný na fyziku klavírní desky (digitální cesta jako Enhancer/Resonance).

## Reference (papíry z IhtacaPapers + web)
- **`RR_9530.pdf`** (Castera/Chabassier/Fisette/Wagijo 2023) — **primární kalibrace:** modální frekvence desky (mód 1=27, 2=42, 3=63, 7=121, 10=164, 20=289 Hz), **modální tlumení `f_ve(f) = 2·10⁻⁵·f² + 7·10⁻²·f`** (per-mód decay `τ=2/f_ve(f)`), ~2500 modů do 10 kHz, materiál smrk ρ380/Ex11/Ey0,65 GPa.
- **`hal.pdf`** (Bank & Chabassier 2019, IEEE SPM) — metoda: *„soundboard IR je šumovité, mnohem kratší doznív než pokoj"* → krátká IR + FIR; radiation-vs-termination split.
- **`RR-8181`/`Modeling…`/`Piano-model-revis`** (Chabassier) — modální deska, per-mód tlumení ↑ s f, **módy hlavně <800 Hz–1 kHz**, „tlumeny rychleji než struny → slyšitelné v attacku".
- **`Teng 2012`** — ⚠️ body-knock IR konvoluce „teplejší, méně kovové"; ALE jediná broadband IR **přebíjí vysoké tóny** → držet MIX subtilní / mírný HF.
- Web (Ege): modální hustota desky ~0,01–0,05 modů/Hz (mid-freq, měřeno), loss factor ~2 %.
- icr `engine/dsp/convolver/` (direct time-domain referenční impl) + `banks/soundboard/pl-grand-*-soundboard.wav` (~25 ms IR).

## Rozhodnutí (vlastní; init hodnoty na mně)
- **Algoritmus:** direct time-domain FIR konvoluce, **mono IR** na L i R, **wet/dry MIX**. Zero-latency, bez FFT závislosti. CPU O(IR_len) — fixní náklad na master sběrnici (1 konvolver, ne per-voice; DSP LOAD metr hlídá).
- **Pozice:** **první** v řetězci — `CONVOLVER → AGC → ENHANCER → Limiter` (tělo před masteringem; icr to má stejně).
- **Max IR délka:** 8192 tapů (~170 ms @48k); ring buffery alokované na MAX v `prepare` → runtime IR swap bez realokace.
- **Default:** enabled=false, MIX=0,25 (subtilní), IR = syntetický „Body soft".

## Architektura

```
input → Convolver(mono IR, MIX) → AGC → ENHANCER → Limiter
        history ring(L/R, fixed MAX) ; out = (1-mix)·dry + mix·Σ ir[k]·x[n-k]
```

**1. `Convolver` (DspStage, engine/dsp/convolver.{h,cpp}):**
- `process(L,R,n)`: per vzorek zapiš do ring (L,R), spočti `Σ ir[k]·hist[n-k]` (direct), wet/dry mix. Alloc-free.
- Ring buffery `buf_l_/buf_r_` velikosti MAX (8192) alokované v `prepare`. Aktivní IR délka ≤ MAX.
- `enabled` (atomic), `MIX` (atomic, 0..1).
- **RT-safe IR swap:** IR držena jako `std::shared_ptr<const std::vector<float>>` (nebo double-buffer + atomic index); audio thread načte ukazatel na začátku `process`. GUI/loader publikuje novou IR atomicky (stará žije přes shared_ptr dokud ji audio nedočte). Ring fixní → swap nemění jeho velikost. (Vzor jako resonance cache ready-flag.)
- **Normalizace IR** při setIR: na jednotnou energii (nebo unit peak) → predikovatelný wet level napříč IR.

**2. IR zdroje (oba vyústí ve `float` IR ≤ MAX):**
- **WAV loader** (`engine/dsp/ir_wav.{h,cpp}` nebo reuse `wav_reader`): mono WAV, resample na engine SR (lineárně), → IR vektor. Default WAV port z icr `pl-grand-*-soundboard.wav` do `banks/ir/` v projektu.
- **Synteticky modální generátor** (`engine/dsp/ir_modal.{h,cpp}`): vyrobí IR z fyzikálního modelu desky:
  ```
  ir[t] = Σ_n A_n · exp(-t/τ_n) · sin(2π f_n t + φ_n),   t ∈ [0, len)
  ```
  - **Modální frekvence f_n:** seed nejnižší z RR_9530 (27,42,63,86,121,164,…289 Hz), pak doplnit nahoru rostoucí hustotou do ~4–5 kHz (cílově desítky až ~120 modů — ne 2500; krátká coloration IR, ne plný FEM).
  - **Decay τ_n = 2 / f_ve(f_n)**, `f_ve(f)=2e-5·f²+7e-2·f` (RR_9530) → nízké módy ~200–280 ms, vysoké ~10 ms.
  - **Amplituda A_n:** spektrální obálka — energie koncentrovaná <~1 kHz, jemný rolloff výš (dle „módy hlavně <800 Hz–1 kHz"). Náhodná fáze φ_n (noiselike, dle Bank).
  - **Délka IR:** oříznout na ~`min(len_cap, kde obálka klesne pod práh)`, typicky ~80–150 ms (cap 8192).
  - **Presety:** „Body soft" (tlumenější HF), „Body bright" (víc presence ~2–4 kHz). Parametry (počet modů, HF rolloff, base decay scale) = laditelné konstanty.

## GUI / persistence
- Nová `IParamPage` **CONVOLVER** (`hasEnable=true`): `MIX` slider (0..1, default 0,25). IR výběr = dropdown z dostupných zdrojů (přibalené WAV v `banks/ir/` + generované presety „Body soft/bright"). Pořadí stránek: **CONVOLVER**, MASTER, RESONANCE, AGC, ENHANCER, LIMITER (config_page 0..5) — nebo CONVOLVER za RESONANCE; finální pořadí v plánu (CONVOLVER musí být **první v audio řetězci** bez ohledu na pořadí GUI stránek).
- Persistence: `convolver_enabled` (bool), `convolver_mix` (float), `convolver_ir` (string — název zdroje/presetu nebo cesta).
- DspChain rozšířen na 4 stage: `Convolver` první.

## RT-safety
- `process` alloc-free; ring + IR alokace off-RT (`prepare`/`setIR`).
- IR swap přes shared_ptr/double-buffer (audio čte ukazatel na začátku bloku); stará IR žije dokud ji hlas nedočte. Ring fixní velikost.
- `enabled=false` → bypass (return).
- Anti-denormal netřeba (FIR bez zpětné vazby; IR konečná).

## Validační harness (`tests/test_convolver.cpp` + `test_ir_modal.cpp`)
- **Konvoluce korektnost:** impulz na vstupu (1,0,0,…) + IR → výstup = IR (× mix); identity IR (1,0,0,…) → passthrough; MIX=0 → bypass; MIX=1 → plně wet.
- **IR normalizace:** energie/peak v cíli.
- **Modální generátor (verifikace proti fyzice):**
  - per-mód decay: vygeneruj IR z jediného módu f → změř pokles obálky → `τ ≈ 2/f_ve(f)` (±tolerance) pro f=100/500/1000/2000 Hz.
  - spektrální obálka: FFT IR → energie koncentrovaná <~1 kHz (víc než nad 2 kHz).
  - délka/stabilita: IR konečná, bez NaN, energie klesá monotónně (decay).
  - modální frekvence: peaky ve spektru u seedovaných nízkých modů (27/42/63…).
- **RT:** `process` alloc-free (review); DSP LOAD při typické IR (~80 ms) — poznámka, ne hard assert.

## Soubory
- **Create:** `engine/dsp/convolver.{h,cpp}` (DspStage), `engine/dsp/ir_modal.{h,cpp}` (modální generátor), IR WAV loader (reuse `io/wav_reader` nebo malý mono loader).
- **Modify:** `engine/dsp/dsp_chain.h` (přidat `Convolver convolver_` jako první stage; `stageCount`→4), root `CMakeLists.txt`.
- **GUI:** `app/gui/convolver_page.h` (IParamPage), `main.cpp` (pole stránek + config_page rozsah), `app_context.cpp` (init + IR load), `persistence.{h,cpp}` (3 pole).
- **Assets:** port icr `banks/soundboard/pl-grand-*-soundboard.wav` → `banks/ir/` v projektu.
- **Tests:** `tests/test_convolver.cpp`, `tests/test_ir_modal.cpp` + `tests/CMakeLists.txt`.
- **Docs:** `docs/reference/G-dsp.md` (Convolver sekce), `H-gui.md` (CONVOLVER stránka).

## Edge cases
- IR delší než MAX → ořízni na MAX (8192) + log warning.
- IR SR ≠ engine SR → resample (lineárně) při loadu.
- Prázdná/chybná IR → stage se chová jako bypass (žádný pád).
- Změna IR za běhu → atomický swap; krátký „překryv" doznívajícího ringu se starou IR je akceptovatelný (subtilní).
- Vysoké tóny (Teng caveat) → default MIX nízký (0,25) + obálka generátoru s mírným HF; uživatel doladí.
- Změna SR (`prepare`) → re-resample WAV IR / re-generuj modální IR pro nové SR.

## Mimo rozsah (v1)
- Partitioned FFT / dlouhé reverb IR (katedrály) — jiný use-case; FFT jako budoucí upgrade pro dlouhé IR.
- True-stereo IR (L/R nezávislé) — mono IR na L/R stačí pro tělo.
- Per-nota / key-tracked IR (Teng naznačuje, že by pomohlo u extrémů) — future.
- Spuštění FEM v enginu (jen kalibrace z publikovaných čísel).
