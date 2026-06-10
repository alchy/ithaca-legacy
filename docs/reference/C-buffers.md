# Zpracování bufferu

Audio výstupní cesta: miniaudio (`AudioDevice`) volá na audio threadu registrovaný callback, který zavolá `Engine::processBlock` — ta renderuje do dočasných L/R scratch bufferů a caller (callback v `app_context.cpp` resp. v `main.cpp`) výsledek interleave do float32 výstupního bufferu miniaudio. Disk I/O se na audio threadu nikdy neodehrává: Voice čte výhradně z RAM (SPSC ring `RingHandle`), který asynchronně plní `StreamEngine` z worker threadů. `StreamEngine` existuje ve dvou instancích: `stream_main_` pro `VoicePool` (main sample voices) a `stream_resonance_` pro `ResonanceEngine` (viz oblast A — Core / Multithreading). Každá instance drží pool ringů, SP-MC request frontu a pool worker threadů; Voice žádá refill přes `requestRead` (lock-free push, drop-on-full = RT-safe; vrací `bool` — při plné frontě volající NEPOSOUVÁ svůj souborový offset a request se příští blok přirozeně zopakuje), worker přijímá přes mutex-serializovaný pop. Recyklaci ringu při steal/retriggeru s in-flight requestem chrání generační čítač `gen_` + producer try-lock `producers_` (viz tabulky níže). Konzumentskou stranu — vlastnictví ringu, lo/hi interpolační okno, refill heuristiku i bulk čtení (1 acquire + 1 release za blok místo 3 atomik/vzorek) — zapouzdřuje sdílený `StreamedSampleReader` (`engine/stream/streamed_reader.{h,cpp}`), který kompozicí používají `Voice` i `ResonanceVoice`; POLICY při prázdném ringu zůstává v hlasech (viz tabulka níže).

---

## Implementováno v souborech

| soubor | odpovědnost | klíčové typy |
|---|---|---|
| `engine/io/audio_device.h` | Deklarace miniaudio playback wrapperu, definice `AudioCallback` typedef | `AudioDevice`, `AudioCallback` |
| `engine/io/audio_device.cpp` | Inicializace `ma_device` (f32, stereo, periodSizeInFrames), C-trampoline `ma_data_callback` → `invokeCallback` | `ma_device`, trampolina `ma_data_callback` |
| `engine/stream/stream_engine.h` | Deklarace SPSC ringu (`RingHandle`), SP-MC fronty (`StreamRequestQueue`), streaming engine (`StreamEngine`) | `RingHandle`, `StreamRequest`, `StreamRequestQueue`, `StreamEngine` |
| `engine/stream/stream_engine.cpp` | Implementace ring push/pop, worker loop (čtení WAV po chuncích), acquire/release pool, underrun timestamp | `workerLoop`, `readWavRange`, `nowMicrosSE` |
| `engine/stream/streamed_reader.h` | Deklarace sdíleného streaming čtece pro `Voice` a `ResonanceVoice`: vlastnictví ringu, lo/hi interpolační okno, EOF-hold primitivy, refill heuristika, bulk čtení, diagnostika | `StreamedSampleReader`, `StreamedSampleReader::Advance` |
| `engine/stream/streamed_reader.cpp` | Implementace — kód přenesen 1:1 z `Voice`/`ResonanceVoice` (bit-exact extrakce, hlídá `tests/test_render_regression.cpp`) | `StreamedSampleReader` (plná implementace) |

---

## `engine/io/audio_device.h`

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `AudioDevice()` | off-RT (init) | — | `app_context.cpp`, `main.cpp` | — | — | Trivální konstruktor; žádná alokace. `ma_device*` se inicializuje na `nullptr`. |
| `~AudioDevice()` | off-RT (teardown) | — | destruktor | `stop()` | — | Garantuje zastavení device před destrukcí objektu. |
| `bool start(AudioCallback cb, void* userdata, int sample_rate, int block_size)` | off-RT (init) | callback + SR + block_size → `bool` | `app_context.cpp`:`AppContext::init`, `main.cpp` | `ma_device_config_init`, `ma_device_init`, `ma_device_start` | `cb` = funkce plnící interleaved stereo f32 výstup; `userdata` = neprůhledný ukazatel předaný zpět do callbacku (zpravidla `Engine*`); `sample_rate` = vzorkovací frekvence; `block_size` = `periodSizeInFrames` | Alokuje `ma_device` na haldě, nakonfiguruje ho na `ma_format_f32`, 2 kanály, předaný SR a block size. `pUserData = this` (ne `userdata`) — device drží ukazatel na `AudioDevice`, trampoline si přes něj dostane k `callback_`. Zaloguje jméno device při úspěchu; vrací `false` při selhání `ma_device_init` nebo `ma_device_start`. Nastaví `running_ = true`. |
| `void stop()` | off-RT | — | `~AudioDevice`, explicitně | `ma_device_uninit` | — | Uninitializuje a dealokuje `ma_device`. Nastaví `running_ = false`. Idempotentní (guard na `device_ != nullptr`). |
| `bool isRunning() const` | libovolné | — → `bool` | GUI diagnostika | — | — | Atomický snapshot `running_`. |
| `const std::string& deviceName() const` | libovolné | — → název | GUI diagnostika | — | — | Vrací `device_->playback.name` uložený při `start()`. |
| `void invokeCallback(float* output, uint32_t frames)` | **audio thread** | `output` buffer, `frames` | `ma_data_callback` (trampoline) | `callback_(userdata_, output, frames)` | `output` = interleaved stereo f32 buffer alokovaný miniaudio; `frames` = počet stereo framů | Veřejná pouze kvůli trampolině (C-callback nemůže být memberem). Deleguje na uložený `callback_`; pokud je `nullptr`, nic nedělá. |

---

