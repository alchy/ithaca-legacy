# Dynamic BBE Sonic Maximizer — Design

**Goal:** Nahradit zjednodušené statické 2-shelf BBE věrnější dynamickou implementací (program-dependent HF boost + fázové zarovnání), aby zapnutí/vypnutí dělalo slyšitelnou změnu charakteru a model odpovídal měřením reálného BBE.

**Proč:** Měřením jsme prokázali, že statické BBE pracuje korektně (+5 dB shelvy), ale je nedramatické: dva statické EQ shelvy postrádají (1) **programově závislou dynamiku HF** a (2) **fázové/group-delay zarovnání**, což jsou dvě věci, které dělají charakter pravého BBE Sonic Maximizeru.

**Reference (web research):**
- Naganov 282i měření: pásma dělená 150/1200 Hz; group delay ~2,5 ms low / ~0,5 ms mid / 0 high; HF shelf ~5 kHz (max +12 dB) **dynamický dle vstupní úrovně** (boost klesá při tichu); bass contour ~150 Hz (max +12 dB) **konstantní**.
- Naganov/melp242 802 měření: HF boost = **bump ~4 kHz** (spíš peak než nekonečný shelf), výrazný **rolloff po 10 kHz**; „compressor-like" dynamika (při tichu HF boost mizí); **level-delta @ 6 kHz ≈ 4 dB** mezi nízkou a vysokou úrovní. (802 „Processing" na minimu i řeže HF −6 dB — varianta staršího modelu, mimo náš rozsah; děláme boost-only.)
- DOA 482i obvod: fázové zarovnání **all-pass** (~700 Hz) → spojitý group delay (delší pro nízké); HF boost shora omezený ~10 kHz; „dynamic program-driven restoration".
- **Syntéza:** HF boost = peak/shelf se středem **~4–5 kHz**, omezený shora **~10 kHz**; dynamický rozsah (level-delta) **řádu ~4 dB+** v HF; bass konstantní; fáze all-pass.

---

## Rozhodnutí (brainstorming)
- Implementovat **HF dynamiku + fázové zarovnání**.
- HF zákon = **autentický „boost-when-loud"**: HF boost škálován broadband level-monitorem vstupu (víc když silný signál, míň/nic v tichu).
- Fázové zarovnání = **all-pass kaskáda (~700 Hz)** (dle obvodu; bez delay bufferů), vyladěná na ~2,5/0,5/0 ms.
- HF boost = pásmo **~5–10 kHz** (ne nekonečný shelf).
- Bass contour = **konstantní** low-shelf ~150 Hz.
- Parametry GUI beze změny: `DEFINITION` (0..12 dB) = max HF, `BASS` rozšířit **0..10 → 0..12 dB**. Persistence klíče `bbe_definition`/`bbe_bass` beze změny (žádná migrace).
- Zachovat `DspStage` rozhraní + `enabled` gate.

## Architektura (signal path, per blok, alloc-free)

```
input ─┬─ [broadband level monitor]  (peak/RMS env, attack ~5ms / release ~80ms) → scale∈[0,1]
       │
       └→ [all-pass kaskáda ~700 Hz]      (fázové/group-delay zarovnání, vždy on)
          → [bass low-shelf ~150 Hz, +BASS dB konstantní]
          → [HF dynamický boost]:  wet = peak/shelf(~5–10 kHz, +DEFINITION dB)(signal)
                                    out = signal + scale·(wet − signal)
          → out
```

**Komponenty:**
1. **Level monitor** — broadband envelope vstupu (max(|L|,|R|) → smoothed, `gain_envelope_smooth` vzor jako AGC; attack rychlý ~5 ms, release ~80 ms). Mapuje na `scale∈[0,1]` přes práh/křivku (`scale = smoothstep(env, lo, hi)`; konstanty laděné). Linkovaný (sdílený pro L i R → stabilní stereo obraz).
2. **All-pass align** — kaskáda 1.–2. řádu all-pass biquadů kolem ~700 Hz; cíl: group delay ~2,5 ms @ low, ~0,5 ms @ mid, ~0 @ high. Počet/řád sekcí + f0 vyladit na Naganovovu křivku (validace níže). Per kanál (stavy v `prepare`).
3. **Bass contour** — `rbj_low_shelf(~150 Hz, BASS dB)`, konstantní (nezávislé na úrovni). Per kanál.
4. **HF dynamický boost** — `wet` = signál po HF boost filtru na **plném** `DEFINITION` zisku; výstup `out = dry + scale·(wet−dry)` (per-sample smoothed scale → boost-when-loud, bez zipperu). HF boost filtr = **peak/bell se středem ~4–5 kHz** (Q tak, aby pásmo ~3–10 kHz), případně high-shelf ~5 kHz + 1-pól LP ~10–12 kHz pro horní mez (rolloff po 10 kHz dle měření). Přesný tvar/střed vyladit dle Naganova. Per kanál. Cíl: level-delta v HF (nízká vs vysoká úroveň) **alespoň ~3–4 dB** (síla dynamiky dle 802).

