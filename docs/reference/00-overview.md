# ithaca — Programová referenční dokumentace

Tento adresář (`docs/reference/`) popisuje implementaci enginu po **oblastech
kontextu**. Každá oblast = jeden dokument: říká, které soubory ji implementují,
a pro každý soubor jeho funkce/metody (vstup/výstup, kdo volá, koho volá a proč,
parametry, vlákno, vysvětlení). Cíl: pochopit návrh i implementaci bez čtení
celého zdrojáku.

> Stav: generováno po oblastech (viz tabulka). Každý dokument navíc obsahuje
> sekci **Nálezy revize** — při sepisování proběhla i kontrola logiky návrhu vs
> implementace (kód se nemění, jen se zaznamenají případné nesrovnalosti).

## Tok signálu (high-level)

```
                    [GUI / hlavní vlákno]                    [MIDI callback vlákno]
                    panely → ctx.state → engine settery       MidiInput → noteOn/CC
                          (atomiky)                                 │
                                                                    ▼
                                                            MidiQueue (lock-free SPSC)
                                                                    │
  [audio vlákno] Engine::processBlock(out_l, out_r, n):             │
    1. drain MidiQueue  ◀───────────────────────────────────────────┘
       → PedalState, VoicePool.noteOn/Off, ResonanceEngine
    2. VoicePool.processBlock      → součet hlavních hlasů  ┐
    3. ResonanceEngine.processBlock→ + sympatická rezonance ├─ čte z RAM head +
    4. × master_gain                                        │   streaming ringů
    5. DspChain.process (AGC→BBE→Limiter)                   │   (stream workeři)
    6. peak metr (atomic)                                   ┘
       → interleave → AudioDevice → zvuk

  [loader, off-RT] scanBank → loadBank → Bank (RAM head + resonance okno);
                   stream workeři dolévají zbytek z disku do ringů.
```

## Oblasti

| # | Oblast | Dokument | Soubory (primárně) |
|---|--------|----------|--------------------|
| A | Core / scaffold | [A-core.md](A-core.md) | `engine/engine.{h,cpp}`, `util/log.{h,cpp}`, `util/version.h`, `app/cli/main.cpp`, `render/batch_renderer.{h,cpp}` |
| B | Zpracování eventů | [B-events.md](B-events.md) | `midi/midi_input.{h,cpp}`, `midi/midi_queue.h`, `pedal/pedal_state.{h,cpp}` |
| C | Zpracování bufferu | [C-buffers.md](C-buffers.md) | `io/audio_device.{h,cpp}`, `stream/stream_engine.{h,cpp}` |
| D | Polyfonie | [D-polyphony.md](D-polyphony.md) | `voice/voice.{h,cpp}`, `voice_pool.{h,cpp}`, `patch_manager.{h,cpp}` |
| E | Rezonance | [E-resonance.md](E-resonance.md) | `resonance/resonance_engine.{h,cpp}`, `harmonic_proximity.{h,cpp}`, `voice/resonance_voice.{h,cpp}` |
| F | Loader | [F-loader.md](F-loader.md) | `sample/bank_index.{h,cpp}`, `sample_loader.{h,cpp}`, `sample_store.{h,cpp}`, `sample_types.h`, `io/wav_reader.{h,cpp}`, `io/wav_writer.{h,cpp}` |
| G | DSP | [G-dsp.md](G-dsp.md) | `dsp/dsp_stage.h`, `dsp_math.h`, `dsp_chain.{h,cpp}`, `agc.{h,cpp}`, `bbe.{h,cpp}`, `limiter.{h,cpp}` |
| H | GUI | [H-gui.md](H-gui.md) | `app/gui/main.cpp`, `app_context.{h,cpp}`, `persistence.{h,cpp}`, `log_subscriber.{h,cpp}`, `voice_page.h`, `theme.h`, `layout.h`, `widgets.h`, `panel_*.{h,cpp}` |
| I | Multithreading | [I-multithreading.md](I-multithreading.md) | *cross-cutting* — vlákna + synchronizace napříč A–H |

## Konvence dokumentů

- **Jazyk:** čeština.
- **Granularita:** triviální/zřejmé funkce (gettery, jednoduché obaly) shrnuté 1–2
  větami; rozsáhlejší logika popsaná detailně.
- **Per soubor** tabulka funkcí:
  `funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení`.
- **Vlákno** = na kterém threadu se funkce typicky volá (audio / GUI / MIDI / worker / off-RT).
- **Nálezy revize** na konci každého dokumentu (kontrola logiky, bez změn kódu).

Pozn.: `engine/voice/_reserved_resampling.h` je rezervovaný (zatím nepoužitý) kód —
zmíněn v Polyfonii jen jako poznámka.
