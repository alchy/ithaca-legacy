# Sample Rate & Resampling — implementace + doporučený workflow

> ## Účel dokumentu
>
> Sjednotit na jednom místě **(a)** jak engine zachází se sample rate (SR) dnes —
> kde žije výstupní SR, jak se převádí SR samplu na výstupní SR, a co se *ne*resampluje —
> a **(b)** doporučený postup pro autory bank, jak dostat co nejlepší kvalitu z nahrávky
> ve vyšším SR (96 kHz) do 48 kHz banky. Cíl: tohle už nikdy nereverse-engineerovat z kódu.
>
> Stav: ✅ runtime SR-konverze (lineární interpolace) je implementovaná; ⛔ kvalitní
> runtime SRC (poly­fázový/sinc downsampler) **není** — proto doporučení resamplovat offline.

---

## TL;DR

- **Engine běží na pevném výstupním SR** (default **48 000 Hz**), konfigurovatelném jen přes
  JSON (`audio_sample_rate` v `state.json`), v GUI read-only.
- **Sampl si nese vlastní SR** z WAV hlavičky (resp. packed indexu). Když se neshoduje
  s engine SR, engine ho **převádí za běhu** přehrávacím krokem `pos_inc = sample_sr / engine_sr`
  s **lineární interpolací**.
- **Při downsamplingu 96→48 (`pos_inc = 2.0`) je runtime cesta čistá decimace bez
  anti-alias filtru → aliasing + lehce zakalené výšky.** Lineární interpolace navíc `frac`
  drží na 0, takže fakticky bere každý druhý frame.
- **Doporučení: resamplovat na 48 kHz offline kvalitním nástrojem (ffmpeg soxr / sox VHQ)
  PŘED bakováním.** Pak je banka nativně 48 kHz, engine hraje `pos_inc = 1.0`, nula
  runtime konverze, nejlepší kvalita a ~poloviční RAM.
- **Transpoziční resampling** (dopočítat chybějící notu pitch-shiftem) je **záměrně
  vypnutý** — chybějící (nota, velocity) = ticho.

---

## 1. Výstupní (engine) sample rate

| Co | Kde | Detail |
|----|-----|--------|
| Default hodnota | `engine/engine.h:30` | `EngineConfig::sample_rate = 48000` |
| Getter pro GUI/render | `engine/engine.h:110` | `int sampleRate() const` |
| Konfigurace z JSONu | `app/gui/persistence.h:44`, `persistence.cpp:165,222` | `audio_sample_rate` v `state.json`, default 48000, **jen z JSONu / GUI read-only** |
| Validace + propsání do enginu | `app/gui/app_context.cpp:58,60` | `cfg.sample_rate = state.audio_sample_rate > 0 ? … : 48000` (nekladné → fallback 48000), validovaná hodnota se zapíše zpět do state |
| Otevření audio zařízení | `app/gui/app_context.cpp:115`, `app/cli/main.cpp:177` | zařízení (miniaudio) se otevírá na `cfg.sample_rate` |
| Propagace do DSP | `engine.cpp:73,481` | `DspChain::prepare(sr, block)` → odvozené koeficienty (`decay_coeff(tau, sr) = exp(-1/(tau·sr))`); přepočet při změně zařízení |

Engine SR je tedy **jeden globální parametr výstupu**, nezávislý na SR samplů. Konfiguruje se
mimo GUI (přímou editací `state.json`) — viz [config-file.md](config-file.md) (`audio_sample_rate`).

## 2. Sample rate samplů v bance

SR **není vlastnost banky, ale jednotlivého souboru** — engine nikde nepředpokládá jednotné SR:

| Formát | Odkud se SR bere | Citace |
|--------|------------------|--------|
| Adresářová banka (fixed/dynamic) | WAV hlavička per soubor (`peekWavInfo`) | `engine/io/wav_reader.cpp` (`peekWavInfo`), `sample_read.cpp:38` (`out.sample_rate = file.sample_rate`) |
| Packed `soundbank.ithaca` | per-entry pole v indexu | `engine/sample/ithaca_format.h:58` (`uint32_t sample_rate`); validace `sample_rate > 768000 → reject` v `ithaca_bank.cpp:86` |

