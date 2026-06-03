# I — Multithreading

Dokument popisuje **model vláken a synchronizace napříč celým enginem**. Není to
tabulka souborů — je to cross-cutting pohled na to, která vlákna existují, jak
mezi sebou sdílejí data a jaké invarianty musí platit za všech okolností.

> Přehled: engine má 5 + N kategorií vláken. Ústřední princip je, že **audio
> vlákno nikdy neblokuje ani nealokuje** — veškerý sdílený stav přechází přes
> lock-free struktury (SPSC fronty, atomiky) nebo protokol „graceful pause"
> (bank reload). Disk I/O se děje výhradně ve worker vláknech.

---

## Vlákna

| Vlákno | Co dělá | Co NESMÍ (RT pravidla) | Spouští / joinuje kdo |
|--------|---------|------------------------|-----------------------|
| **Audio vlákno** (`processBlock`) | Drainuje `MidiQueue` → aktualizuje `PedalState`, `VoicePool`, `ResonanceEngine`; renderuje hlasy; aplikuje master gain + `DspChain`; počítá peak metr. | Alokovat paměť, volat blokující I/O, zamykat mutexy (vyjma atomik), volat ne-RT logger (`log()`). | miniaudio (interní OS audio thread); `AudioDevice::start()` / `AudioDevice::stop()`. |
| **GUI hlavní vlákno** | GLFW event loop + ImGui render loop (~60 Hz vsync); čte diagnostické atomiky z `Engine`; volá `engine.setMasterGain/setReleaseMs/…`; volá `engine.reloadBank()`. | Nic specificky zakázáno — GUI je non-RT. | `main()` ve `app/gui/main.cpp`; žije po celou dobu procesu. |
| **MIDI callback vlákno** | RtMidi interní vlákno; `MidiInput::callback()` překládá MIDI bajty → `engine.noteOn/noteOff/sustainPedal/allNotesOff()` (vše jen `midi_q_.push()`). | Volat cokoli blokujícího; přímý přístup na engine state mimo frontu. | RtMidi knihovna; `MidiInput::open()` / `MidiInput::close()`. |
| **Stream worker thready — main pool** | `StreamEngine::workerLoop()` (N vláken, **auto-sized** v `Engine::init` z `hardware_concurrency()`: `clamp(jádra/2, 2, 8)`; configem lze přebít); vybírají `StreamRequest` z `StreamRequestQueue` (mutex na pop); čtou WAV z disku pres `readWavRange()`; zapisují do `RingHandle::buf` (`push()`); nastavují `ring->eof_`. | — (non-RT; mohou blokovat na disk I/O). | `StreamEngine::start()` v `engine/stream/stream_engine.cpp`; `StreamEngine::stop()` (join). Engine::init() → stream_main_->start(). |
| **Stream worker thready — resonance pool** | Totéž pro druhý `StreamEngine` (`stream_resonance_`); izolovaný od main poolu (separátní ringy + fronta). Auto-sized `clamp(jádra/4, 1, 4)`. | — | Stejný vzor; `Engine::init()` → `stream_resonance_->start()`; `Engine::~Engine()` → `stop()`. |
| **Log-flush vlákno** | Každých 10 ms volá `Logger::default_().flushRTBuffer()` — přesouvá záznamy z RT ring bufferu do konzole/souboru/subscriberů. | — | `main()` ve `app/gui/main.cpp`; `std::thread log_thr`; joinován před `ctx.shutdown()`. |

> **Poznámka k CLI:** `app/cli/main.cpp` má tentýž log-flush vzor (100 Hz sleep
> v `log_thr`) a identický audio callback.

---

## Sdílení dat / synchronizace

### MidiQueue — SPSC lock-free (1024 slotů)

- **Typ:** SPSC fronta (`engine/midi/midi_queue.h`); producent = MIDI/GUI vlákno,
  konzument = audio vlákno.
