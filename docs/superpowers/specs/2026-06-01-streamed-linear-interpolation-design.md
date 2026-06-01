# Lineární interpolace ve streamované části (Voice + ResonanceVoice)

**Datum:** 2026-06-01
**Stav:** schváleno k implementaci
**Souvisí s:** dřívější oprava ring/pos_inc_ (commit 20c04f1) — toto je dokončení TODO „faze 6/7" zmíněného v `voice.cpp` a `resonance_voice.cpp`.

## Cíl

Nahradit nearest-neighbor čtení ve streamované (ring) části přehrávání
**lineární interpolací** mezi dvěma sousedními framy, řízenou zlomkovou částí
`position_`. Tím zmizí schodovité zkreslení / aliasing pro `pos_inc_ != 1.0`
(typicky 44100 Hz sample na 48000 Hz enginu, `pos_inc ≈ 0.919`) a vyhladí se
šev mezi RAM regionem (head / preload_resonance) a ringem.

Na 48000 Hz (`pos_inc_ = 1.0`) je výstup **beze změny** — `frac` je vždy 0,
takže interpolace vrací přesně původní vzorek. Proto se chyba dosud
neprojevila (všechny banky byly 48k).

## Kontext / současný stav (verbatim chování)

Sampl se čte na zlomkové `position_` (krok `pos_inc_ = sample_sr/engine_sr`).

**Head region (RAM)** už interpoluje lineárně:
```cpp
float frac = position_ - p0;
sL = head[p0]*(1-frac) + head[p1]*frac;
```

**Ring region** (po dřívější opravě) bere floor/nearest — drží JEDEN cache
frame `ring_cur_l_/r_` na indexu `ring_cur_idx_ = floor(position_)`, zlomek
zahazuje (`voice.cpp:214-249`, `resonance_voice.cpp:194-223`).

**preload_resonance region** (jen ResonanceVoice, v RAM) také bere
nearest-neighbor (`resonance_voice.cpp:185-193`).

**Šev** `p0 == head_frames-1` (poslední vzorek hlavy) se nedá interpolovat
přes hranici do ringu → dnes vrací holý vzorek bez interpolace
(`voice.cpp:208-213`).

## Architektura: „lo/hi sliding window"

Místo jednoho cache framu držíme **dvojici** sousedních framů:
- `ring_lo_*` na indexu `floor(position_)`     (= dřívější `ring_cur_*`)
- `ring_hi_*` na indexu `floor(position_) + 1` (lookahead o 1 frame)

**Výstup** každého output framu:
```cpp
float frac = (float)(position_ - (double)ring_lo_idx_);
sL = ring_lo_l_*(1.f - frac) + ring_hi_l_*frac;
sR = ring_lo_r_*(1.f - frac) + ring_hi_r_*frac;
```

**Posun okna** per output frame (dokud `lo` nedosáhne `floor(position_)`):
```cpp
const int64_t target = (int64_t)position_;
while (ring_lo_idx_ < target) {
    ring_lo_l_ = ring_hi_l_; ring_lo_r_ = ring_hi_r_;   // hi → lo
    if (!ring_->popFrame(ring_hi_l_, ring_hi_r_)) { /* EOF/underrun, viz níže */ }
    ring_lo_idx_++;
}
```
Tím se z ringu spotřebuje správný počet framů (zachová dřívější `pos_inc_`
fix) a zároveň jsou oba framy k dispozici pro interpolaci.

## Šev head→ring (a preload_resonance→ring)

Při PRVNÍM vstupu do ring větve (`ring_lo_idx_ < 0`) naseedovat:
- **Voice:** `ring_lo_* = head[head_frames-1]`, `ring_lo_idx_ = head_frames-1`;
  `ring_hi_*` = první `popFrame()` z ringu.
- **ResonanceVoice:** `ring_lo_* = preload_resonance[res_frames-1]`,
  `ring_lo_idx_ = res_end-1`; `ring_hi_*` = první `popFrame()`.

