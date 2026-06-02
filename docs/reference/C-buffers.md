# Zpracování bufferu

Audio výstupní cesta: miniaudio (`AudioDevice`) volá na audio threadu registrovaný callback, který zavolá `Engine::processBlock` — ta renderuje do dočasných L/R scratch bufferů a caller (callback v `app_context.cpp` resp. v `main.cpp`) výsledek interleave do float32 výstupního bufferu miniaudio. Disk I/O se na audio threadu nikdy neodehrává: Voice čte výhradně z RAM (SPSC ring `RingHandle`), který asynchronně plní `StreamEngine` z worker threadů. `StreamEngine` existuje ve dvou instancích: `stream_main_` pro `VoicePool` (main sample voices) a `stream_resonance_` pro `ResonanceEngine` (viz oblast A — Core / Multithreading). Každá instance drží pool ringů, SP-MC request frontu a pool worker threadů; Voice žádá refill přes `requestRead` (lock-free push, drop-on-full = RT-safe), worker přijímá přes mutex-serializovaný pop.

---

## Implementováno v souborech

| soubor | odpovědnost | klíčové typy |
|---|---|---|
| `engine/io/audio_device.h` | Deklarace miniaudio playback wrapperu, definice `AudioCallback` typedef | `AudioDevice`, `AudioCallback` |
| `engine/io/audio_device.cpp` | Inicializace `ma_device` (f32, stereo, periodSizeInFrames), C-trampoline `ma_data_callback` → `invokeCallback` | `ma_device`, trampolina `ma_data_callback` |
| `engine/stream/stream_engine.h` | Deklarace SPSC ringu (`RingHandle`), SP-MC fronty (`StreamRequestQueue`), streaming engine (`StreamEngine`) | `RingHandle`, `StreamRequest`, `StreamRequestQueue`, `StreamEngine` |
| `engine/stream/stream_engine.cpp` | Implementace ring push/pop, worker loop (čtení WAV po chuncích), acquire/release pool, underrun timestamp | `workerLoop`, `readWavRange`, `nowMicrosSE` |

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

## `engine/stream/stream_engine.h`

### `RingHandle` — SPSC ring buffer pro jeden hlas