- **`f##` token v názvu** (`m021-vel0-f48.wav`) je **jen poradní, ignorovaný** — rozhoduje
  hlavička/index. Mismatch tokenu a reality nemá žádný efekt.
- **Smíšené SR v bance jsou strukturálně podporované** (každý `MicLayer.file` nese své SR,
  `pos_inc` se počítá per hlas). Velocity řazení podle peak RMS je amplitudové přes ~50 ms
  **časové** okno → SR-neutrální, mixed SR řazení nerozbije. Baker měří analýzu per soubor
  z jeho vlastního SR (`tools/bake_soundbank.py:114` `head_frames_for`, `:217,:221`).

## 3. Runtime SR konverze (jak je implementovaná)

Když `sample_sr ≠ engine_sr`, engine převádí **přehrávacím krokem** + **lineární interpolací**.

### 3.1 Přehrávací krok `pos_inc`

```cpp
// engine/voice/voice.cpp:109-110
double sample_sr = mic_ ? (double)mic_->file.sample_rate : (double)engine_sr;
pos_inc_ = pitch_ratio * (sample_sr / (double)engine_sr);   // pitch_ratio je VŽDY 1.0
```

- `sample_sr == engine_sr` → `pos_inc = 1.0` (žádná konverze, nejlepší případ).
- 44.1 kHz na 48 kHz enginu → `pos_inc ≈ 0.91875` (upsampling).
- **96 kHz na 48 kHz enginu → `pos_inc = 2.0`** (downsampling 2:1).
- Rezonanční hlasy dělají totéž: `engine/voice/resonance_voice.cpp:41-42` (`pos_inc_ = sample_sr/engine_sr`), `:182` (`position_ += pos_inc_`). Rezonance není transponovaná (pitch_ratio implicitně 1.0).

### 3.2 Interpolace

| Region | Kód | Metoda |
|--------|-----|--------|
| Preload head (RAM) | `voice.cpp:198-204` | lineární interpolace mezi `p0` a `p0+1`, `frac = position_ - p0` |
| Streamed (ring) | `voice.cpp:205-206` + `streamed_reader.cpp` | lineární interpolace přes lo/hi okno readeru |
| Rezonance | `resonance_voice.cpp:184,193,253` | totéž (lin. interp v RAM i přes ring) |

**Lineární interpolace je jediná metoda. Žádný vyšší řád / sinc / anti-aliasing.**

### 3.3 Streamovaný reader a `pos_inc > 1`

Downsampling 2:1 znamená posun o 2 zdrojové framy na výstupní vzorek. Reader to zvládne
korektně — `advance(target)` je `while` smyčka, která popne tolik framů, kolik je potřeba:

```cpp
// engine/stream/streamed_reader.cpp:51-64
StreamedSampleReader::Advance StreamedSampleReader::advance(int64_t target) noexcept {
    while (ring_lo_idx_ < target) {        // při pos_inc=2.0 popne 2 framy / výstup
        ring_lo_l_ = ring_hi_l_; ring_lo_r_ = ring_hi_r_;
        float L, R;
        if (!popFrameRaw(L, R)) return Advance::RingEmpty;
        ring_hi_l_ = L; ring_hi_r_ = R;
        ring_lo_idx_++;
    }
    return Advance::Reached;
}
```

Funkčně tedy hraje správnou výšku i délku; jen ring **vyprazdňuje 2× rychleji** (viz §5).

## 4. Co se NEresampluje — transpozice / chybějící noty

`selectVoice` používá **výhradně nahraný sampl pro danou notu**, `pitch_ratio = 1.0`
(`engine/voice/patch_manager.cpp:39,66`). **Chybějící (nota, velocity) → ticho**, žádné
odvozování transpozicí (rozhodnutí 2026-05-30). Pitch-shift kód (transponuj nejbližší
nahranou notu) je odložený, **mimo build**, v `engine/voice/_reserved_resampling.h`.