- **Memory ordering:** `push`: čte `r_` s `acquire`, zapisuje `w_` s `release`.
  `pop`: čte `w_` s `acquire`, zapisuje `r_` s `release`. Standardní SPSC pattern.
- **Co řeší:** přenos MIDI událostí (NoteOn/Off/Sustain/AllNotesOff) z libovolného
  non-RT vlákna na audio vlákno bez zámku a bez blokování.
- **Drop-on-full:** fronta je kapacity 1024; při přetečení `push` vrátí `false`
  a událost se zahodí. Na audio vláknu nedojde k čekání.
- **Použití:** `Engine::noteOn/noteOff/sustainPedal/allNotesOff()` — volatelné z
  MIDI callback threadu i GUI threadu současně (producent je single-thread z pohledu
  `w_`, ale MIDI a GUI mohou volat interleaveně — viz Nálezy revize).

---

### StreamRequestQueue — SP-MC (256 slotů)

- **Typ:** Single-producer / Multi-consumer; `engine/stream/stream_engine.h`.
  Push je lock-free (RT-safe, audio vlákno), pop serializován `pop_mtx_` (mutex)
  jen mezi worker thready navzájem.
- **Memory ordering:** push: `r_` acquire, `w_` release. Pop: pod mutexem, `w_`
  acquire, `r_` release.
- **Co řeší:** `Voice`/`ResonanceVoice` na audio vlákně posílají refill požadavky
  do worker poolu bez blokování. Více workerů soutěží o pop — mutex na pop side
  je správné řešení pro SPMC bez lock-free MPMC struktury.
- **Drop-on-full:** při plné frontě `requestRead()` zahazuje požadavek (bez chyby).
  Voice přežije díky underrun fade (`kUnderrunFadeMs = 5 ms`).

---

### RingHandle — SPSC ring (per-voice stereo streaming)

- **Typ:** SPSC kruhový buffer stereo float; producent = worker vlákno,
  konzument = přesně jeden `Voice`/`ResonanceVoice` (audio vlákno).
- **Memory ordering:** `push`: `r_` acquire, `w_` release. `popFrame`: `w_`
  acquire, `r_` release. `eof_`: worker zapisuje `release`, voice čte `acquire`.
- **Co řeší:** přenos streamovaných audio dat z disku (worker) do audio vlákna
  bez alokací a bez blokování.
- **Alokátor slotů:** `in_use_` atomic bool na každém `RingHandle`. `acquireRing()`
  CAS (`acquire`/`relaxed`); `releaseRing()` store `false` s `release` — po resetu
  kurzorů.
- **Kapacita:** default 8192 stereo frames (~170 ms @ 48 kHz). Dva oddělené pooly:
  `stream_main_` (256 ringů) a `stream_resonance_` (48 ringů).

---

### RT log ring + flush vlákno

- **Typ:** SPSC ring (1024 slotů fixed-size `Entry`); producent = audio vlákno
  (`logRT`), konzument = log-flush vlákno (`flushRTBuffer`).
- **Memory ordering:** `vlogRT`: `rt_read_idx_` acquire, `rt_write_idx_` release
  (entry kompletní před publish). `flushRTBuffer`: `rt_write_idx_` acquire,
  `rt_read_idx_` release po flush.
- **Co řeší:** audio vlákno smí logovat bez mutex, alokace ani blokování.
  `MESSAGE_MAX = 256` bajtů; `COMPONENT_MAX = 32` bajtů — vše na zásobníku.
- **Drop-on-full:** `rt_dropped_` počítá zahozené zprávy (GUI/diag).
- **Flush vlákno:** `log_thr` v `main.cpp`, 10 ms sleep mezi průchody.
  Záznamy z RT cesty **nejsou** posílány subscriberům (GUI log strip) — flush
  jde jen na konzoli/soubor. Subscribery dostávají jen non-RT logy (`log()`).

---

