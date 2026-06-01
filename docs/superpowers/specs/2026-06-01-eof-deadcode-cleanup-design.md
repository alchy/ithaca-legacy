# EOF dead-code cleanup + clean-end vs underrun log distinction

**Datum:** 2026-06-01
**Stav:** schváleno k implementaci
**Nahrazuje:** dřívější EOF declick spec/plán (premisa „hold last sample → lup" neplatí — viz níže).

## Pozadí (proč nahrazujeme původní EOF declick záměr)

Investigace ukázala, že EOF větev v `Voice::process` je **nedosažitelná dead
code**:
- Stream worker nastaví `ring_->eof_` pouze při short-read nebo zero-read
  (`stream_engine.cpp:199-203, 219-228`).
- Voice ale capuje **každý** read request na přesný konec souboru
  (`voice.cpp:338-346`: `if (want > remain) want = remain;`), takže worker
  vždy dorучí přesně tolik, kolik bylo žádáno → nikdy short-read → `eof_` se
  pro Voice nikdy nenastaví.
- Důsledek: čistý konec streamovaného vzorku **vždy** projde **underrun**
  cestou (ring se vyprázdní, `popFrame` selže s `eof_`==false), spustí 5 ms
  fade a deaktivuje se s `reason=underrun_fade_zero`, logováno jako
  `WARNING "UNDERRUN"`.

Zvuk je v pořádku (5 ms fade, žádný lup), takže původní záměr „hold last
sample → declick fade" řešil neexistující problém. Co JE problém: (1) mrtvý
kód EOF větve + hard guardu ve Voice, a (2) matoucí log — čistý konec
hlášený jako underrun warning.

`ResonanceVoice` je jiný případ: jeho EOF větev JE dosažitelná přes start
path `total_after <= 0` (`resonance_voice.cpp:74-78` nastaví
`ring_->eof_.store(true)` přímo), takže její EOF větev **NEmažeme**.

## Cíl

1. Smazat nedosažitelnou EOF větev a hard guard ve `Voice::process`.
2. V underrun cestě rozlišit **čistý konec vzorku** od **skutečného
   underrunu** a logovat je odlišně (čistý konec = Info, underrun = Warning).
   Zvukové chování (5 ms fade) zůstává u obou stejné.

## Řešení

### A) Smazat dead code ve `Voice::process` (engine/voice/voice.cpp)

V ring branch smazat celý `else if (ring_->eof_.load(std::memory_order_acquire))`
blok (~ř. 232-241 dnešního stavu — clamp hi=lo, inkrement, `log_end("ring_eof_drained")`).
Po smazání má smyčka posunu okna jen dvě cesty: `popFrame` OK → posun; `popFrame`
selhal → nastavit `underrun = true; break;` (sjednoceno — už není potřeba
rozlišovat eof vs non-eof uvnitř smyčky, protože eof_ je u Voice vždy false).

Smazat i hard guard pod ring branch (~ř. 274-281: `if ((int)position_ >=
total_frames && ring_ && ring_->eof_.load(...) && ring_->available()==0)` +
`log_end("hard_guard_file_end")`) — vyžaduje `eof_`, tedy mrtvý.

`log_end("ring_eof_drained")` a `log_end("hard_guard_file_end")` tím zmizí.

### B) Rozlišit clean-end od underrunu (engine/voice/voice.cpp)

V místě, kde `popFrame` selže (dnešní underrun blok ~ř. 248-258), rozhodnout
podle toho, zda už byl vyžádán celý soubor:

```cpp
if (underrun) {
    if (!underrun_fading_) {
        underrun_fading_ = true;
        underrun_gain_   = 1.f;
        // Cisty konec vzorku vs. skutecny underrun: pokud uz byl cely soubor
        // vyzadan (file_request_off_ dosahl konce) a ring je prazdny, je to
        // legitimni konec — NE chyba. Jinak worker nestihl dodat data.
        const bool clean_end = (file_request_off_ >= (int64_t)total_frames);
        if (clean_end) {
            log::Logger::default_().log("voice_end", log::Severity::Info,
                "END-OF-SAMPLE midi=%d pos=%lld total=%d", midi_,
                (long long)position_, total_frames);
        } else {
            log::Logger::default_().log("voice_end", log::Severity::Warning,
                "UNDERRUN midi=%d pos=%lld total=%d head=%d ring_avail=%d",
                midi_, (long long)position_, total_frames,
                head_frames, ring_->available());
        }
    }
    sL = 0.f; sR = 0.f;
}
```

Finální deaktivace po dofadnutí zůstává `log_end("underrun_fade_zero")` (jeden
společný — fade je stejný pro oba). Tj. diskriminace je u STARTU fade
(severity Info vs Warning), ne u deaktivace. `file_request_off_` je už
existující member sledující, kam až Voice požádal worker o čtení.

### C) ResonanceVoice (engine/voice/resonance_voice.cpp)

EOF větev (`else if (ring_->eof_.load(...))`, ~ř. 216-222) **PONECHAT** — je
dosažitelná přes `total_after<=0` start path. Underrun warning tam (`LOG_RT_WARN`,
~ř. 229-234) ponechat beze změny — u ResonanceVoice je underrun přes ring vždy
skutečný (clean end jde přes EOF větev), takže rozlišení netřeba. Žádná změna.

## Co se NEMĚNÍ

- Audio chování: 5 ms fade na konci/underrunu beze změny.
- Lineární interpolace, šev, 48k.
- `stream_engine` eof_ semantika (oživení EOF větve = zamítnutá volba 1).
- ResonanceVoice render.

## Testy (TDD)

1. **Clean-end → Info, ne Warning:** streamovaný sampl (ramp/konstanta) dohraje
   do konce. Přes existující log subscriber (`test_log_subscriber` ukazuje API:
   `Logger::default_().addSubscriber([](const LogEntry&){...})`) zachytit log
   eventy, ověřit, že na konci přišel event s `topic=="voice_end"`,
   `sev==Info` a zprávou obsahující `"END-OF-SAMPLE"`, a že NEpřišel
   `Severity::Warning` s `"UNDERRUN"`. Plus `activeVoices()==0`.
2. **Žádná regrese:** plná sada (24, resp. aktuální count) zelená; stávající
   streamed testy (`test_streamed_interp`, `test_long_sample_stream`,
   `test_resonance_stream_sr`) projdou beze změny.
3. **Skutečný underrun → Warning:** HONEST GAP — deterministicky vyrobit
   underrun (hladový worker) je v unit testu nespolehlivé. Necháme jako
   code-review kontrolu diskriminátoru (`file_request_off_ >= total_frames`),
   netestujeme automaticky. Dokumentovat v plánu.

## Mimo rozsah (YAGNI)

- DSP chain (AGC/convolver/BBE/limiter) — samostatná feature příště.
- Oživení EOF větve / změna stream_engine.