> Pozor na terminologii: „resampling" zde znamená **dvě různé věci** — (a) *SR-konverze*
> samplu vs. výstup (§3, implementovaná) a (b) *transpoziční* resampling chybějící noty
> (§4, vypnutá). Tento dokument je hlavně o (a).

## 5. Kvalita: proč 96→48 za běhu aliasuje

Korektní downsampling 2:1 = **anti-alias low-pass (< 24 kHz) → decimace**. Runtime cesta
dělá jen druhý krok:

- Při `pos_inc = 2.0` startuje `position_` na 0.0 a přičítá přesně 2.0 → indexy 0, 2, 4, …
  → `frac` je **vždy 0** → lineární interpolace fakticky **bere každý druhý frame**.
  **Čistá decimace, nulový filtr.** Vše mezi 24–48 kHz se složí (fold) zpět do pásma.
- I při neceločíselném poměru je **lineární interpolace mizerný filtr** (trojúhelníkové okno):
  jemný rolloff → *ubírá výšky v pásmu* (droop k Nyquistu) a *špatně potlačuje* stopband.
  Takže oboje: aliasing + lehce „zakalené" výšky. Přesné 2:1 je **nejhorší případ**.
- U klavíru je energie nad 24 kHz nízká → aliasing bývá subtilní (lehký „fizz"/tvrdost na
  jasných notách, hůř při hodně hlasech), ale je to měřitelné a zbytečné.

## 6. Doporučený workflow — offline resample na 48 kHz před bakováním

**Vždy resampluj na 48 kHz offline kvalitním nástrojem a teprve to bakuj.** Pak je banka
nativně 48 kHz, engine hraje `pos_inc = 1.0` → žádná runtime konverze, nula aliasingu,
nejlepší kvalita, ~½ RAM oproti 96 kHz bance.

```sh
# ffmpeg + soxr (vysoká přesnost) — doporučeno
ffmpeg -i in_96k.wav -af "aresample=48000:resampler=soxr:precision=28" \
       -c:a pcm_f32le out_48k.wav

# sox (Very High Quality)
sox in_96k.wav -r 48000 out_48k.wav rate -v -s

# DAW: Reaper / Logic / Pro Tools "Resample" — všechny mají kvalitní SRC
```

- **Bitovou hloubku** drž float32 / 24-bit po celý řetězec; na 16-bit kvantuj (s ditherem)
  až úplně na konci, pokud vůbec. (Packed/WAV reader podporuje PCM16/24/32 i float32.)
- Pak `bash tools/make_dynamic_bank.sh …` → `python3 tools/bake_soundbank.py …`
  (viz [bank-format-packed.md](bank-format-packed.md)).

**Má smysl nahrávat v 96 kHz, i když finál je 48 kHz?** Pro capture ano: jemnější analogový
anti-alias filtr ADC, čistší pásmo u 20 kHz, a nelineární zpracování (saturace…) v 96 kHz
negeneruje aliasing, pokud finální downsampling uděláš pořádně. Pro čistě lineární záznam
klavíru je rozdíl marginální — ale jako capture formát obhajitelné, **pokud downsampling
neuděláš lineární interpolací enginu.**

## 7. Když přesto necháš banku v 96 kHz (následky za běhu)

| Formát | Chování @ 96 kHz / mixed SR |
|--------|------------------------------|
| **Extended** | Nenačte se vůbec (faze 7, prázdná banka) — `sample_store.cpp:369`. SR je bezpředmětné. |
| **Packed** | Hraje (`pos_inc=2.0`), ale: aliasing (§5), **~2× RAM**, **2× streaming**. SR validováno ≤ 768 kHz. |
| **Dynamic / fixed adresář** | Totéž jako packed (stejná `pos_inc` + lin. interp cesta). |
| **Smíšené SR (packed i adresář)** | Plně podporováno per-entry/per-soubor; každý sampl převeden zvlášť; RMS řazení OK; **bez varování**. |