Interpolace pak plynule překlene hranici (to je „1-frame lookbehind" z TODO).

**Sloučení „last head sample" case:** dnešní `else if (p0 < head_frames)`
větev (poslední vzorek hlavy bez interpolace) se ZRUŠÍ a tento hraniční vzorek
spadne rovnou do ring větve s naseedovaným `lo` = posledním head framem. Tím
i poslední head vzorek interpoluje směrem k prvnímu ring framu. Podmínka ring
větve se tedy změní z `else if (p0 < head_frames) {...} else if (ring_)` na
jedinou `else if (ring_)` pokrývající `p0 >= head_frames-1`.

POZOR na pořadí: head interpolační větev musí zůstat `p0 < head_frames-1`
(potřebuje `p1 = p0+1` uvnitř hlavy). Při `p0 == head_frames-1` jde řízení do
ring větve, kde `lo = head[head_frames-1]` a `hi` = ring[0].

## EOF / konec streamu — hold last sample (clamp)

Když `popFrame` selže během posunu okna:
- **EOF** (`ring_->eof_` true): nastav `ring_hi_* = ring_lo_*` (clamp) a přestaň
  posouvat — pro zbývající zlomkovou pozici výstup plynule drží poslední
  platný vzorek (`s = lo`). Jakmile `frac` dojede / `position_` překročí konec,
  deaktivuj (`active_ = false`, `log_end("ring_eof_drained")` u Voice). Žádný
  click, hlas dohraje čistě.
- **Underrun** (ring prázdný, ale NE EOF): beze změny oproti dnešku — spustit
  fast underrun fade do ticha + log (Voice: non-RT warn; ResonanceVoice:
  LOG_RT_WARN), výstup `sL=sR=0` po dobu fade.

## preload_resonance region (jen ResonanceVoice, v RAM)

Interpolovat přímo v bufferu:
```cpp
int local = p0 - res_start;
float frac = (float)(position_ - (double)p0);
if (local < res_frames - 1) {
    sL = res[local*2]*(1-frac)     + res[(local+1)*2]*frac;
    sR = res[local*2+1]*(1-frac)   + res[(local+1)*2+1]*frac;
} else {
    // poslední preload_resonance frame → ring větev (seed lo, hi=ring[0])
}
```
Stejné sloučení jako u Voice: poslední preload frame spadne do ring větve.

## Dotčené soubory

| Soubor | Změna |
|--------|-------|
| `engine/voice/voice.h` | nahradit `ring_cur_l_/r_`,`ring_cur_idx_` za `ring_lo_l_/r_`, `ring_hi_l_/r_`, `ring_lo_idx_` |
| `engine/voice/voice.cpp` | ring větev: lo/hi okno + interpolace; sloučit „last head sample" case; EOF clamp; reset polí v `start()` a `hardStop()` |
| `engine/voice/resonance_voice.h` | totéž v polích |
| `engine/voice/resonance_voice.cpp` | interpolace v preload_resonance regionu + ring větev (lo/hi) + šev + EOF clamp; reset v `start()` a `hardStop()` |

Pole: pět floatů + jeden int64 na hlas. `ring_hi_` se vždy drží naplněný po
seedu, takže není potřeba separátní „hi valid" flag — seed (`ring_lo_idx_ < 0`)
ho inicializuje, EOF clamp ho drží roven `lo`.

## Testy (TDD)

Rozšířit existující harness (`tests/test_long_sample_stream.cpp` styl pro
Voice, `tests/test_resonance_stream_sr.cpp` pro ResonanceVoice; ramp WAV +
TempDir, render po blocích se sleep pro stream worker).

1. **Voice @44.1k ramp hladkost:** ramp WAV header SR 44100, engine 48000
   (`pos_inc ≈ 0.919`), streamovaný. Ověřit, že po naběhu jsou sousední
   výstupní framy hladké (rozdíl ≈ `pos_inc × krok rampy`), BEZ opakovaných
   nebo přeskočených hodnot typických pro nearest-neighbor. (RED: nearest dá
   „schody" — některé sousední framy identické, jiné skok 2×.)
2. **Šev head→ring:** kolem `head_frames` žádná diskontinuita > tolerance
   (lineární ramp → join musí být plynulý).
3. **EOF hold:** streamovaný ramp dohraje do konce bez clicku; poslední
   nenulový výstup ≈ poslední vzorek souboru; pak `activeVoices()==0`.
4. **48k regrese:** `pos_inc=1.0` ramp → výstup numericky identický se vstupem
   (frac=0 ⇒ interpolace = původní vzorek). Chrání před změnou chování na
   produkčních 48k bankách.
5. **ResonanceVoice @96k nebo @44.1k:** rozšířit `test_resonance_stream_sr` o
   kontrolu hladkosti streamované rezonance (aspoň jeden SR≠48k případ).

## Mimo rozsah (YAGNI)

- Anti-aliasing low-pass filtr pro downsampling (96k→48k) — samostatná fáze.
- Vyšší-řádová interpolace (cubic / sinc / windowed) — lineární stačí pro
  piano-class samply a malé SR konverze.
- Multi-mic mix, pitch transpozice — nesouvisí.