## `engine/io/audio_device.cpp`

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `static void ma_data_callback(ma_device* dev, void* output, const void* /*input*/, ma_uint32 frame_count)` | **audio thread** | raw `ma_device` callback → vyplněný `output` buffer | miniaudio interně (za každý period) | `self->invokeCallback(...)` | `dev` = miniaudio device; `output` = interleaved stereo f32 buffer (přetypujeme na `float*` — bezpečné dle konfigurace); `frame_count` = počet framů k vyplnění | C-trampoline: z `dev->pUserData` extrahuje `AudioDevice*`, zavolá `invokeCallback`. Vstupní kanál (`input`) se ignoruje (playback-only device). Přetypování na `float*` je bezpečné, protože device je nakonfigurován jako `ma_format_f32`, 2 kanály. |

---

## Vzorkovací frekvence (SR) — kdo ji určuje a role miniaudio

Engine **nepřebírá SR z banky**. SR je naše (standalone) nebo hostitelská (plugin) volba:

- **Standalone:** `audio_sample_rate` ze `state.json` (default 48000) → `EngineConfig::sample_rate` → request do miniaudio (`config.sampleRate` v `AudioDevice::start`). Mění se **jen ručně v JSONu**, aplikuje se při startu; GUI ho jen zobrazuje read-only (topbar `SR`). Žádný runtime SR setter neexistuje (ani ho nepotřebujeme, dokud SR nemění host).
- **Plugin / JUCE (future):** SR diktuje host přes `prepareToPlay`; `Engine::processBlock(out_l, out_r, n)` je per-call buffer-agnostický, takže funguje beze změny. Tehdy bude engine potřebovat `setSampleRate`/`prepare(sr,...)` entrypoint aktualizující `cfg_.sample_rate` (analogicky `setBlockSize`).

**Co dělá miniaudio se SR:** `config.sampleRate` je rychlost, na které běží náš `dataCallback` (a tím i `processBlock`/engine). miniaudio **interně resampluje** mezi nativní rychlostí HW (`device_->playback.internalSampleRate`) a naším požadavkem — máme-li request 48000 a HW jede 44100, callback dostává 48000 framů a miniaudio je převede na 44100 pro výstup. Důsledek: `engine.sampleRate()` (= `cfg_.sample_rate`) je **autoritativní pravda** pro veškerou SR-závislou matematiku (ms latence na liště, DSP load perioda, onset/release/`pos_inc`). Pozn.: kdyby byl request 0, miniaudio by použil nativní rate a museli bychom přečíst `device_->sampleRate` zpět do enginu — proto vždy posíláme explicitní nenulovou hodnotu.

**Sladění SR banky vs engine — per-voice resampling:** každý sample si nese vlastní nativní SR z WAV hlavičky (`mic_->file.sample_rate`). `Voice::start` (`voice.cpp`) počítá krok čtení `pos_inc_ = pitch_ratio * (sample_sr / engine_sr)` a Voice čte lineární interpolací. Banka tedy SR **nediktuje** — může být nahraná na jiné SR (i smíšeně napříč samply) a engine ji ke svému SR dorovná za běhu (výška/tempo zůstanou správné). Viz oblast D — Polyfonie.

---

## Runtime změna bufferu + DSP load metr (Fáze 8)

**BUFFER selektor (GUI topbar):** `audio_block_size` ze `state.json` (default 256), runtime měnitelný comboem `{32, 64, 128, 256, 512, 1024, 2048, 4096, 8192}`. Změna jde přes `AppContext::setAudioBlockSize(int)`:
1. `audio->stop()` **PRVNÍ** (joinne miniaudio callback) — pořadí je kritické: jinak by krok 2 mutoval engine stav (DSP koeficienty, rezonance decay, refill práh) souběžně s in-flight `processBlock` na audio threadu = data race.
2. `engine.setBlockSize(n)` — clamp [32, 8192], `recomputeRefillThreshold()` obou `StreamEngine` instancí, re-prepare DSP + rezonance decay.
3. `audio->start(&audioCallback, &engine, engine.sampleRate(), n)` — restart device s aktuálním (nezměněným) SR.
4. persist `audio_block_size` (debounced save v `main.cpp`).

Krátký audio gap při přepnutí je očekávaný (uživatelská akce). Hodnoty se validují i při initu — `initFromState` clampuje block na [32, 8192] a SR padá na fallback 48000 při neplatném (≤ 0) JSON. `ring_capacity_frames` (8192) ≥ max blok (8192) → konzistentní; větší buffer streamování ulehčí (víc času na refill).

**DSP load metr (`Engine::processBlock`, audio thread):** na konci bloku se změří `dt = render_us` (vůči `block_t0` z `nowMicros()` po `bank_loading_` guardu), `period_us = n_samples*1e6 / sample_rate`, `load = dt / period`. Peak-hold s decay ~0.5 s se ukládá do atomiky `dsp_load_peak_`; při `load >= 1.0` (blok minul deadline → riziko dropoutu) se orazítkuje `last_overload_us_`. Gettery `Engine::dspLoadPeak()` a `Engine::overloadRecent(ms)` (vzor master peak / `noteOnRecent`, relaxed atomiky — single-writer audio, reader GUI). GUI je zobrazuje v **indicator stripu** jako 5. dlaždici `DSP LOAD` (`<peak%>`, vedle VOICES/RESONANCE/MAIN RINGS/RESO RINGS, viz oblast H — GUI); číslo **červeně** (ring_red) při `overloadRecent(4000)` (jen červená, žádná žlutá). BUFFER selektor v topbaru ukazuje jen počet framů (ms latence `frames*1000/SR` se kvůli místu na liště nezobrazuje, ale platí jako koncept). Perioda počítaná z `n_samples` → metr se přizpůsobí jakémukoli bufferu, i když ho řídí host.

---

## `engine/stream/stream_engine.h`

### `RingHandle` — SPSC ring buffer pro jeden hlas