**RAM ~2×:** vše se sizuje ve *zdrojových* framech. Head `preload_ms·SR` = 14400 framů @96k
místo 7200 @48k; rezonanční okno (default 12 s) ≈ **9,2 MB/notu @96k** místo ~4,6 MB. Celá
banka má ~dvojnásobnou stopu → dřív naráží na `cache_budget_mb` (default 2400) a **OOM guard
může banku načíst neúplnou** (`truncated`, ERROR). Při 96 kHz zvaž zvýšení `cache_budget_mb`.

> Pozn.: rozhodnutí **FullyLoaded vs. Streamed** je SR-invariantní v čase — práh
> `info.frames <= preload_frames * 2` (`sample_store.cpp:90-97`) odpovídá `2·preload_ms`
> (= 300 ms) bez ohledu na SR.

**Žádné varování:** engine nikde nehlásí `sample_sr ≠ engine_sr` ani mixed SR — spolkne to tiše.

## 8. Mapa implementace (file:line)

| Oblast | Soubor:řádek |
|--------|--------------|
| Default engine SR | `engine/engine.h:30`, getter `:110` |
| Konfigurace z `state.json` | `app/gui/persistence.{h:44,cpp:165,222}`, `app/gui/app_context.cpp:58,60` |
| SR samplu (adresář) | `engine/io/wav_reader.cpp` (`peekWavInfo`), `engine/io/sample_read.cpp:38` |
| SR samplu (packed) | `engine/sample/ithaca_format.h:58`, validace `ithaca_bank.cpp:86` |
| `pos_inc` (hlas) | `engine/voice/voice.cpp:109-110` |
| `pos_inc` (rezonance) | `engine/voice/resonance_voice.cpp:41-42,182` |
| Lin. interpolace | `voice.cpp:198-206`, `resonance_voice.cpp:184,193,253` |
| Streamovaný advance (`pos_inc>1`) | `engine/stream/streamed_reader.cpp:51-64` |
| Bez transpozice (chybějící nota → ticho) | `engine/voice/patch_manager.cpp:39,66` |
| Odložený pitch-shift | `engine/voice/_reserved_resampling.h` |
| FullyLoaded/Streamed práh | `engine/sample/sample_store.cpp:90-97` |
| Extended odmítnut | `engine/sample/sample_store.cpp:369` |
| Baker — analýza per SR | `tools/bake_soundbank.py:114,217,221` |
| IR resample (oddělená cesta) | `engine/dsp/ir_wav.{h,cpp}`, viz [G-dsp.md](reference/G-dsp.md) §IR |

## 9. Křížové odkazy

| Oblast | Vazba |
|--------|-------|
| **Loader** | [reference/F-loader.md](reference/F-loader.md) — kde se čte SR, FullyLoaded/Streamed, packed load path |
| **DSP / IR** | [reference/G-dsp.md](reference/G-dsp.md) — IR (`ir_wav`) resampluje IR na engine SR (vlastní cesta, nezávislá na samplech) |
| **Packed formát** | [bank-format-packed.md](bank-format-packed.md) — per-entry `sample_rate` v indexu, bake |
| **Dynamic formát** | [bank-format-proposed.md](bank-format-proposed.md) §8.3 — otevřená otázka „enforce single SR vs. mixed" |
| **Konfig** | [config-file.md](config-file.md) — `audio_sample_rate` (JSON-only) |

## 10. Otevřené otázky / možná budoucí práce

- **Vynutit jednotné SR banky?** Dnes mixed SR projde tiše. Volitelně warn/reject při mixu
  (otevřená otázka v [bank-format-proposed.md](bank-format-proposed.md) §8.3).
- **Kvalitní runtime SRC.** Pokud se má 96 kHz banka provozovat *přímo* (bez offline kroku),
  nahradit `pos_inc` lineární interpolaci poly­fázovým/sinc FIR downsamplerem ve `Voice`
  i `ResonanceVoice`. Větší zásah; zatím řešeno doporučením resamplovat offline (§6).
- **Varování na SR mismatch.** Levné: při loadu zalogovat, kolik samplů má `sample_rate ≠
  engine SR` (upozorní autora banky, že běží runtime konverze).
</content>
</invoke>