### Atomiky — Engine

| Atomická proměnná | Typ | Producent → konzument | Ordering | Co řeší |
|---|---|---|---|---|
| `master_gain_` | `atomic<float>` | GUI vlákno (`setMasterGain`) → audio vlákno (`processBlock`) | relaxed/relaxed | Okamžitá změna hlasitosti bez zámku |
| `master_peak_l_`, `master_peak_r_` | `atomic<float>` | audio vlákno (processBlock) → GUI vlákno (peak metr) | relaxed/relaxed | Peak metr pro VU indikátor; decay ~100 ms |
| `bank_loading_` | `atomic<bool>` | GUI vlákno / `reloadBank` (`store release`) → audio vlákno (`load acquire`) | release/acquire | „Graceful pause" guard pro bank reload; při `true` audio vrátí ticho |
| `last_note_on_us_` | `atomic<uint64_t>` | MIDI/GUI vlákno (`noteOn`) → GUI vlákno (`noteOnRecent`) | relaxed/relaxed | Blikání NOTE indikátoru; 64b write je atomické na x86/arm64 |
| `last_note_off_us_` | `atomic<uint64_t>` | MIDI/GUI vlákno (`noteOff`) → GUI vlákno (`noteOffRecent`) | relaxed/relaxed | Blikání OFF indikátoru |

---

### Atomiky — StreamEngine (per pool)

| Proměnná | Typ | Producent → konzument | Ordering | Co řeší |
|---|---|---|---|---|
| `run_` | `atomic<bool>` | `start()`/`stop()` (main/GUI) → worker thready | CAS + acquire v worker loop | Řídí životní cyklus worker vláken |
| `last_underrun_us_` | `atomic<uint64_t>` | audio vlákno (`noteUnderrun()`) → GUI vlákno (`underrunRecent()`) | relaxed/relaxed | Indikátor underrunu v GUI |
| `refill_threshold_` | `atomic<int>` | GUI/main vlákno (`setRefillThresholdFrames`) → audio vlákno (`refillThresholdFrames()`) | relaxed/relaxed | Prahová hodnota pro refill request |
| `RingHandle::w_`, `r_` | `atomic<size_t>` | worker (w_) / audio (r_) | release/acquire | SPSC ring kursory |
| `RingHandle::eof_` | `atomic<bool>` | worker (`release`) → audio vlákno (`acquire`) | release/acquire | Signál konce souboru |
| `RingHandle::in_use_` | `atomic<bool>` | audio vlákno CAS acquire → kdokoli release | acquire/release | Alokátor slotů |

---

### Atomiky — ResonanceEngine

| Proměnná | Typ | Producent → konzument | Ordering | Co řeší |
|---|---|---|---|---|
| `strength_` | `atomic<float>` | GUI vlákno (`setStrength`) → audio vlákno (`onPlayedNoteOn`) | relaxed/relaxed | Síla rezonance za běhu |
| `max_voices_` | `atomic<int>` | GUI vlákno (`setMaxVoices`) → audio vlákno (`enforceVoiceBudget`) | relaxed/relaxed | Živý strop počtu rezonančních hlasů |

---

### Atomiky — DSP stage (Convolver, AGC, Enhancer, Limiter)

Každá DSP stage (`Convolver`, `AGC`, `Enhancer`, `Limiter`) drží parametry jako `atomic<float>`
a `atomic<bool> enabled_`. Pattern je identický:

- **Producent:** GUI vlákno (`set()`, `setEnabled()`).
- **Konzument:** audio vlákno (`process()`).
- **Ordering:** relaxed na obou stranách (parametry se mění v GUI tempu, řádově
  jednotky Hz; přechodné stare hodnoty nejsou slyšitelné).
- **Metr** (`cur_gain_` v AGC, `gr_db_` v Limiteru): psán audio vláknem (relaxed),
  čten GUI vláknem (relaxed).

---