| funkce / člen (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `std::vector<float> buf` | — | — | — | — | Kapacita = `capacity_frames * 2` floatů (interleaved stereo) | Jednou alokováno v `StreamEngine` konstruktoru; nikdy se neralokuje. |
| `std::atomic<size_t> w_` | producent (worker) | — | — | — | Monotónně rostoucí write cursor | Zapisuje výhradně worker thread. Čte audio thread (v `popFrame`, `available`). Memory order: store `release`, load `acquire` na straně konzumenta. |
| `std::atomic<size_t> r_` | konzument (audio thread) | — | — | — | Monotónně rostoucí read cursor | Zapisuje výhradně audio thread. Čte worker (v `push`). Memory order: store `release`, load `acquire` na straně producenta. |
| `std::atomic<bool> eof_` | worker → audio | — | — | — | EOF příznak | Worker nastaví na `true` (memory_order_release) po skončení posledního chunku, pokud byl request podán s `eof_when_done=true` a gen snapshot stále platí. `StreamedSampleReader::beginEofOnly` (volá `ResonanceVoice::start`) ho nastavuje i přímo (případ `total_after <= 0` — za rezonančním oknem už není co streamovat). `Voice` ho čte jen diagnosticky (log, `eofRelaxed`) — čistý konec rozhoduje přes `cleanEnd()` (`file_request_off_ >= total`) a doznívá stejným 5ms fade jako underrun (liší se jen Info/Warning log). `ResonanceVoice` `eof_` čte přes `eofAcquire()` pro hold-last-sample / ukončení hlasu. |
| `std::atomic<bool> in_use_` | pool allocator | — | — | — | Alokační flag | CAS v `acquireRing` (false→true, `acquire`); store false v `releaseRing` (`release`). |
| `std::atomic<uint32_t> gen_` | pool allocator / worker | — | — | — | Generace vlastnictví (ABA guard) | `releaseRing` dělá `fetch_add` (acq_rel) — zneplatní všechny in-flight requesty. `StreamRequest` nese snapshot `gen`; worker ověřuje shodu před každým chunk push i před `eof_` store. Stale request (po releaseRing) tak nikdy nezapíše data/EOF do ringu nového vlastníka. |
| `std::atomic<int> producers_` | worker / acquireRing | — | — | — | Producer try-lock (0/1) | Do ringu smí zapisovat v daném okamžiku max jeden worker; lock se drží JEN přes push (memcpy), ne přes disk I/O (čekání max ~desítky µs). `acquireRing` na něj před resetem kurzorů krátce (bounded spin) počká — kryje zbytkové okno, kdy gen bump přišel během probíhajícího memcpy. |
| `int push(const float* src_interleaved, int n_frames)` | **stream worker** | interleaved stereo float pole, počet framů → počet skutečně zapsaných | `workerLoop` | `memcpy` (1–2 segmenty při wrapu) | `src_interleaved` = zdrojová data z `readWavRange`; `n_frames` = kolik framů zapsat | Výpočet volného místa: `free_frames = capacity_frames − (w_ − r_)`. Zápis zvládá přechod přes konec bufferu (wrap) dvěma `memcpy` volání. Store `w_` s `memory_order_release`. Vrací skutečně zapsaný počet (≤ `n_frames`); při plném ringu vrátí 0. Bez alokací. |
| `bool popFrame(float& L, float& R) noexcept` | **audio thread** | — → jeden stereo frame (L, R) nebo `false` | `StreamedSampleReader::popFrameRaw` (mimo otevřený bulk blok), testy | — | `L`, `R` = výstupní reference | Load `w_` s `memory_order_acquire`, load `r_` s `memory_order_relaxed`. Pokud `r_ >= w_` (prázdný ring), vrátí `false`. Jinak přečte frame na pozici `r_ % capacity_frames`, store `r_+1` s `memory_order_release`. Jeden frame = 2 floaty z `buf`. Render smyčky hlasů čtou ring bulk režimem (viz `snapshotW`/`cursorR`/`commitR` níže); `popFrame` zůstává pro testy a cesty mimo bulk blok (`seed`, `popInto`). |
| `size_t snapshotW() const noexcept` | **audio thread** | — → snapshot write kurzoru | `StreamedSampleReader::beginBlock`; re-snapshot v `popFrameRaw`/`ringAvailable` při vyčerpání lokálního okna | — | — | Bulk API (perf): load `w_` s `memory_order_acquire`. Konzument si 1× za blok snapshotne `w_` a dál čte lokálním kurzorem — místo acquire loadu na každý frame. Hodnoty čtené bulk režimem jsou bit-exact shodné s `popFrame`. |
| `size_t cursorR() const noexcept` | **audio thread** | — → aktuální read kurzor | `StreamedSampleReader::beginBlock` | — | — | Load `r_` s `memory_order_relaxed` (kurzor vlastní konzument — píše ho jen audio thread). Inicializuje lokální kurzor bulk bloku. |
| `void commitR(size_t r) noexcept` | **audio thread** | lokální kurzor → publikace | `StreamedSampleReader::endBlock` | — | `r` = lokální read kurzor po bloku | Store `r_` s `memory_order_release` — 1× za blok zviditelní workeru zkonzumované framy (uvolní místo pro `push`). Spolu se `snapshotW`/`cursorR` dává 1 acquire + 1 release za blok místo 3 atomických operací na KAŽDÝ frame. |
| `int available() const noexcept` | **audio thread** (i worker při výpočtu free) | — → počet framů k dispozici | `StreamedSampleReader` (`refill` threshold check, `ringAvailable` mimo bulk blok), `workerLoop` (výpočet free_frames) | — | — | Load `w_` s `memory_order_acquire`, load `r_` s `memory_order_relaxed`. Vrátí `w_ − r_` (nezáporné díky SPSC kontraktu). Atomický snapshot — hodnota může být o malý kus zastaralá, ale nikdy nadhodnocená ze strany konzumenta (w_ roste pouze na straně producenta). |
| `void resetForReuse() noexcept` | thread volající `acquireRing` | — | `acquireRing` (po bounded spinu na `producers_`) | — | — | Store `w_=0`, `r_=0`, `eof_=false` s `memory_order_relaxed`. Volá ji výhradně `acquireRing` až po krátkém spinu na `producers_ == 0` — `releaseRing` kurzory NEresetuje (jen gen bump + `in_use_=false`), takže reset nikdy neběží souběžně s worker push. |

### `StreamRequestQueue` — SP-MC fronta

| funkce / člen (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `bool push(const StreamRequest& r) noexcept` | **audio thread** (single producer) | `StreamRequest` → `bool` (false = drop) | `StreamEngine::requestRead` | — | `r` = request s ringem, cestou, frame_off, n_frames, eof_when_done | Lock-free RT-safe push. Load `r_` s `acquire`, load `w_` s `relaxed`. Je-li `w_ − r_ >= kCap` (256), vrátí `false` (drop-on-full — `requestRead` ho propaguje volajícímu, který neposune offset a request zopakuje). Zapíše do `buf_[w_ % kCap]`, store `w_+1` s `release`. Žádný mutex. Kopíruje celý `StreamRequest` včetně `std::string path` a `gen` snapshotu (SBO pro krátké cesty, pro delší alokuje — RT-kompromis přijatý pro první iteraci). |
| `bool pop(StreamRequest& out) noexcept` | **stream worker** (multiple consumers) | — → `StreamRequest` nebo `false` | `workerLoop` | — | `out` = výstupní request | Mutex `pop_mtx_` serializuje více worker threadů navzájem (ne proti `push`). Load `r_` s `relaxed`, load `w_` s `acquire`; je-li `r_ >= w_`, vrátí `false`. Zkopíruje `buf_[r_ % kCap]` do `out`, store `r_+1` s `release`. |

---

## `engine/stream/stream_engine.cpp`

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `StreamEngine::StreamEngine(int n_rings, int ring_capacity_frames, int n_workers)` | off-RT (init) | — | `Engine::init` (`engine.cpp`) | `rings_.push_back(make_unique<RingHandle>())` | `n_rings` = počet slotů (fallback default 32; produkčně z `EngineConfig`: 256 main / 48 resonance, vždy ≥ počtu hlasů příslušného poolu); `ring_capacity_frames` = kapacita každého ringu v stereo framech (default 8192); `n_workers` = počet worker threadů (fallback default 4; produkčně auto-sizing v `Engine::init`: `clamp(jádra/2, 2, 8)` main, `clamp(jádra/4, 1, 4)` resonance; explicitní kladná hodnota z configu má přednost) | Alokuje pool `n_rings` instancí `RingHandle` jako `unique_ptr` (pointery se nesmí invalidovat). Pro každý ring: nastaví `capacity_frames`, resize `buf` na `capacity_frames * 2` nul. Nastaví `refill_threshold_ = ring_capacity_frames / 2` (přepíše se přes `setRefillThresholdFrames` z `Engine::recomputeRefillThreshold`). Workery se spouštějí až v `start()`. |
| `StreamEngine::~StreamEngine()` | off-RT (teardown) | — | destruktor | `stop()` | — | Volá `stop()` pro join workerů před destrukcí poolu. |
| `void StreamEngine::start()` | off-RT | — | `Engine::init` | `workers_.emplace_back([this]{ workerLoop(); })` | — | Idempotentní (`run_` CAS false→true; při opakovaném volání se vrátí hned). Spustí `n_workers_` worker threadů, každý volá `workerLoop()`. |
| `void StreamEngine::stop()` | off-RT | — | `Engine` destruktor, `~StreamEngine` | `w.join()` pro každý worker | — | Idempotentní (`run_` CAS true→false). Nastaví `run_=false`, joinuje všechny worker thready; workery vidí `run_==false` při příštím iteraci `while` (nebo po sleep), ukončí se. |
| `RingHandle* StreamEngine::acquireRing()` | **audio thread** (při `Voice::start`) nebo off-RT | — → `RingHandle*` nebo `nullptr` | `StreamedSampleReader::begin`/`beginEofOnly` (z `Voice::start`, `ResonanceVoice::start`) | `resetForReuse()` | — | Lineárně prochází pool. Pro každý slot zkusí CAS `in_use_` false→true s `memory_order_acquire`; při úspěchu krátce (bounded spin) počká na `producers_ == 0` (dobíhající worker memcpy), pak zavolá `resetForReuse()` a vrátí ukazatel. Není-li žádný volný slot, vrátí `nullptr` (Voice hraje bez streamingu nebo přeskočí). |
| `void StreamEngine::releaseRing(RingHandle* r)` | **audio thread** (při `Voice` EOF / steal / noteOff) | `RingHandle*` → vrací pool slot | `StreamedSampleReader::release` (z `Voice::prepareDamp` / `hardStop` / `start` (recyklace) / `process` (po deaktivaci), `ResonanceVoice` analogicky) | `r->gen_.fetch_add` (acq_rel), store `in_use_=false` s `release` | `r` = ring k vrácení | Nejprve gen bump (`gen_.fetch_add`, acq_rel) — zneplatní in-flight requesty (worker je proti `gen` snapshotu ověřuje před každým zápisem i před `eof_` store), pak store `in_use_=false` s `release`. Kurzory se zde NEresetují — reset dělá až `acquireRing` po spinu na `producers_`. Tím je vyloučena ABA recyklace ringu: stale request po re-acquire do ringu nového vlastníka nic nezapíše. |
| `bool StreamEngine::requestRead(RingHandle* ring, const std::string& path, int64_t frame_off, int64_t n_frames, bool eof_when_done) noexcept` | **audio thread** | ring + cesta + offset + délka → push do `req_q_`, vrací `bool` | `StreamedSampleReader::begin` (initial fill) a `StreamedSampleReader::refill` (průběžný refill) — z `Voice`/`ResonanceVoice` | `req_q_.push(req)` | `ring` = cílový ring; `path` = absolutní cesta k WAV; `frame_off` = startovní frame v souboru; `n_frames` = kolik framů načíst; `eof_when_done` = má worker nastavit `ring->eof_` po skončení | Sestaví `StreamRequest`, zkopíruje `path` (string copy), přibalí snapshot `ring->gen_` a zavolá `req_q_.push()`. Vrací `bool`: `false` = plná fronta (drop-on-full, RT-safe) — volající pak NEPOSOUVÁ `file_request_off_`, takže se request příští blok přirozeně zopakuje (dříve tichý drop maskoval underrun jako END-OF-SAMPLE). Kontrola `ring != nullptr` na vstupu. |
| `void StreamEngine::workerLoop()` | **stream worker** | — | `start()` (jako lambda) | `req_q_.pop`, `readWavRange`, `ring->push`, `ring->eof_.store` | — | Hlavní smyčka worker threadu. Běží dokud `run_.load(acquire) == true`. Při prázdné frontě spí 1 ms. Po přijetí `StreamRequest` iteruje: nejdřív ověří `req.gen == ring->gen_` (stale request po `releaseRing` → konec, nic nezapisuje); výpočet volného místa v ringu (`capacity_frames − available()`); je-li ring plný, spí 1 ms a zkusí znovu. Jinak volá `readWavRange(path, off, chunk)` (blokující disk I/O — OK na workeru). Při chybě čtení zaloguje a přeruší zpracování requestu. Při `data.frames == 0` (EOF) nastaví `eof = true`. Zápis do ringu (`ring->push`) běží pod producer try-lockem `producers_` (CAS 0→1, drží se jen přes memcpy) s gen re-checkem uvnitř zámku; pak posune `off` a `remain`. Po dokončení requestu: je-li `eof && eof_when_done` a gen stále platí, nastaví `ring->eof_` store s `memory_order_release`. |
| `int StreamEngine::numRingsUsed() const noexcept` | libovolné | — → počet aktivních slotů | GUI diagnostika | — | — | Lineární průchod poolem, load `in_use_` s `acquire`. Snapshot bez locku — hodnota může být o 1 mimo při souběžném acquire/release, pro GUI refresh ~30 Hz dostačuje. |
| `int StreamEngine::refillThresholdFrames() const noexcept` | **audio thread** | — → práh v stereo framech | `StreamedSampleReader::refill` (z `Voice::process`, `ResonanceVoice::process`) | — | — | Load `refill_threshold_` s `relaxed`. Reader ho porovnává s `ring->available()` a rozhoduje, zda poslat `requestRead`. |
| `void StreamEngine::setRefillThresholdFrames(int v) noexcept` | off-RT | int → — | `Engine::recomputeRefillThreshold` (init i `setBlockSize`, obě `StreamEngine` instance) | — | `v = min(max(capacity/2, block_size*4), capacity − 64)` (clamp shora, aby práh nepřerostl kapacitu) | Store `refill_threshold_` s `relaxed`. Volá se mimo audio thread. |
| `void StreamEngine::noteUnderrun() noexcept` | **audio thread** | — | `Voice::process`, `ResonanceVoice::process` (POLICY hlasů, ne reader) | `nowMicrosSE()` | — | Razítkuje `last_underrun_us_` aktuálním monotónním časem (μs, `steady_clock`) s `memory_order_relaxed`. Volá se pouze při skutečném underrunu (Voice: `advance() == RingEmpty` a `!reader_.cleanEnd()`; ResonanceVoice: `RingEmpty` a `!eofAcquire()`), ne při čistém konci samplu. |
| `bool StreamEngine::underrunRecent(float ms) const noexcept` | libovolné (GUI) | práh v ms → `bool` | `Engine::mainStreamUnderrunRecent`, `Engine::resonanceStreamUnderrunRecent` → GUI panel | `nowMicrosSE()` | `ms` = okno v milisekundách | Load `last_underrun_us_` s `relaxed`. Vrátí `true` pokud `(nyní − last_underrun) < ms*1000 μs`. Při `t==0` (žádný underrun) vrátí `false`. |
| `static uint64_t nowMicrosSE()` (anonymní ns) | libovolné | — → μs od epoch | `noteUnderrun`, `underrunRecent` | `steady_clock::now()` | — | Pomocná funkce: `duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()`. |
| `int RingHandle::push(...)` | **stream worker** | viz výše v `.h` tabulce | `workerLoop` | `memcpy` | viz výše | Implementace: load `r_` s `acquire`, výpočet `free_frames`, wrap přes dvě `memcpy` volání, store `w_` s `release`. |
| `bool RingHandle::popFrame(float& L, float& R) noexcept` | **audio thread** | viz výše | `StreamedSampleReader::popFrameRaw` (mimo bulk blok), testy | — | viz výše | Implementace: load `w_` s `acquire`, load `r_` s `relaxed`, přečte 2 floaty, store `r_+1` s `release`. |
| `int RingHandle::available() const noexcept` | audio / worker | viz výše | `StreamedSampleReader` (threshold check), `workerLoop` (free calc) | — | viz výše | Implementace: load `w_` s `acquire`, load `r_` s `relaxed`. |
| `void RingHandle::resetForReuse() noexcept` | thread volající `acquireRing` | viz výše | `acquireRing` | — | viz výše | Implementace: tři relaxed stores do `w_`, `r_`, `eof_`. Exkluzivitu zaručuje protokol `acquireRing` (CAS `in_use_` + bounded spin na `producers_ == 0`). |

---

## `engine/stream/streamed_reader.h` / `engine/stream/streamed_reader.cpp`

`StreamedSampleReader` — sdílený streaming čtec pro `Voice` a `ResonanceVoice` (kompozice, žádné virtuály). Vznikl bit-exact extrakcí dříve duplikovaného kódu z obou hlasů (spec 2026-06-10 část B) — žádná změna chování, hlídá `tests/test_render_regression.cpp` (FNV-1a hash výstupu deterministické scény). Zapouzdřuje: vlastnictví ringu (`begin`/`beginEofOnly`/`release`), lo/hi interpolační okno (`seed`/`advance`/`loL..hiR`/`loIdx`), EOF-hold primitivy (`eofAcquire`/`holdHiFromLo`/`bumpLoIdx`), refill heuristiku (práh ze `StreamEngine`, half-cap reset pendingu, no-advance-on-drop), bulk čtení (`beginBlock`/`endBlock`), `popInto` (damp crossfade) a diagnostiku. **POLICY při prázdném ringu zůstává v hlasech** (~10 řádků per hlas): `Voice` rozlišuje čistý konec (`cleanEnd()` → zero + 5ms fade, deaktivace) vs skutečný underrun (hold-last + 5ms fade); `ResonanceVoice` drží EOF-hold přes `eofAcquire()` + `holdHiFromLo()`/`bumpLoIdx()`. Reader vrací `Advance::RingEmpty` a vystavuje primitivy. Celá třída běží **výhradně na audio vlákně** (jeden reader = jeden hlas; SPSC konzument svého ringu).

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `bool begin(StreamEngine* se, const std::string& path, int64_t start_frame, int64_t total_frames) noexcept` | **audio thread** | engine + cesta + rozsah → `bool` (ring přidělen?) | `Voice::start`, `ResonanceVoice::start` | `se->acquireRing()`, `se->requestRead(...)` | `start_frame` = první frame za RAM regionem (head / preload_resonance); `total_frames` = délka souboru | Plný reset stavu + acquire ring + první request `[start_frame, min(capacity, zbytek))`. Vrací `false` při plném ring poolu nebo `se == nullptr` — hlas pak dohraje RAM region a utichne (žádný crash). Vrátí-li `requestRead` `false` (plná fronta), `file_request_off_` se NEposouvá → přirozený retry v `refill()` (no-advance-on-drop). |
| `bool beginEofOnly(StreamEngine* se, int64_t request_off) noexcept` | **audio thread** | engine + offset → `bool` | `ResonanceVoice::start` (případ `total_after <= 0`) | `se->acquireRing()`, `ring->eof_.store(true, release)` | `request_off` = konec RAM regionu | Varianta bez requestu: za RAM regionem už není co streamovat → ring se jen označí EOF (EOF-hold policy hlasu pak korektně dojede konec). |
| `void seed(float lo_l, float lo_r, int64_t lo_idx) noexcept` | **audio thread** | poslední RAM frame → seedované okno | `Voice::process`, `ResonanceVoice::process` (první vstup do ring větve) | `popFrameRaw` (první hi) | `lo_*` = poslední frame předchozího RAM regionu; `lo_idx` = jeho FILE-GLOBAL index | Šev RAM→ring: lo = poslední frame head/preload_resonance, hi = první ring pop (plynulý přechod bez nespojitosti). Fallback hi=lo při prázdném ringu — posun okna / EOF policy to vyřeší v `advance()`. |
| `bool seeded() const noexcept` | **audio thread** | — → `bool` | `Voice::process`, `ResonanceVoice::process` | — | — | `ring_lo_idx_ >= 0` (-1 = neseedováno; resetuje `release`). |
| `Advance advance(int64_t target) noexcept` | **audio thread** | cílový frame index → `Reached` / `RingEmpty` | `Voice::process`, `ResonanceVoice::process` | `popFrameRaw` | `target` = `(int64_t)position_` | Posouvá okno (lo←hi, pop hi, idx++) dokud `lo_idx < target`. Při `RingEmpty`: lo už přepsané hi, idx NEinkrementován — přesně původní chování Voice; co dál (zero/hold/EOF-hold), rozhoduje POLICY volajícího hlasu. |
| `float loL()/loR()/hiL()/hiR() const noexcept`, `int64_t loIdx() const noexcept` | **audio thread** | — → interpolační okno | hlasy (lineární interpolace `lo + frac*(hi−lo)`; hold-last při underrunu čte `loL/loR`) | — | — | Gettery lo/hi okna a FILE-GLOBAL indexu lo. |
| `void holdHiFromLo() noexcept` / `void bumpLoIdx() noexcept` | **audio thread** | — | `ResonanceVoice::process` (EOF-hold policy) | — | — | EOF-hold primitivy: hi = lo (drž poslední vzorek) / `lo_idx++` (dojezd indexu k `total_frames − 1` → deaktivace u konce souboru). |
| `void beginBlock() noexcept` | **audio thread** | — → otevřený bulk blok | `Voice::process`, `ResonanceVoice::process` (před render smyčkou, jen s ringem) | `ring->snapshotW()`, `ring->cursorR()` | — | Bulk režim: 1× za blok snapshot `w_` (acquire) + lokální kurzory `blk_r_`/`blk_w_` — místo 3 atomických operací na každý frame. No-op bez ringu nebo při už otevřeném bloku. Hodnoty bit-exact shodné s `popFrame`. |
| `void endBlock() noexcept` | **audio thread** | — → commit `r_` | `Voice::process`, `ResonanceVoice::process` (po smyčce, PŘED `refill()`), `release` | `ring->commitR(blk_r_)` | — | 1× za blok commit `r_` (release) — zviditelní workeru zkonzumované framy. Volat PŘED `refill()`, který čte `available()` z atomik. `release()` commitne automaticky. |
| `void refill(StreamEngine* se, const std::string& path) noexcept` | **audio thread** | — → případný `requestRead` | `Voice::process`, `ResonanceVoice::process` (konec bloku, aktivní hlas s ringem, PO `endBlock()`) | `ring->available()`, `se->refillThresholdFrames()`, `se->requestRead(...)` | — | Per-blok refill heuristika (dříve duplikovaná v obou hlasech): `stream_pending_` se shazuje při `avail >= capacity/2` (worker dohnal request); pod prahem žádá tolik framů, kolik se vejde do volného místa (clamp na zbytek souboru), `eof_when_done` při dosažení konce. Drop (plná fronta) → žádný posun offsetu (jinak by se underrun maskoval jako END-OF-SAMPLE). `remain <= 0` → žádný další request (worker po dohrání nastaví `eof_`). |
| `int popInto(float* dst_interleaved, int max_frames) noexcept` | **audio thread** | — → počet vypopovaných framů | `Voice::prepareDamp` (streamed větev damp crossfade) | `popFrameRaw` | `dst_interleaved` = interleaved stereo cíl | Vypopuje až `max_frames` do `dst` — nadcházející vzorky z ringu pro click-free doběh při retriggeru/stealu. |
| `bool hasRing() const noexcept` | **audio thread** | — → `bool` | hlasy (guard ring větve, refill, release) | — | — | Ring přidělen? |
| `int ringAvailable() const noexcept` | **audio thread** | — → počet framů / `-1` | hlasy (diagnostické logy, EOF hard-guard `ResonanceVoice`) | `ring->available()` / `ring->snapshotW()` | — | `-1` bez ringu (shoda s původními diagnostickými logy). V otevřeném bulk bloku počítá z lokálních kurzorů; při 0 refreshne snapshot `w_` (`blk_w_` je `mutable`) — stejná sémantika jako atomic `available()`, EOF hard-guard nesmí deaktivovat kvůli stale snapshotu. |
| `bool eofAcquire() const noexcept` | **audio thread** | — → `bool` | `ResonanceVoice::process` (EOF-hold policy, hard-guard konce souboru) | `ring->eof_.load(acquire)` | — | `false` bez ringu. |
| `bool eofRelaxed() const noexcept` | **audio thread** | — → `bool` | `Voice::process` (jen diagnostika/logy) | `ring->eof_.load(relaxed)` | — | Relaxed varianta pro logy. |
| `int64_t requestOffset() const noexcept` | **audio thread** | — → `file_request_off_` | diagnostika/logy | — | — | Kolik souboru už bylo vyžádáno. |
| `bool cleanEnd() const noexcept` | **audio thread** | — → `bool` | `Voice::process` (policy při `RingEmpty`) | — | — | `file_request_off_ >= total_frames_`: celý soubor už byl vyžádán → prázdný ring je čistý konec (Info END-OF-SAMPLE, zero+fade), jinak skutečný underrun (Warning, hold-last+fade). |
| `void release(StreamEngine* se) noexcept` | **audio thread** (i off-RT `hardStop`) | — → ring vrácen, stav vynulován | `Voice::prepareDamp`/`hardStop`/`start`/`process` (deaktivace), `ResonanceVoice::start`/`hardStop`/`process` | `endBlock()`, `se->releaseRing(ring)` | — | Případný otevřený bulk blok nejdřív commitne, pak vrátí ring do poolu (pokud je) a plně resetuje stav (okno, offsety, pending, `lo_idx = -1`). Bezpečné i bez ringu / s `se == nullptr`. |
| `bool popFrameRaw(float& L, float& R) noexcept` (privátní) | **audio thread** | — → 1 stereo frame / `false` | `seed`, `advance`, `popInto` | `ring->popFrame` (mimo bulk blok) / lokální kurzor nad snapshotem | — | V bulk režimu čte lokálním kurzorem nad snapshotem (`buf[blk_r_ % capacity]`, commit dělá `endBlock`). Při vyčerpání okna **re-snapshot** `w_`: worker mohl dodat data BĚHEM bloku — sémantika prázdného ringu zůstává identická s per-frame `popFrame` (žádný falešný underrun při mid-block dodávce). Mimo bulk blok deleguje na `ring->popFrame`. |

---

## Křížové odkazy

**Polyfonie (`Voice` / `VoicePool`) — kdo žádá refill (vše přes `StreamedSampleReader`, viz tabulka výše):**
- `Voice::start` (audio thread) → `reader_.begin(stream_main_, ...)` = `acquireRing()` + první `requestRead(...)` (počáteční plnění). Vrátí-li `requestRead` `false`, offset se neposune a `reader_.refill()` request zopakuje.
- `Voice::process` (audio thread) → `reader_.beginBlock()` před render smyčkou, `reader_.endBlock()` po ní (bulk čtení ringu), pak `reader_.refill(stream_main_, path)`: `available() < refillThresholdFrames()` → `requestRead(...)` (při `false` se `file_request_off_` neposouvá).
- `Voice::process` → při `reader_.advance() == RingEmpty` a `!reader_.cleanEnd()` (skutečný underrun, ne čistý konec) → `stream_main_->noteUnderrun()`.
- `Voice::prepareDamp` / `hardStop` / deaktivace v `process` (čistý EOF, underrun fade, release) / steal → `reader_.release(stream_main_)` (= `releaseRing`).
- `VoicePool::setStreamEngine(StreamEngine*)` předá ukazatel na `stream_main_` všem `Voice` instancím.

**Rezonance (`ResonanceVoice` / `ResonanceEngine`) — kdo žádá refill:**
- POZN. (fáze 8): rezonance hraje primárně z **RAM cache** (`preload_resonance`, 12 s, jen cílová vrstva — viz oblast F). `stream_resonance_` se použije jen jako fallback: (a) když rezonance přežije 12 s okno, (b) ve **stream módu** (`ResonanceVoice` `use_cache=false`) během background přestavby cache po změně „Resonance Layer" slideru. Za normálu tedy `stream_resonance_` většinou nemá práci → mizí dřívější underruny.
- `ResonanceVoice::start` → `reader_.begin(stream_resonance_, ...)`; když za RAM regionem už není co streamovat (`total_after <= 0`), `reader_.beginEofOnly(...)` — ring se jen označí EOF.
- `ResonanceVoice::process` → `reader_.beginBlock()`/`endBlock()` (bulk čtení) + `reader_.refill(stream_resonance_, path)` — stejný vzor jako `Voice`.
- `ResonanceVoice::process` → `stream_resonance_->noteUnderrun()` při underrunu (ring prázdný bez EOF).
- `ResonanceVoice` odpojení → `reader_.release(stream_resonance_)`.
- `ResonanceEngine::setStreamEngine(StreamEngine*)` propaguje ukazatel na `stream_resonance_` do všech `ResonanceVoice` slotů.

**Dvě instance StreamEngine (`Engine::init`, `engine.cpp`):**
- `stream_main_` — `n_rings` = `EngineConfig::num_rings` (default 256, vždy ≥ `max_voices`), `n_workers` = `stream_threads` (0 = auto: `clamp(jádra/2, 2, 8)`).
- `stream_resonance_` — `n_rings` = `EngineConfig::resonance_num_rings` (default 48, vždy ≥ `max_resonance_voices`), `n_workers` = `resonance_stream_threads` (0 = auto: `clamp(jádra/4, 1, 4)`).
- Refill práh obou instancí přepočítává `Engine::recomputeRefillThreshold` (init i `setBlockSize`): `min(max(cap/2, block_size*4), cap − 64)`.
- `Engine::mainStreamUnderrunRecent` / `Engine::resonanceStreamUnderrunRecent` — GUI čte oddělené underrun stavy.

**Loader (`wav_reader.h` / `wav_reader.cpp`):**
- `workerLoop` volá `readWavRange(path, frame_off, chunk)` pro každý chunk — blokující disk I/O výhradně na stream worker threadu.
- `SampleStore` volá `readWavRange` off-RT pro preload hlavy samplu a resonance okna při načítání banky.

**Multithreading (oblast A — Core):**
- Audio thread → pouze `snapshotW`/`cursorR`/`commitR` (bulk čtení přes `StreamedSampleReader::beginBlock`/`endBlock`), `popFrame`, `available`, `requestRead` (lock-free push), `noteUnderrun`, `releaseRing`, `acquireRing`. Celý `StreamedSampleReader` žije na audio vlákně.
- Stream worker threads → `pop` (mutex), `readWavRange` (blokující I/O), `push`, store `eof_`.
- GUI / off-RT → `start`, `stop`, `setRefillThresholdFrames`, `numRingsUsed`, `underrunRecent`.

---

## Nálezy revize

**Recyklace ringu s in-flight requestem (ABA / 2 producenti) — ✅ VYŘEŠENO (fix/revize-2026-06-10):**
- Původní nález (revize 2026-06-10, §1.3, KRITICKÁ): `releaseRing` resetoval kurzory a vrátil slot do poolu, i když worker mohl mít k témuž ringu rozpracovaný request (request nelze odvolat). Při steal/retriggeru byl tentýž ring okamžitě znovu přidělen → (a) `resetForReuse` souběžně s worker `push` = data race na kurzorech (UB), (b) dva workeři psali do téhož ringu (propletená data dvou souborů), (c) starý request s `eof_when_done` nastavil `eof_` ringu nového vlastníka = předčasný konec uprostřed samplu.
- Řešení: generační čítač `RingHandle::gen_` — `releaseRing` dělá jen gen bump + `in_use_=false` (kurzory NEresetuje), `StreamRequest` nese snapshot `gen` a worker ho ověřuje před každým chunk push i před `eof_` store → stale request nic nezapíše. Producer try-lock `producers_` (CAS 0/1, držený jen přes memcpy) zaručuje max jednoho zapisujícího workera i při dvou in-flight requestech téhož ringu; `acquireRing` před `resetForReuse` na `producers_ == 0` krátce (bounded) spinuje — kryje zbytkové okno gen bumpu během probíhajícího memcpy. Druhá cesta (předčasné shození `stream_pending_` → druhý request na tentýž ring) je díky producer locku + gen checku neškodná: requesty se serializují a píší v pořadí fronty.

**SPSC/SP-MC korektnost:**
- `RingHandle` je SPSC: `popFrame`/`available` mají jediného konzumenta (Voice na audio threadu) a `push` má v daném okamžiku jediného producenta — to už nezaručuje jen konvence „jeden request v letu“, ale producer try-lock `producers_` (viz výše). Memory ordering je správný: store `w_` s `release` v `push`, load `w_` s `acquire` v `popFrame`; symetricky pro `r_`. Žádný problém.
- `StreamRequestQueue` je SP (single audio thread) + MC (více worker threadů): `push` je lock-free, `pop` serializuje workery mutexem `pop_mtx_`. Správně: mutex chrání pouze workery vzájemně, audio thread se mutexu nedotýká.

**Memory ordering:**
- Bez nálezů. `push` / `popFrame` / `available` dodržují klasický SPSC `release/acquire` handshake. `gen_` používá `acq_rel` na bump a `acquire` na čtení (snapshot i worker check); `producers_` `acq_rel` CAS / `release` unlock. `noteUnderrun`/`underrunRecent` používají `relaxed` (diagnostický timestamp bez ordering požadavku — akceptovatelné). `resetForReuse` používá `relaxed` — exkluzivitu nově vynucuje protokol `acquireRing` (CAS `in_use_` + spin na `producers_`), už to není jen předpoklad o volajícím.
- Drobná poznámka: `available()` v `workerLoop` volá load `w_` s `acquire` a `r_` s `relaxed`. Ve funkci `available()` je to naopak (`w_` acquire, `r_` relaxed) — z pohledu workeru čtoucího `available()` je `r_` psáno audio threadem, takže `relaxed` je dostačující (worker nepotřebuje vidět data za `r_` kursorem, pouze číselnou hodnotu). Bez nálezů.

**Drop-on-full:**
- `StreamRequestQueue::push` vrací `false` při plné frontě (kapacita 256); `requestRead` výsledek propaguje volajícímu (`bool`). `StreamedSampleReader` (`begin`/`refill`) při `false` NEPOSOUVÁ `file_request_off_` → request se příští blok přirozeně zopakuje. Dřívější tiché zahození výsledku (drop posouval offset a maskoval underrun jako END-OF-SAMPLE) je opraveno (fix/revize-2026-06-10). Bez nálezů.

**Underrun cesta:**
- `Voice::process`: při `reader_.advance() == RingEmpty` rozlišuje čistý konec (`reader_.cleanEnd()` → Info END-OF-SAMPLE, výstup nuly = deactivate+zero dle reference icr) od skutečného underrunu (Warning + `noteUnderrun()`, drží poslední známý vzorek `reader_.loL()/loR()`, aby ho fade tvaroval). Oba případy dozní stejným 5ms fade (`kUnderrunFadeMs`). `ResonanceVoice::process`: rozhoduje přes `reader_.eofAcquire()` (EOF-hold: `holdHiFromLo` + `bumpLoIdx`, konec hlasu u konce souboru) vs. underrun (Warning + fade). POLICY zůstává v hlasech, primitivy poskytuje `StreamedSampleReader`. Logika je konzistentní. Bez nálezů.

**Potenciální suboptimalita (poznámka, ne nález):**
- Při `wrote < data.frames` v `workerLoop` kód posune `off` pouze o `wrote` a v příští iteraci znovu přečte (disk I/O) část dat, která již byla jednou načtena ale nevešla se do ringu. Kód sám tuto situaci komentuje jako FUTURE optimalizaci (partial-push s buffered zbytkem). V praxi je ring vždy větší než chunk, takže `wrote == data.frames` platí vždy — bez funkčního dopadu.
