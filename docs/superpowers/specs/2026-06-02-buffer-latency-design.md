# Runtime Buffer Size + DSP Load / Latency Meter — Design

**Goal:** (A) Měřit dobu zpracování audio bloku vs délka okna (deadline) a zobrazit v GUI jako „DSP LOAD %" s červeným blikáním při přešvihu okna. (B) Umožnit za běhu měnit velikost audio bufferu (32–8192 framů) selektorem v horní liště — mění latenci i DSP load.

**Proč:** Detekujeme rezonanční underruny (I/O hladovění workerů), ale ne **výpočetní** deadline audio vlákna. A standalone app neumí měnit buffer (latence). Core je per-call buffer-agnostický (host/JUCE řídí buffer až 8 kB) — metr i engine to musí zvládat.

**Architektura:** Dvě ortogonální veličiny měřené každá na své hranici: (1) latence = wall-time `Engine::processBlock` vs perioda `n_samples/sr` (audio vlákno, single → bez contention; workery do toho nevstupují, jejich selhání = underrun, už máme). (2) buffer = `Engine::setBlockSize` (už existuje) + restart `AudioDevice` z GUI + persistence. Atomiky + GUI flash ve stejném vzoru jako master peak / per-pool underrun.

---

## A) DSP load / latency metr

**Engine (`engine.{h,cpp}`)** — v `processBlock` obal render práce:
- `t0 = nowMicros()` na začátku reálné práce (po `bank_loading_` guardu), `dt = nowMicros() - t0` na konci.
- `period_us = (uint64_t)n_samples * 1'000'000 / cfg_.sample_rate`; `load = (float)dt / period_us`.
- Atomiky (psané audio vláknem, čtené GUI — relaxed):
  - `std::atomic<float> dsp_load_peak_{0}` — peak-hold: `peak = max(load, peak * decay)`, `decay = exp(-n_samples/(0.5f*sr))` (tau ~0.5 s, čitelné).
  - `std::atomic<uint64_t> last_overload_us_{0}` — orazítkuj když `load >= 1.0f`.
- Gettery: `float dspLoadPeak() const`, `bool overloadRecent(float ms) const` (vzor `noteOnRecent`/`underrunRecent`).
- Perioda z `n_samples` → metr se přizpůsobí jakémukoli bufferu, i když ho mění host (JUCE).

**GUI (`panel_topbar.cpp`)** — vedle BUFFER selektoru (viz B): `DSP <peak%>`; číslo **červené** když `overloadRecent(4000)` (load ≥ 1.0 = minul deadline), jinak `muted`. **Jen červená, žádná žlutá.**

## B) Runtime buffer selektor + read-only SAMPLE RATE

**GUI topbar (`panel_topbar.cpp`)** — mezi `CH []` a (vpravo) `LOG []`:
- **`SR`** read-only label — `engine.sampleRate()` zformátovaný (např. „48 kHz"). NENÍ editovatelný v GUI; mění se POUZE v `state.json` (`audio_sample_rate`) a aplikuje se při startu. Engine SR je naše/device volba (request do miniaudio, které dorovná na nativní HW rate), NE bank-derived — samply se sladí per-voice resamplingem (`pos_inc_ = pitch_ratio·sample_sr/engine_sr`, voice.cpp:129), takže banka SR nediktuje. Protože se SR za běhu nemění, NEPOTŘEBUJEME `engine.setSampleRate()` ani re-prepare na SR.
- Label **`BUFFER`** + combo s hodnotami `{32, 64, 128, 256, 512, 1024, 2048, 4096, 8192}` framů; vedle latence v ms.
- **ms vždy z aktuální SR:** `frames * 1000.0f / engine.sampleRate()` (live getter `cfg_.sample_rate`, engine.h:84) — NIKDY ne z literálu 48000. Stejný zdroj pravdy jako DSP LOAD perioda (`n_samples/cfg_.sample_rate`). Pozn.: dnes SR za běhu nikdo nemění (standalone fixně 48000); až host (JUCE `prepareToPlay`) změní SR, engine bude potřebovat `setSampleRate`/`prepare(sr,...)` entrypoint aktualizující `cfg_.sample_rate` (analogicky `setBlockSize`) — getter to do GUI propíše sám, topbar se nemění. To je FUTURE, mimo rozsah.
- Na změnu (GUI vlákno, v `AppContext` helper `setAudioBlockSize(int)`):
  1. `audio->stop()` (joinne audio callback),
  2. `engine.setBlockSize(n)` (clamp 32..8192, přepočet refill prahu, re-prepare DSP + rezonance),
  3. `audio->start(&audioCallback, &engine, sample_rate, n)`,
  4. `state.audio_block_size = n` (persist).
  - Krátký audio gap při přepnutí je OK (uživatelská akce).

**Persistence (`persistence.{h,cpp}`)** — nová pole `int audio_block_size = 256;` a `int audio_sample_rate = 48000;` (save + obranné load, jako DSP pole; schema zůstává v4). `AppContext::initFromState` použije `state.audio_block_size` + `state.audio_sample_rate` pro `cfg.block_size`/`cfg.sample_rate` i pro `audio->start`. `audio_block_size` se mění z GUI (debounce/uložení po změně v `main.cpp`); `audio_sample_rate` se mění jen ručně v JSONu (GUI ho jen zobrazuje read-only).

**Vztah k host/JUCE:** `Engine::processBlock(out_l,out_r,n)` už bere `n` per-call → v plugin buildu řídí buffer host (až 8192) a metr (perioda z `n`) funguje i tam. Selektor je jen pro standalone `AudioDevice`. `ring_capacity_frames` (8192) ≥ max blok (8192) → konzistentní; větší buffer streamování ulehčí (víc času na refill).

---

## Edge cases
- Buffer 32: velmi nízká latence (~0.7 ms), vyšší riziko overloadu → metr to ukáže (to je smysl pro testování).
- `setBlockSize` clampuje na [32, 8192]; combo hodnoty v tom rozsahu.
- Při `bank_loading_` (silence) se load neaktualizuje (drží poslední, decay) — bez vlivu.
- Restart device selže? `audio->start` vrací false → log warning, zkusit vrátit původní buffer; GUI combo odráží skutečný `engine.blockSize()`.

## Testy
- `overloadRecent` timestamp okno (jako `underrunRecent`) — doctest v `test_dsp.cpp` nebo `test_stream_engine.cpp` (Engine getter; init bez banky).
- `setBlockSize` clamp už pokryto/triviální; přidat check že `Engine::blockSize()` odráží nastavené.
- GUI + restart device = smoke (manuální: měnit buffer, sledovat DSP LOAD %, ověřit zvuk pokračuje).

## Soubory
- Modify: `engine/engine.h` (atomiky + gettery), `engine/engine.cpp` (timing v `processBlock`).
- Modify: `app/gui/app_context.{h,cpp}` (`setAudioBlockSize` helper + init z `audio_block_size`).
- Modify: `app/gui/persistence.{h,cpp}` (pole `audio_block_size`).
- Modify: `app/gui/panel_topbar.cpp` (BUFFER combo + DSP LOAD readout).
- Modify: `app/gui/main.cpp` (debounce change-detekce `audio_block_size`).
- Modify: `tests/test_dsp.cpp` (overloadRecent / blockSize getter).

## Mimo rozsah
- Měření i v `AudioDevice` callbacku (celkový deadline vč. interleave) — v1 stačí engine render load.
- Automatické přizpůsobení bufferu dle zátěže — ne.