### PedalState — single-thread (audio)

`PedalState` **není thread-safe** a ani to není potřeba. Všechna volání
(`setSustainCC`, `noteOn`, `noteOff`, `allNotesOff`) přicházejí výhradně z audio
vlákna při drainování `MidiQueue`. Hlavička to explicitně dokumentuje:

> *„API je jednovláknové — všechna volání z audio threadu při drainu MidiQueue.
> GUI/MIDI thread posílá změny výhradně přes MidiQueue."*

GUI čte `pedalCC()` přes `Engine::pedalCC()` — komentář v `engine.cpp` říká, že
zápis 1 bajtu je fakticky atomický na všech platformách a přechodná hodnota při
změně je tolerovaná (ramcova konzistence).

---

### LogRingBuffer — mutex (GUI log strip)

`app/gui/log_subscriber.h`: cyklický buffer 256 `LogEntry` chráněný `std::mutex`.
Producent = libovolný thread volající `Logger::log()` (non-RT cesta, Logger volá
subscibery pod `subscriber_mtx_`), konzument = GUI render thread (snapshot ~30 Hz).
Push je triviální (lock + 1 přiřazení), lock je krátkodobý.

---

## Vlastnictví & lifetime

```
main() / main.cpp
  └── AppContext (stack)
        ├── Engine (member, celá délka AppContext)
        │     ├── unique_ptr<VoicePool>       pool_
        │     │     └── vector<Voice>         (raw StreamEngine*)
        │     ├── unique_ptr<StreamEngine>    stream_main_   ← vlastní worker thready
        │     ├── unique_ptr<StreamEngine>    stream_resonance_ ← vlastní worker thready
        │     ├── unique_ptr<ResonanceEngine> resonance_
        │     │     └── array<unique_ptr<ResonanceVoice>, 128>
        │     │           (raw StreamEngine*)
        │     ├── MidiQueue                   midi_q_
        │     ├── PedalState                  pedal_
        │     └── dsp::DspChain               dsp_
        ├── unique_ptr<AudioDevice>   audio    ← vlastní audio thread (miniaudio)
        ├── MidiInput                 midi     ← vlastní MIDI callback thread (RtMidi)
        └── LogRingBuffer             log_buf
  └── log_thr (std::thread, stack v main)
```

### Pořadí startu (initFromState)

1. `Logger` subscriber zaregistrován (log_buf.push).
2. `Engine::init()` — vytvoří VoicePool, oba StreamEngine (spuštění workerů:
   `stream_main_->start()`, `stream_resonance_->start()`), ResonanceEngine, DspChain.
3. `Engine::loadBank()` — disk I/O na calling vlákně (GUI/main).
4. `AudioDevice::start()` — spustí miniaudio, audio vlákno začne volat callback.
5. `MidiInput::open()` — spustí RtMidi callback vlákno.
6. `log_thr` — spuštěn v `main()` těsně po `ctx.initFromState`.

### Pořadí shutdown (shutdown + main)

1. `log_run = false` → `log_thr.join()`.
2. `ctx.shutdown()`:
   a. `midi.close()` — zastaví RtMidi callback vlákno jako první (už neposílá
      do engine).
   b. `audio->stop()` — zastaví miniaudio, audio vlákno přestane volat callback.
   c. `Logger::clearSubscribers()` — log_buf přestane přijímat.
3. `ctx` destruktor — `Engine::~Engine()`:
   a. `stream_main_->stop()` (join workerů).
   b. `stream_resonance_->stop()` (join workerů).
4. ImGui/GLFW shutdown.

### raw `StreamEngine*` v hласech — proč to není use-after-free

`Voice` i `ResonanceVoice` drží `stream_ = StreamEngine*` (raw pointer). Lifetime
je bezpečná, protože:

- `stream_main_` a `stream_resonance_` jsou `unique_ptr` v `Engine`, který žije
  déle než oba `VoicePool` / `ResonanceEngine` (jsou jeho membery).