| funkce / člen (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `std::vector<float> buf` | — | — | — | — | Kapacita = `capacity_frames * 2` floatů (interleaved stereo) | Jednou alokováno v `StreamEngine` konstruktoru; nikdy se neralokuje. |
| `std::atomic<size_t> w_` | producent (worker) | — | — | — | Monotónně rostoucí write cursor | Zapisuje výhradně worker thread. Čte audio thread (v `popFrame`, `available`). Memory order: store `release`, load `acquire` na straně konzumenta. |
| `std::atomic<size_t> r_` | konzument (audio thread) | — | — | — | Monotónně rostoucí read cursor | Zapisuje výhradně audio thread. Čte worker (v `push`). Memory order: store `release`, load `acquire` na straně producenta. |
| `std::atomic<bool> eof_` | worker → audio | — | — | — | EOF příznak | Worker nastaví na `true` (memory_order_release) po skončení posledního chunku, pokud byl request podán s `eof_when_done=true`. Voice čte (acquire) a při `eof_==true` + prázdný ring ukončí hlas čistě bez underrun fade. |
| `std::atomic<bool> in_use_` | pool allocator | — | — | — | Alokační flag | CAS v `acquireRing` (false→true, `acquire`); store false v `releaseRing` (`release`). |
| `int push(const float* src_interleaved, int n_frames)` | **stream worker** | interleaved stereo float pole, počet framů → počet skutečně zapsaných | `workerLoop` | `memcpy` (1–2 segmenty při wrapu) | `src_interleaved` = zdrojová data z `readWavRange`; `n_frames` = kolik framů zapsat | Výpočet volného místa: `free_frames = capacity_frames − (w_ − r_)`. Zápis zvládá přechod přes konec bufferu (wrap) dvěma `memcpy` volání. Store `w_` s `memory_order_release`. Vrací skutečně zapsaný počet (≤ `n_frames`); při plném ringu vrátí 0. Bez alokací. |
| `bool popFrame(float& L, float& R) noexcept` | **audio thread** | — → jeden stereo frame (L, R) nebo `false` | `Voice::processBlock`, `ResonanceVoice::processBlock` | — | `L`, `R` = výstupní reference | Load `w_` s `memory_order_acquire`, load `r_` s `memory_order_relaxed`. Pokud `r_ >= w_` (prázdný ring), vrátí `false`. Jinak přečte frame na pozici `r_ % capacity_frames`, store `r_+1` s `memory_order_release`. Jeden frame = 2 floaty z `buf`. |
| `int available() const noexcept` | **audio thread** (i worker při výpočtu free) | — → počet framů k dispozici | `Voice` (refill threshold check), `workerLoop` (výpočet free_frames) | — | — | Load `w_` s `memory_order_acquire`, load `r_` s `memory_order_relaxed`. Vrátí `w_ − r_` (nezáporné díky SPSC kontraktu). Atomický snapshot — hodnota může být o malý kus zastaralá, ale nikdy nadhodnocená ze strany konzumenta (w_ roste pouze na straně producenta). |
| `void resetForReuse() noexcept` | off-RT nebo thread s exkluzivitou | — | `acquireRing`, `releaseRing` | — | — | Store `w_=0`, `r_=0`, `eof_=false` s `memory_order_relaxed`. Bezpečné pouze tehdy, kdy žádný jiný thread nedrží ring aktivně — viz poznámky v hlavičce a `.cpp`. |

### `StreamRequestQueue` — SP-MC fronta

| funkce / člen (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `bool push(const StreamRequest& r) noexcept` | **audio thread** (single producer) | `StreamRequest` → `bool` (false = drop) | `StreamEngine::requestRead` | — | `r` = request s ringem, cestou, frame_off, n_frames, eof_when_done | Lock-free RT-safe push. Load `r_` s `acquire`, load `w_` s `relaxed`. Je-li `w_ − r_ >= kCap` (256), vrátí `false` (drop-on-full — hlas přežije přes underrun fade). Zapíše do `buf_[w_ % kCap]`, store `w_+1` s `release`. Žádný mutex. Kopíruje celý `StreamRequest` včetně `std::string path` (SBO pro krátké cesty, pro delší alokuje — RT-kompromis přijatý pro první iteraci). |
| `bool pop(StreamRequest& out) noexcept` | **stream worker** (multiple consumers) | — → `StreamRequest` nebo `false` | `workerLoop` | — | `out` = výstupní request | Mutex `pop_mtx_` serializuje více worker threadů navzájem (ne proti `push`). Load `r_` s `relaxed`, load `w_` s `acquire`; je-li `r_ >= w_`, vrátí `false`. Zkopíruje `buf_[r_ % kCap]` do `out`, store `r_+1` s `release`. |

---

## `engine/stream/stream_engine.cpp`

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `StreamEngine::StreamEngine(int n_rings, int ring_capacity_frames, int n_workers)` | off-RT (init) | — | `Engine::init` (`engine.cpp` řádky 33–35) | `rings_.push_back(make_unique<RingHandle>())` | `n_rings` = počet slotů (default 32); `ring_capacity_frames` = kapacita každého ringu v stereo framech (default 8192); `n_workers` = počet worker threadů (default 4) | Alokuje pool `n_rings` instancí `RingHandle` jako `unique_ptr` (pointery se nesmí invalidovat). Pro každý ring: nastaví `capacity_frames`, resize `buf` na `capacity_frames * 2` nul. Nastaví `refill_threshold_ = ring_capacity_frames / 2` (přepíše se přes `setRefillThresholdFrames` po `setBlockSize`). Workery se spouštějí až v `start()`. |
| `StreamEngine::~StreamEngine()` | off-RT (teardown) | — | destruktor | `stop()` | — | Volá `stop()` pro join workerů před destrukcí poolu. |
| `void StreamEngine::start()` | off-RT | — | `Engine::init` | `workers_.emplace_back([this]{ workerLoop(); })` | — | Idempotentní (`run_` CAS false→true; při opakovaném volání se vrátí hned). Spustí `n_workers_` worker threadů, každý volá `workerLoop()`. |
| `void StreamEngine::stop()` | off-RT | — | `Engine` destruktor, `~StreamEngine` | `w.join()` pro každý worker | — | Idempotentní (`run_` CAS true→false). Nastaví `run_=false`, joinuje všechny worker thready; workery vidí `run_==false` při příštím iteraci `while` (nebo po sleep), ukončí se. |
| `RingHandle* StreamEngine::acquireRing()` | **audio thread** (při `Voice::start`) nebo off-RT | — → `RingHandle*` nebo `nullptr` | `Voice::start`, `ResonanceVoice::start` | `resetForReuse()` | — | Lineárně prochází pool. Pro každý slot zkusí CAS `in_use_` false→true s `memory_order_acquire`; při úspěchu zavolá `resetForReuse()` a vrátí ukazatel. Není-li žádný volný slot, vrátí `nullptr` (Voice hraje bez streamingu nebo přeskočí). |
| `void StreamEngine::releaseRing(RingHandle* r)` | **audio thread** (při `Voice` EOF / steal / noteOff) | `RingHandle*` → vrací pool slot | `Voice::stop`, `Voice::processBlock` (EOF/underrun), `ResonanceVoice` analogicky | `r->resetForReuse()`, store `in_use_=false` s `release` | `r` = ring k vrácení | Nejprve `resetForReuse()` (reset cursorů), pak store `in_use_=false` s `release` — pořadí zabraňuje race s případným pozdním worker zápisem do ringu (komentář v `.cpp`). |
| `void StreamEngine::requestRead(RingHandle* ring, const std::string& path, int64_t frame_off, int64_t n_frames, bool eof_when_done) noexcept` | **audio thread** | ring + cesta + offset + délka → push do `req_q_` | `Voice::processBlock` (refill check), `Voice::start` (initial fill), analogicky `ResonanceVoice` | `req_q_.push(req)` | `ring` = cílový ring; `path` = absolutní cesta k WAV; `frame_off` = startovní frame v souboru; `n_frames` = kolik framů načíst; `eof_when_done` = má worker nastavit `ring->eof_` po skončení | Sestaví `StreamRequest`, zkopíruje `path` (string copy), zavolá `req_q_.push()`. Vrácená hodnota (false = drop) je zahozena — RT-safe; při dlouhodobém dropu voice underrunuje a fade-ne do ticha. Kontrola `ring != nullptr` na vstupu. |
| `void StreamEngine::workerLoop()` | **stream worker** | — | `start()` (jako lambda) | `req_q_.pop`, `readWavRange`, `ring->push`, `ring->eof_.store` | — | Hlavní smyčka worker threadu. Běží dokud `run_.load(acquire) == true`. Při prázdné frontě spí 1 ms. Po přijetí `StreamRequest` iteruje: výpočet volného místa v ringu (`capacity_frames − available()`); je-li ring plný, spí 1 ms a zkusí znovu. Jinak volá `readWavRange(path, off, chunk)` (blokující disk I/O — OK na workeru). Při chybě čtení zaloguje a přeruší zpracování requestu. Při `data.frames == 0` (EOF) nastaví `eof = true`. Zapíše data do ringu (`ring->push`), posune `off` a `remain`. Po dokončení requestu: je-li `eof && eof_when_done`, nastaví `ring->eof_` store s `memory_order_release`. |
| `int StreamEngine::numRingsUsed() const noexcept` | libovolné | — → počet aktivních slotů | GUI diagnostika | — | — | Lineární průchod poolem, load `in_use_` s `acquire`. Snapshot bez locku — hodnota může být o 1 mimo při souběžném acquire/release, pro GUI refresh ~30 Hz dostačuje. |
| `int StreamEngine::refillThresholdFrames() const noexcept` | **audio thread** | — → práh v stereo framech | `Voice::processBlock`, `ResonanceVoice::processBlock` | — | — | Load `refill_threshold_` s `relaxed`. Voice ho porovnává s `ring->available()` a rozhoduje, zda poslat `requestRead`. |
| `void StreamEngine::setRefillThresholdFrames(int v) noexcept` | off-RT | int → — | `Engine::setBlockSize` (oba `StreamEngine` instance) | — | `v = max(capacity/2, block_size*4)` | Store `refill_threshold_` s `relaxed`. Volá se z `Engine::setBlockSize` mimo audio thread. |
| `void StreamEngine::noteUnderrun() noexcept` | **audio thread** | — | `Voice::processBlock`, `ResonanceVoice::processBlock` | `nowMicrosSE()` | — | Razítkuje `last_underrun_us_` aktuálním monotónním časem (μs, `steady_clock`) s `memory_order_relaxed`. Volá se pouze při skutečném underrunu (ring prázdný a `eof_==false`). |
| `bool StreamEngine::underrunRecent(float ms) const noexcept` | libovolné (GUI) | práh v ms → `bool` | `Engine::mainStreamUnderrunRecent`, `Engine::resonanceStreamUnderrunRecent` → GUI panel | `nowMicrosSE()` | `ms` = okno v milisekundách | Load `last_underrun_us_` s `relaxed`. Vrátí `true` pokud `(nyní − last_underrun) < ms*1000 μs`. Při `t==0` (žádný underrun) vrátí `false`. |
| `static uint64_t nowMicrosSE()` (anonymní ns) | libovolné | — → μs od epoch | `noteUnderrun`, `underrunRecent` | `steady_clock::now()` | — | Pomocná funkce: `duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()`. |
| `int RingHandle::push(...)` | **stream worker** | viz výše v `.h` tabulce | `workerLoop` | `memcpy` | viz výše | Implementace: load `r_` s `acquire`, výpočet `free_frames`, wrap přes dvě `memcpy` volání, store `w_` s `release`. |
| `bool RingHandle::popFrame(float& L, float& R) noexcept` | **audio thread** | viz výše | `Voice`, `ResonanceVoice` | — | viz výše | Implementace: load `w_` s `acquire`, load `r_` s `relaxed`, přečte 2 floaty, store `r_+1` s `release`. |
| `int RingHandle::available() const noexcept` | audio / worker | viz výše | `Voice` (threshold check), `workerLoop` (free calc) | — | viz výše | Implementace: load `w_` s `acquire`, load `r_` s `relaxed`. |
| `void RingHandle::resetForReuse() noexcept` | off-RT nebo thread s exkluzivitou | viz výše | `acquireRing`, `releaseRing` | — | viz výše | Implementace: tři relaxed storesdo `w_`, `r_`, `eof_`. Bezpečné pouze při absenci souběžných operací na ringu. |

---

## Křížové odkazy

**Polyfonie (`Voice` / `VoicePool`) — kdo žádá refill:**
- `Voice::start` (audio thread) → `stream_main_->acquireRing()` + `stream_main_->requestRead(...)` (počáteční plnění).
- `Voice::processBlock` (audio thread) → `ring_->available() < refillThresholdFrames()` → `stream_main_->requestRead(...)` (průběžný refill).
- `Voice::processBlock` → při prázdném ringu a `eof_==false` → `stream_main_->noteUnderrun()`.
- `Voice::stop` / EOF / steal → `stream_main_->releaseRing(ring_)`.
- `VoicePool::setStreamEngine(StreamEngine*)` předá ukazatel na `stream_main_` všem `Voice` instancím.

**Rezonance (`ResonanceVoice` / `ResonanceEngine`) — kdo žádá refill:**
- `ResonanceVoice::start` → `stream_resonance_->acquireRing()` + `stream_resonance_->requestRead(...)`.
- `ResonanceVoice::processBlock` → analogický refill threshold check + `requestRead`.
- `ResonanceVoice::processBlock` → `stream_resonance_->noteUnderrun()` při underrunu.
- `ResonanceVoice` odpojení → `stream_resonance_->releaseRing(ring_)`.
- `ResonanceEngine::setStreamEngine(StreamEngine*)` propaguje ukazatel na `stream_resonance_` do všech `ResonanceVoice` slotů.

**Dvě instance StreamEngine (`engine.cpp` řádky 33–44):**
- `stream_main_` — `n_rings` = počet hlasů v poolu (main), `n_workers` = 4.
- `stream_resonance_` — `n_rings` = rezonančních slotů, `n_workers` = 4.
- Oba jsou přepočítány přes `setRefillThresholdFrames` při `Engine::setBlockSize`.
- `Engine::mainStreamUnderrunRecent` / `Engine::resonanceStreamUnderrunRecent` — GUI čte oddělené underrun stavy.

**Loader (`wav_reader.h` / `wav_reader.cpp`):**
- `workerLoop` volá `readWavRange(path, frame_off, chunk)` pro každý chunk — blokující disk I/O výhradně na stream worker threadu.
- `SampleStore` volá `readWavRange` off-RT pro preload hlavy samplu a resonance okna při načítání banky.

**Multithreading (oblast A — Core):**
- Audio thread → pouze `popFrame`, `available`, `requestRead` (lock-free push), `noteUnderrun`, `releaseRing`, `acquireRing`.
- Stream worker threads → `pop` (mutex), `readWavRange` (blokující I/O), `push`, store `eof_`.
- GUI / off-RT → `start`, `stop`, `setRefillThresholdFrames`, `numRingsUsed`, `underrunRecent`.

---

## Nálezy revize

**SPSC/SP-MC korektnost:**
- `RingHandle` je čistý SPSC: `push` má jediný producent (worker), `popFrame`/`available` mají jediného konzumenta (Voice na audio threadu). Memory ordering je správný: store `w_` s `release` v `push`, load `w_` s `acquire` v `popFrame`; symetricky pro `r_`. Žádný problém.
- `StreamRequestQueue` je SP (single audio thread) + MC (více worker threadů): `push` je lock-free, `pop` serializuje workery mutexem `pop_mtx_`. Správně: mutex chrání pouze workerů vzájemně, audio thread se mutexu nedotýká.

**Memory ordering:**
- Bez nálezů. `push` / `popFrame` / `available` dodržují klasický SPSC `release/acquire` handshake. `noteUnderrun`/`underrunRecent` používají `relaxed` (diagnostický timestamp bez ordering požadavku — akceptovatelné). `resetForReuse` používá `relaxed` — bezpečné za předpokladu exkluzivity při volání (je-li dodrženo volajícím kódem).
- Drobná poznámka: `available()` v `workerLoop` volá load `w_` s `acquire` a `r_` s `relaxed`. Ve funkci `available()` je to naopak (`w_` acquire, `r_` relaxed) — z pohledu workeru čtoucího `available()` je `r_` psáno audio threadem, takže `relaxed` je dostačující (worker nepotřebuje vidět data za `r_` kursorem, pouze číselnou hodnotu). Bez nálezů.

**Drop-on-full:**
- `StreamRequestQueue::push` vrací `false` při plné frontě (kapacita 256); `requestRead` výsledek tiše zahazuje. Voice přežije díky underrun fade. Chování je záměrné a zdokumentováno. Bez nálezů.

**Underrun cesta:**
- `Voice::processBlock` / `ResonanceVoice::processBlock`: při `popFrame == false` a `eof_ == false` → `noteUnderrun()` + fade-out. Při `eof_ == true` a prázdném ringu → čistý konec bez underrun záznamu. Logika je konzistentní. Bez nálezů.

**Potenciální suboptimalita (poznámka, ne nález):**
- Při `wrote < data.frames` v `workerLoop` kód posune `off` pouze o `wrote` a v příští iteraci znovu přečte (disk I/O) část dat, která již byla jednou načtena ale nevešla se do ringu. Kód sám tuto situaci komentuje jako FUTURE optimalizaci (partial-push s buffered zbytkem). V praxi je ring vždy větší než chunk, takže `wrote == data.frames` platí vždy — bez funkčního dopadu.
