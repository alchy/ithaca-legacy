# EOF safety declick fade (Voice + ResonanceVoice)

**Datum:** 2026-06-01
**Stav:** schváleno k implementaci
**Souvisí s:** streamed linear interpolation (merge 0eb9caf) — upravuje EOF větev tamtéž zavedenou.

## Problém

Po dokončení lineární interpolace dělá EOF větev streamovaného přehrávání
(ring prázdný + `ring_->eof_` flag) **hold-last-sample**: nastaví `hi = lo`,
inkrementuje `ring_lo_idx_`, a po dosažení `total_frames-1` deaktivuje hlas.
Drží tedy poslední (potenciálně nenulový) vzorek až do deaktivace → při
vypnutí vznikne DC step → lupnutí, zvlášť když streamovaný sampl končí na
nenulovém framu (oříznutý / neklavírní materiál).

Reference `icr` SamplerCore řeší přirozený konec jako „deaktivace + nula" bez
fade (spoléhá na to, že klavírní tail dozní k ~0). My zvolíme robustnější
variantu — **krátký declick fade do nuly** — protože streamovaný sampl u nás
nemusí na konci doznívat.

## Řešení

Na EOF spustit **stejný rychlý fade-to-zero mechanismus, jaký už používá
underrun** (`underrun_fading_`, doba `kUnderrunFadeMs` = 5 ms), místo
hold-last-sample. EOF i underrun tak sdílí jednu cestu „rychlý declick →
deaktivace". Liší se jen v logu (EOF je legitimní konec, underrun je chybový
stav, kdy data měla přijít a nepřišla).

### Voice (engine/voice/voice.cpp)

V ring větvi, v EOF případu (`popFrame` selhal A `ring_->eof_` je true),
nahradit současný hold-last-sample blok:

```cpp
} else if (ring_->eof_.load(std::memory_order_acquire)) {
    // EOF: clamp hi=lo (hold last sample), prestan posouvat.
    ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_;
    ring_lo_idx_++;
    if (ring_lo_idx_ >= (int64_t)total_frames - 1) {
        log_end("ring_eof_drained");
        active_ = false;
    }
    break;
}
```

za:

```cpp
} else if (ring_->eof_.load(std::memory_order_acquire)) {
    // EOF: ring dotekl + worker oznacil konec. Misto hold-last-sample
    // (DC step → lup) spustime rychly declick fade do nuly (stejny jako
    // underrun) a hlas dohaje. hi=lo clamp → fade klesa z posledni realne
    // hodnoty, ne ze skoku.
    ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_;
    if (!underrun_fading_) {
        underrun_fading_ = true;
        underrun_gain_   = 1.f;
        log_end("ring_eof_fade");
    }
    break;
}
```

Klíčové: **nezvyšujeme `ring_lo_idx_`** (necháme `frac` dopočítat z aktuální
pozice), a **nenastavujeme `active_ = false`** přímo — to udělá envelope blok
(ř. 300-309) po dosažení `underrun_gain_ <= 0`. Výstupní blok pak spočítá
`sL = ring_lo_l_*(1-frac) + ring_hi_l_*frac = ring_lo_l_` (hi=lo) a vynásobí
ho klesajícím `underrun_gain_`. Po dofadnutí `log_end("underrun_fade_zero")`
(existující) a deaktivace.

Pozn.: `log_end("ring_eof_fade")` se zaloguje jednou při startu fade (uvnitř
`if (!underrun_fading_)`), nikoli každý sample. `log_end("ring_eof_drained")`
zmizí (nahrazen). Hard guard (ř. 274-281) zůstává jako pojistka.

### ResonanceVoice (engine/voice/resonance_voice.cpp)

V ring větvi je dnešní EOF případ tichá deaktivace:

```cpp
} else if (ring_->eof_.load(std::memory_order_acquire)) {
    ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_;
    ring_lo_idx_++;
    if (ring_lo_idx_ >= (int64_t)total_frames - 1) {
        active_ = false;
    }
    break;
}
```

nahradit za:

```cpp
} else if (ring_->eof_.load(std::memory_order_acquire)) {
    // EOF: rychly declick fade do nuly (stejny jako underrun), pak deaktivace
    // v envelope bloku. hi=lo clamp → fade z posledni hodnoty.
    ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_;
    if (!underrun_fading_) {
        underrun_fading_ = true;
        underrun_gain_   = 1.f;
        LOG_RT_WARN("resonance_voice", "eof-fade midi=%d", midi_);
    }
    break;
}
```

ResonanceVoice už underrun fade aplikuje (resonance_voice.cpp:295-304:
`env *= underrun_gain_; underrun_gain_ += underrun_step_;` → deaktivace) a
`start()` nastavuje `underrun_step_` (resonance_voice.cpp:56), takže žádné
nové instalatérství netřeba.

## Co se NEMĚNÍ

- **Underrun** (ring prázdný, ale NE eof): beze změny — už fadne 5 ms.
- **Lineární interpolace, šev, 48k chování:** beze změny.
- **Hard guard** na `position_ >= total_frames`: ponechán jako pojistka.

## Testy (TDD)

1. **Voice EOF declick:** streamovaný sampl končící na NENULOVÉM framu
   (konstantní amplituda ~0.5, NE doznívající ramp), header SR 48000, délka
   tak, aby šel přes ring. Po EOF výstup musí **plynule klesnout k ~0** přes
   ~5 ms (žádný skok z ~0.5 na 0). Měřit: max |out[i]-out[i-1]| v koncové
   části < malá tolerance (ne ~0.5 skok); poslední nenulový vzorek malý;
   `activeVoices()==0`. RED (hold-last-sample) by ukázal náhlý skok / poslední
   vzorek ~0.5 následovaný 0.
2. **Voice clean-end regrese:** upravit stávající EOF case v
   `tests/test_streamed_interp.cpp` ("Streamed EOF: hold last sample") — teď
   hlas fadne k 0, takže původní `CHECK(last_nonzero > 0.5f)` už neplatí.
   Nahradit kontrolou, že hlas se deaktivoval (`activeVoices()==0`) a dohrál
   podstatnou část (peak přes celý běh > 0.6 — dosáhli jsme vrcholu rampy
   před fade). Přejmenovat case (už to není "hold last sample").
3. **ResonanceVoice EOF:** v `tests/test_resonance_stream_sr.cpp` ověřit, že
   streamovaná rezonance po EOF čistě dohaje (deaktivace v rozpočtu) — pokud
   stávající checky stačí, jen potvrdit; jinak přidat declick kontrolu.
4. **Plná sada 24/24** (resp. počet po úpravách) zelená.

## Mimo rozsah (YAGNI)

- Konfigurovatelná délka EOF fade (5 ms `kUnderrunFadeMs` stačí).
- Anti-aliasing pro downsampling (samostatná fáze).
- Změna underrun chování.