- `Engine::~Engine()` nejprve stopuje StreamEngine (join workerů), až poté jsou
  uvolněny Voice objekty (přes VoicePool destruktor).
- `reloadBank` volá `pool_->reset()` a `resonance_->reset()` (hard-stop všech
  hlasů) **před** `loadBank()` — hlasy přestanou sahat na StreamEngine před tím,
  než se cokoli dalšího děje se StreamEngine ringem.

### reloadBank — graceful pause (bank_loading_ guard)

Sekvence v `Engine::reloadBank()` (volat POUZE z non-RT vlákna):

```
1. allNotesOff()         → push AllNotesOff do midi_q_ (non-blocking)
2. sleep 50 ms           → release ramp dobíhá
3. bank_loading_.store(true, release)
4. sleep 10 ms           → in-flight processBlock doběhne (< 6 ms @ 256/48k)
5. pool_->reset()        → hard-stop všech Voice (ring uvolněn)
   resonance_->reset()   → hard-stop všech ResonanceVoice
6. loadBank(dir)         → disk I/O (audio vlákno mlčí)
7. bank_loading_.store(false, release)
```

Audio vlákno na začátku `processBlock` čte `bank_loading_.load(acquire)` — při
`true` vrátí ticho a přeskočí veškerý render.

---

## RT-safety pravidla

### Co se na audio vláknu nesmí

| Zakázaná operace | Riziko |
|---|---|
| `malloc` / `new` / `delete` / libovolná alokace | Latency spike, potenciální deadlock v alokátoru |
| `std::mutex::lock()` (blokující zámek) | Blokování při lock contention |
| Blokující I/O (disk, síť, `fopen`) | Latency spike |
| `std::string` konstruktor z char* (pokud alokuje) | Skrytá alokace (SBO výjimka pro krátké řetězce) |
| `Logger::log()` (non-RT cesta) | Obsahuje mutex + potenciální alokaci |
| `sleep_for` / jakékoli aktivní čekání | Latence |

### Jak to kód dodržuje

- **LOG_RT_\* makra** → `logRT()` / `vlogRT()` — SPSC ring 1024 fixních entry,
  bez alokací, bez zámku, drop-on-full.
- **MidiQueue** — drop-on-full, bez wait.
- **StreamRequestQueue push** — drop-on-full, bez wait.
- **RingHandle::popFrame** — non-blocking, vrátí `false` při prázdném ringu.
- **Předalokované buffery** — `VoicePool` (`vector<Voice>`), ringy (`RingHandle::buf`),
  damping buffer (`Voice::damp_buf_[2*kDampMaxFrames]`) — vše alokováno při init.
- **DspChain** — biquad koeficienty přepočítá lazy v `process()` z atomic paramů;
  žádné alokace.
- **acquireRing / releaseRing** — CAS na `in_use_`, bez alokace.

### Známé výjimky a technické dluhy

1. **`steady_clock::now()` na audio vláknu** — `Engine::noteOnRecent()` a
   `StreamEngine::underrunRecent()` volají `nowMicros()` (steady_clock) z GUI
   vlákna, ale `StreamEngine::noteUnderrun()` — volaný z Voice na audio vláknu
   po underrunu — volá `nowMicrosSE()` (steady_clock) **na audio vláknu**
   (`stream_engine.cpp:240`). `std::chrono::steady_clock::now()` není garantovaně
   RT-safe na všech platformách (macOS: `clock_gettime(CLOCK_UPTIME_RAW)` je OK;
   Linux: závisí na vNDSO). V praxi OK, ale formálně RT porušení.

2. **`log()` (non-RT) na audio vláknu** — `engine.cpp` volá
   `Logger::default_().log(...)` uvnitř `processBlock` při drainování MidiQueue
   (midi_off, midi_cc, AllNotesOff event logy, řádky 174, 188, 199). Tato cesta
   drží `log_mutex_` a potenciálně alokuje string — **RT porušení**.
   `LOG_RT_*` makra jsou připravena, ale event drain používá `log()`.