**RT:** všechny biquad/all-pass stavy + smoothing stav alokované/nulované v `prepare`; žádné alokace v `process`. `enabled=false` → čistý bypass (return).

## Parametry / GUI / persistence
- `kParams`: `{"definition","DEFINITION",0..12,def 0}`, `{"bass","BASS",0..12,def 0}` (bump max bass na 12). `paramCount()=2` beze změny → BBE GUI stránka i `bbe_definition`/`bbe_bass` v `state.json` fungují bez migrace.
- Vnitřní konstanty (all-pass f0, level prahy, attack/release, HF horní cut) jsou neexponované konstanty v `.cpp` (laditelné).

## Validační harness (nahradí `tests/test_bbe_measure.cpp`)
Cíl: objektivně porovnat naši odezvu s Naganovem.
1. **Magnitudový sweep** — log-spaced sinusy 20 Hz–20 kHz, ustálený RMS → `gain_dB(freq)`. Měřeno při **dvou vstupních úrovních** (nízká ~−40 dBFS, vysoká ~−6 dBFS) → dvě křivky.
2. **Group delay** — impulz → výstup → FFT → unwrap fáze → `−dφ/dω` → `group_delay_ms(freq)`. (Jednoduchá DFT na log mřížce stačí; nebo měření zpoždění band-limited pulzů.)
3. **CSV výstup** `freq, gain_db_low, gain_db_high, group_delay_ms` → uživatel overlayuje na Naganovovy grafy (gnuplot/sheet).
4. **doctest asserty** na cílové charakteristiky (s tolerancí):
   - bypass (enabled=false) → gain ≈ 0 dB napříč, group delay ≈ 0.
   - bass shelf: gain @ 60 Hz roste s `BASS`, corner ~150 Hz; **nezávislé na úrovni** (low/high křivka stejná v bассu).
   - HF: gain @ ~4–6 kHz roste s `DEFINITION` a **závisí na úrovni** (high-level křivka > low-level v HF) — autentická dynamika. Konkrétní cíl: level-delta @ ~6 kHz **≥ ~3 dB** (dle 802 ~4 dB).
   - HF horní mez: boost @ >12 kHz výrazně omezený / rolloff po ~10 kHz (ne plný shelf donekonečna).
   - group delay: monotónně klesá s frekvencí, ~low/mid/high ≈ Naganov ±tolerance; stabilita (žádné NaN/blow-up).

**Limit:** Naganov měřil analogový hardware → ověřujeme **charakteristickou shodu** (polohy rohů, dB úrovně, ms zpoždění, level-závislost), ne bit-exact křivku.

## Soubory
- **Rewrite:** `engine/dsp/bbe.{h,cpp}` (nová dynamická implementace; zachovat `DspStage` API, `kParams` ids).
- **Možná přidat:** `engine/dsp/dsp_math.h` — all-pass koeficient helper (`rbj_allpass(fc, q, sr)`) a/nebo smoothstep, pokud chybí.
- **Rewrite test:** `tests/test_bbe_measure.cpp` → `tests/test_bbe_response.cpp` (sweep + group delay + CSV + asserty); registrace v `tests/CMakeLists.txt`.
- **Docs:** `docs/reference/G-dsp.md` (BBE sekce — nová topologie), průběžně.

## Edge cases
- Velmi tichý vstup → scale→0 → HF boost ≈ 0 (žádné zesílení šumu); bass + all-pass stále aktivní (konstantní).
- `DEFINITION=0` / `BASS=0` → příslušná složka neutrální; all-pass stále zarovnává fázi (jemné). Pozn.: i s param=0 bude enabled BBE měnit fázi (group delay) — to je správně (alignment je vždy on), ale magnitudově ~plochá → toggle při param=0 bude slyšet jen jako fázový/transientní rozdíl. Akceptováno.
- Změna SR (`prepare`) → přepočet všech koeficientů + delay/all-pass dle nového sr.
- Mono kompatibilita: linkovaný level monitor → bez stereo image shiftu.

## Mimo rozsah
- Subharmonic synth / „Lo Contour" rozšíření nad rámec bass shelfu.
- Konfigurovatelné all-pass f0 / prahy v GUI (interní konstanty).
- Lineárně-fázová varianta (BBE je záměrně minimum-phase/all-pass).
- Oversampling.