3. **`StreamRequest::path` kopie na audio vláknu** — `Voice`/`ResonanceVoice`
   v `process()` volají `stream_->requestRead(..., path, ...)`, kde `path` je
   `std::string` z `MicLayer::file.path`. `requestRead` ji kopíruje do
   `StreamRequest::buf_` (`buf_[w % kCap] = r` v push). Kopie `std::string`
   může alokovat (pokud path > SSO ~22 znaků). Komentář v `stream_engine.h:70–76`
   toto uznává jako known issue s poznámkou FUTURE (držet `const char*` z
   dlouhožijícího stringu v Bank).

4. **`cfg_.release_ms` — ne-atomický float** — `Engine::setReleaseMs()` zapisuje
   `cfg_.release_ms` z GUI vlákna; `scaledReleaseMs()` jej čte z audio vlákna.
   Komentář v `engine.cpp:333–338` toto vědomě akceptuje: na x86/arm64 je 4B
   aligned float write de-facto atomický a přechodná hodnota je neslyšitelná.
   Formálně je to UB (data race), i když prakticky bezpečné.

5. **`activeMidiNotes` / `currentGainFor`** — iterují přes `VoicePool::voicesView()`
   z GUI vlákna; `Voice::active()`, `Voice::midi()`, `Voice::currentLevel()` jsou
   ne-atomické fieldy zapisované audio vláknem. Komentář v `engine.cpp:298–300`
   akceptuje „lehký lag". Formálně data race (UB), prakticky benigní na x86/arm64
   (word-tearing nenastane pro bool/int/float).

---

## Křížové odkazy

| Oblast | Dokument | Relevance pro multithreading |
|---|---|---|
| A — Core / scaffold | [A-core.md](A-core.md) | Engine fasáda, `processBlock`, `reloadBank`, log makra |
| B — Zpracování eventů | [B-events.md](B-events.md) | `MidiQueue` SPSC, `PedalState` single-thread, MIDI callback vlákno |
| C — Zpracování bufferu | [C-buffers.md](C-buffers.md) | `StreamEngine` worker thready, `StreamRequestQueue` SP-MC, `RingHandle` SPSC |
| D — Polyfonie | [D-polyphony.md](D-polyphony.md) | `VoicePool` audio-thread only; `Voice` raw StreamEngine* lifetime |
| E — Rezonance | [E-resonance.md](E-resonance.md) | `ResonanceEngine` atomiky (`strength_`, `max_voices_`); `ResonanceVoice` streaming |
| F — Loader | [F-loader.md](F-loader.md) | `loadBank` off-RT (disk I/O); `reloadBank` graceful pause |
| G — DSP | [G-dsp.md](G-dsp.md) | DSP stage atomické parametry; `DspChain.process()` audio-thread only |
| H — GUI | [H-gui.md](H-gui.md) | GUI vlákno; log-flush thread; `LogRingBuffer` mutex; `AppContext` lifecycle |

---

## Nálezy revize

### P1 — Non-RT `log()` na audio vlákně

**Soubor:** `engine/engine.cpp`, řádky ~174, ~188, ~199 (uvnitř `processBlock`).

```cpp
// řádek 174 (NoteOff case v MidiQueue drain):
log::Logger::default_().log("midi_off", log::Severity::Info, ...);
// řádek 188 (Sustain case):
log::Logger::default_().log("midi_cc", log::Severity::Info, ...);
// řádek 199 (AllNotesOff case):
log::Logger::default_().log("midi_off", log::Severity::Info, ...);
```

`Logger::log()` drží `log_mutex_` (potenciálně déle při file I/O) a volá
`nowMicros()` (system_clock). Oba kroky jsou blokující a alokující. Tato vlákna
bežejí uvnitř `processBlock` → RT porušení. **Oprava:** nahradit `LOG_RT_INFO`
resp. `LOG_RT_DEBUG` makry (RT ring je připraven, makra existují).

---

### P1 — `std::string` kopie v `requestRead` na audio vlákně

**Soubor:** `engine/stream/stream_engine.cpp:149–156` + `stream_engine.h:70–76`.

`StreamEngine::requestRead()` je voláno z `Voice::process()` a
`ResonanceVoice::process()` na audio vlákně. Uvnitř kopíruje `path` (std::string)
do `StreamRequest`. Pokud délka path překročí SSO (~15–22 znaků), dojde k `malloc`.
Komentář v hlavičce toto uznává jako FUTURE fix. Reálné banky mají cesty typicky
desítky–stovky znaků → alokace nastává. **Oprava:** `MicLayer::file.path` je
long-lived (žije celý lifetime `Bank`); předat jako `const char*` nebo index.

---

### P2 — `cfg_.release_ms` — data race (formální UB)

**Soubor:** `engine/engine.cpp:339` (write z GUI), `engine.cpp:357` (read z audio).

Zápis `cfg_.release_ms = ms` z GUI vlákna a čtení ve `scaledReleaseMs()` na audio
vláknu je data race dle C++ standardu (UB), i když prakticky neškodné na
x86/arm64. **Oprava:** přidat `std::atomic<float> release_ms_` (vzor existuje:
`master_gain_`). Nit: komentář v kódu toto rozhodnutí dokumentuje a vědomě
akceptuje.

---

### P2 — `activeMidiNotes` / `currentGainFor` — data race (formální UB)

**Soubor:** `engine/engine.cpp:296–328`.

Čtení `Voice::active_`, `Voice::midi_`, `Voice::currentLevel()` (vrací `rel_gain_`
× `vel_gain_`) z GUI vlákna při souběžném zápisu audio vlákna je data race.
`Voice` fields nejsou atomické. Na x86/arm64 word-tearing nenastane pro
zarovnané typy, výsledek je „stale snapshot" — akceptovatelné pro UI, ale
formálně UB. **Oprava:** označit pole `active_`, `midi_`, `rel_gain_` jako
`std::atomic<>` nebo zavést snapshot mechanismus (memcpy pod krátkým spinlockem).

---

### P2 — `steady_clock::now()` v `StreamEngine::noteUnderrun()` na audio vlákně

**Soubor:** `engine/stream/stream_engine.cpp:240–242`.

`noteUnderrun()` je voláno z `Voice::process()` na audio vlákně; interně volá
`nowMicrosSE()` = `steady_clock::now()`. Na Linuxu závisí na vNDSO (obvykle
user-space, RT-safe); na macOS jde na `clock_gettime(CLOCK_UPTIME_RAW)` (RT-safe).
Nit spíše než P2: v praxi OK, ale není to garantováno standardem POSIX.

---

### Nit — `MidiQueue` producent není striktně single-producer

**Soubor:** `engine/midi/midi_queue.h:26–32`.

`MidiQueue` je dokumentována jako SPSC. V praxi mohou `noteOn()` / `noteOff()`
volat souběžně **MIDI callback vlákno** i **GUI vlákno** (např. GUI tlačítko
AllNotesOff + příchozí MIDI event). `w_` je sdílen oběma → není to SPSC, ale
MPSC. Aktuální implementace neserializuje push ze strany producentů → data race
na `buf_[w % MIDI_Q_SIZE] = e` + `w_.store(w+1)` pokud oba thready vidí stejné
`w`. **Oprava:** přidat mutex na push side nebo přejít na skutečnou MPSC
(např. dvojitý CAS). Pravděpodobnost kolize je nízká (MIDI event + GUI souběžně
v jedné mikrosekundě), ale formálně race condition.
