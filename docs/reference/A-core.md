# Core / scaffold

Oblast A tvoří páteř celého systému. Třída `Engine` je hlavní fasádou knihovny
`libithaca_core`: drží banku (RAM head + resonance okno), dva oddělené streaming
pooly (`stream_main_`, `stream_resonance_`), pool hlavních hlasů (`VoicePool`),
sympatickou rezonanci (`ResonanceEngine`), DSP řetěz a lock-free MIDI frontu
(`MidiQueue`). Tokový bod orchestrace je `Engine::processBlock`, volaný exkluzivně
z audio vlákna: drainuje MIDI frontu, renderuje hlavní hlasy, přičítá rezonanci,
aplikuje master gain, předává blok DSP řetězu a atomicky aktualizuje peak metr
pro GUI. Thread-safety je řešena kombinací `std::atomic` (gain, peak, příznaky,
časové razítky) a lock-free SPSC MidiQueue — audio vlákno nikdy nezamyká mutex.
`EngineConfig` je jediná konfigurační POD struktura (sample rate, block size,
streaming, rezonance, DSP). Doplňkem jsou: RT-safe logger (SPSC ring pro audio vlákno,
mutex-based pro non-RT volání, subscriber API pro GUI log strip), definice verze
(`ITHACA_VERSION`), CLI entrypoint (`ithaca-cli`) a offline batch renderer.

---

## Implementováno v souborech

| Soubor | Odpovědnost | Klíčové typy |
|--------|-------------|--------------|
| `engine/engine.h` | Deklarace fasády a konfigurace | `Engine`, `EngineConfig` |
| `engine/engine.cpp` | Implementace `Engine` (init, processBlock, reload, diagnostika) | viz h |
| `engine/util/log.h` | RT-safe logger — deklarace + makra | `Logger`, `LogEntry`, `Severity` |
| `engine/util/log.cpp` | Implementace loggeru (non-RT mutex, SPSC RT ring, subscriber) | viz h |
| `engine/util/version.h` | Jediný zdroj verze projektu | `ITHACA_VERSION` (makro) |
| `app/cli/main.cpp` | CLI entrypoint `ithaca-cli` (play/render/inspect/selftest) | `PlayCtx`, `BatchNote` |
| `engine/render/batch_renderer.h` | Deklarace offline stereo WAV rendereru | `BatchNote`, `renderNotes` |
| `engine/render/batch_renderer.cpp` | Implementace sekvenčního renderování not do WAV | viz h |

---

## `engine/engine.h` + `engine/engine.cpp`

### Přehled `EngineConfig`

`EngineConfig` je POD struktura se všemi konfiguračními hodnotami předanými při
inicializaci. Nemá metody. Výchozí hodnoty: SR 48 000 Hz, block 256, max 256
hlavních hlasů, master gain 1.0, release 200 ms, 4 stream workery pro každý pool,
ring 8 192 vzorků, max 32 rezonančních hlasů, rezonance strength 0.5,
excite decay 5 000 ms. Pole `midi_from`/`midi_to` omezuje rozsah načítané banky
(rychlé testy). Separátní ring pooly pro hlavní hlasy a rezonanci (`num_rings` /
`resonance_num_rings`) zabraňují hladovění hlavních hlasů při rezonančním burstu
pod pedalem.

### Tabulka funkcí `Engine`

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|--------------------|--------|----------------|---------|-------------|-----------|------------|
| `Engine()` | off-RT | — | `app/cli/main.cpp`, `app/gui/app_context.cpp` | — | — | Triviální konstruktor; žádná inicializace. |
| `~Engine()` | off-RT | — | destruktor vlastníka | `stream_main_->stop()`, `stream_resonance_->stop()` | — | Zastaví oba stream pooly před destrukcí; hlasy ani banka se explicitně neuvolňují (RAII unique_ptr). |
| `bool init(const EngineConfig& cfg)` | off-RT | `cfg` → `true` (vždy) | `app/cli/main.cpp`, `app/gui/app_context.cpp` | `VoicePool`, `StreamEngine` (×2), `ResonanceEngine`, `dsp_.prepare` | `cfg` — viz `EngineConfig` | Vytvoří VoicePool, oba StreamEngine (přičemž clampuje `num_rings` ≥ `max_voices`), nastaví refill threshold, spustí stream workery, vytvoří ResonanceEngine, uloží `master_gain`, připraví DSP řetěz. Nastaví `initialized_ = true`. Vždy vrací `true` (nikdy nenastane partial-init selhání — disk I/O nenastává zde). |
| `bool loadBank(const std::string& dir)` | off-RT | cesta → `true` pokud načteno ≥1 sample | `reloadBank()`, `app/cli/main.cpp` | `ithaca::loadBank()` (volná funkce z `sample_store`) | `dir` — adresář banky | Deleguje na volnou funkci `ithaca::loadBank` s `cfg_.midi_from/to`, `preload_ms`, `resonance_window_ms`. Výsledek zapíše do `bank_`. Vrátí false pokud `loaded_samples == 0`. |
| `bool reloadBank(const std::string& dir)` | off-RT (blokující ~60 ms) | cesta → `bool` | `app/gui/panel_bank.cpp` | `allNotesOff()`, `pool_->reset()`, `resonance_->reset()`, `loadBank()` | `dir` — nový adresář banky | **Graceful reload** v 7 krocích: (1) push AllNotesOff do MIDI fronty; (2) sleep 50 ms (release ramp); (3) `bank_loading_ = true` (audio vlákno vidí a vrací ticho); (4) sleep 10 ms (in-flight processBlock dobíhá); (5) tvrdě zastaví pool_ i resonance_ (`reset()`), aby žádný Voice nedržel ukazatel do staré banky; (6) `loadBank(dir)` (disk I/O, audio mlčí); (7) `bank_loading_ = false`. Volat **pouze** z non-RT vlákna. |
| `void noteOn(int midi, int velocity)` | GUI / MIDI callback | midi 0–127, vel 1–127 | `app/cli/main.cpp`, `MidiInput::callback`, `batch_renderer` | `midi_q_.push()`, `last_note_on_us_.store()` | `midi` — MIDI číslo noty; `velocity` — 1–127 | Pokud `velocity ≤ 0`, deleguje na `noteOff`. Jinak zapíše časové razítko a vloží `NoteOn` event do lock-free MIDI fronty. Thread-safe. |
| `void noteOff(int midi)` | GUI / MIDI callback | midi 0–127 | `app/cli/main.cpp`, `MidiInput::callback`, `batch_renderer` | `midi_q_.push()`, `last_note_off_us_.store()` | `midi` — MIDI číslo noty | Zapíše časové razítko a vloží `NoteOff` do fronty. Thread-safe. |
| `void allNotesOff()` | GUI / MIDI callback / off-RT | — | `reloadBank()`, `batch_renderer`, `MidiInput::callback` | `midi_q_.push()` | — | Vloží `AllNotesOff` event do fronty. Audio vlákno ho zpracuje nejbližší blok. |
| `void sustainPedal(uint8_t cc)` | MIDI callback | CC64 0–127 | `MidiInput::callback` | `midi_q_.push()` | `cc` — hodnota CC64 (0 = pedál nahoře, 127 = dole) | Vloží `Sustain` event do fronty. Half-pedal: celé rozmezí 0–127 se přenáší spojitě do `PedalState`. |
| `void processBlock(float* out_l, float* out_r, int n_samples) noexcept` | **audio** | L/R scratch buffery (caller nuluje) → zapsány in-place | `AudioDevice` callback (`app_context.cpp`, `main.cpp`) | `midi_q_.pop()`, `pool_->processBlock()`, `resonance_->processBlock()`, `dsp_.process()` | `out_l`, `out_r` — non-interleaved; `n_samples` — velikost bloku | Viz detailní popis níže. |
| `int setBlockSize(int new_block_size) noexcept` | GUI / off-RT | nový block size → clamped hodnota | `app/gui/...` | `recomputeRefillThreshold()`, `resonance_->setExciteDecayTimeMs()`, `dsp_.prepare()` | `new_block_size` — cílová velikost (clamped 32–8192) | Aktualizuje `cfg_.block_size`, přepočítá refill threshold obou stream poolů, synchronizuje decay_per_block konstant v ResonanceEngine, znovu připraví DSP řetěz. Caller je zodpovědný za restart AudioDevice. |
| `StreamEngine* streamEngine()` | GUI | — → ukazatel (může být null před init) | `app/gui/...` (diag) | — | — | Zpětná kompatibilita; vrací `stream_main_.get()`. |
| `int resonanceVoices() const noexcept` | GUI | — → počet | `app/gui/panel_diag.cpp` | `resonance_->activeCount()` | — | Triviální diagnostický getter; null-safe. |
| `int numRingsUsed() const noexcept` | GUI | — → součet ringů obou poolů | `app/gui/...` | `stream_main_->numRingsUsed()`, `stream_resonance_->numRingsUsed()` | — | Součet across obou stream poolů. Separátní gettery `mainRingsUsed()` / `resonanceRingsUsed()` pro jemnější pohled. |
| `int mainRingsUsed() const noexcept` | GUI | — → int | GUI | `stream_main_->numRingsUsed()` | — | Triviální getter; null-safe. |
| `int mainRingsTotal() const noexcept` | GUI | — → int | GUI | `stream_main_->numRings()` | — | Triviální getter; null-safe. |
| `int resonanceRingsUsed() const noexcept` | GUI | — → int | GUI | `stream_resonance_->numRingsUsed()` | — | Triviální getter; null-safe. |
| `int resonanceRingsTotal() const noexcept` | GUI | — → int | GUI | `stream_resonance_->numRings()` | — | Triviální getter; null-safe. |
| `bool mainStreamUnderrunRecent(float ms) const noexcept` | GUI | `ms` → bool | GUI | `stream_main_->underrunRecent(ms)` | `ms` — okno v ms | Deleguje na StreamEngine; null-safe. |
| `bool resonanceStreamUnderrunRecent(float ms) const noexcept` | GUI | `ms` → bool | GUI | `stream_resonance_->underrunRecent(ms)` | `ms` — okno v ms | Null-safe delegát. |
| `uint8_t pedalCC() const noexcept` | GUI | — → CC64 0–127 | `app/gui/...` | `pedal_.sustainCC()` | — | Čtení jednoho bytu; race-free na všech rozumných platformách. |
| `bool noteOnRecent(float ms) const noexcept` | GUI | `ms` → bool | `app/gui/panel_indicators.cpp` | `last_note_on_us_.load()`, `nowMicros()` | `ms` — okno v ms | Porovná `steady_clock` razítko posledního note-on s aktuálním časem. Vrátí false pokud se žádný note-on ještě nestal. |
| `bool noteOffRecent(float ms) const noexcept` | GUI | `ms` → bool | `app/gui/panel_indicators.cpp` | `last_note_off_us_.load()`, `nowMicros()` | `ms` — okno v ms | Analogicky pro note-off. |
| `void activeMidiNotes(bool out[128]) const noexcept` | GUI | — → bool[128] | `app/gui/...` (klávesnice) | `pool_->voicesView()` | `out` — výstupní maska | Iteruje view hlasů (bez locku, „lehký lag" tolerance), nastaví `out[midi] = true` pro každý aktivní hlas. |
| `void resonatingMidiNotes(bool out[128]) const noexcept` | GUI | — → bool[128] | `app/gui/...` (klávesnice) | `resonance_->isResonating(n)` | `out` — výstupní maska | Analogicky pro rezonující hlasy. |
| `float currentGainFor(int midi) const noexcept` | GUI | midi → 0..1 | `app/gui/...` (klávesnice alpha) | `pool_->voicesView()` | `midi` — MIDI číslo | Vrátí max `currentLevel` přes všechny hlasy dané noty. Range check: mimo 0–127 vrací 0. |
| `void setMasterGain(float g)` | GUI | g (lin) | `app/gui/voice_page.h`, `panel_topbar.cpp` | `master_gain_.store()` | `g` — lineární gain | Atomický store, memory_order_relaxed. |
| `void setReleaseMs(float ms) noexcept` | GUI | ms → (clamped 1..60000) | `app/gui/voice_page.h`, `panel_topbar.cpp` | `cfg_.release_ms =` | `ms` — release v ms | Zápis do plain float; race s audio vláknem benigní na x86/arm (4-byte aligned write). |
| `void setResonanceStrength(float s) noexcept` | GUI | s 0..1 | `app/gui/voice_page.h`, `panel_topbar.cpp` | `resonance_->setStrength(s)` | `s` — 0 (žádná) .. 1 (max) | Deleguje; ResonanceEngine si sám clampuje rozsah. |
| `void setExciteDecayMs(float ms) noexcept` | GUI | ms | `app/gui/...` | `resonance_->setExciteDecayTimeMs()` | `ms` — tau v ms | Synchronizuje `decay_per_block` v ResonanceEngine. |
| `void setMaxResonanceVoices(int n) noexcept` | GUI | n | GUI | `resonance_->setMaxVoices(n)` | `n` — hard cap | Přímá delegace; null-safe. |
| `int maxResonanceVoices() const noexcept` | GUI | — → int | GUI | `resonance_->maxVoices()` | — | Null-safe getter. |
| `float masterPeakL() const noexcept` | GUI | — → 0..∞ | GUI (peak metr) | `master_peak_l_.load()` | — | Atomický load, relaxed. |
| `float masterPeakR() const noexcept` | GUI | — → 0..∞ | GUI (peak metr) | `master_peak_r_.load()` | — | Analogicky pro pravý kanál. |
| `float dspLoadPeak() const noexcept` | GUI | — → 0..∞ | GUI (DSP LOAD dlaždice) | `dsp_load_peak_.load()` | — | Peak-hold zatížení audio threadu = čas renderu / perioda bloku. 1.0 = hranice deadline. Relaxed. |
| `bool overloadRecent(float ms) const noexcept` | GUI | ms → bool | GUI (červené blikání DSP LOAD) | `nowMicros()` | `ms` = okno | `true` když overload (load ≥ 1.0) nastal před méně než `ms`. Vzor `noteOnRecent`. |
| `dsp::DspChain& dspChain() noexcept` | GUI / off-RT | — → ref | GUI (params panel) | — | — | Přímý přístup ke DSP řetězu pro ovládání stage z GUI. |
| `void recomputeRefillThreshold() noexcept` | off-RT / `setBlockSize` | — | `init()`, `setBlockSize()` | `stream_main_->setRefillThresholdFrames()`, `stream_resonance_->...` | — | Vypočítá refill threshold = max(cap/2, block_size×4), clamped na (cap−64). Volá se pro oba stream pooly. Zajišťuje, že workery doplňují ring dřív, než ho audio vlákno vyprázdní. |
| `float scaledReleaseMs() const` | **audio** (v processBlock) | — → ms | `processBlock()` | `pedal_.sustainCC()` | — | Half-pedal kontinuální škálování release: `kf = exp(t × ln(20))`, kde `t = CC64 / 127`. CC0 → ×1, CC64 → ~×4, CC127 → ×20. Volána při každém NoteOff a AllNotesOff v drain smyčce. |

#### `processBlock` — detailní popis

Jde o centrální audio-RT funkci. Volá ji callback `AudioDevice` (~48 000/256 ≈ 187× za sekundu). Funkce musí být `noexcept` a nesmí alokovat ani zamykat mutex.

**Krok 0 — bank reload guard:** Pokud `bank_loading_.load(acquire)` je `true`, oba buffery se vynulují (`memset`), peak metr se resetuje na 0 a funkce se okamžitě vrátí. Kontrakt výstupu je tak vždy platný (nulová data místo náhodného obsahu).

**Krok 1 — drain MIDI fronty:** Iteruje `midi_q_.pop(e)` dokud jsou události. Pořadí zpracování odpovídá spec 5.5.1:
- `NoteOn (vel=0)` → MIDI konvence NoteOff: `pedal_.noteOff()`, `pool_->noteOff()`.
- `NoteOn (vel>0)` → pravidlo B **před** voice noteOn: `resonance_->onSelfNoteOn(m, sr)` (zastaví případnou probíhající rezonanci na téže notě), poté `selectVoice(bank_, m, v, rr_)` (round-robin výběr samplu), `pool_->noteOn(...)`. **Po** voice noteOn: `resonance_->onPlayedNoteOn(...)` (eligibilita vidí nový aktivní hlas).
- `NoteOff` → logguje, `pedal_.noteOff(m)`, `pool_->noteOffWithPedal(m, pedal_, ...)`.
- `Sustain` → logguje, `pedal_.setSustainCC(cc)`. Při přechodu DOWN→UP: `pool_->releasePendingNotes(...)`.
- `AllNotesOff` → logguje, `pedal_.allNotesOff()`, `pool_->allNotesOff(cfg_.release_ms, sr)`.

**Krok 2 — render hlasů:** `pool_->processBlock(out_l, out_r, n_samples, sr)` (sčítá hlavní hlasy in-place). Poté `resonance_->processBlock(out_l, out_r, n_samples, pedal_)` přičítá sympaticky rezonující hlasy.

**Krok 3 — master gain:** Gain se čte atomicky. Aplikuje se pouze pokud `|g − 1| > 0.001` (optimalizace: skip scalar loop pro gain ≈ 1).

**Krok 3b — DSP řetěz:** `dsp_.process(out_l, out_r, n_samples)` — AGC → BBE → Limiter; vypnutá stage = no-op.

**Krok 4 — peak metr:** Najde `abs` peak v bloku, kombinuje s exponenciálně klesajícím předchozím peak: `new = max(peak_now, cur × decay)`, kde `decay = exp(−n_samples / (0.1 × SR))`. Pro 256 vzorků @ 48 kHz ≈ 0.948 na blok (pokles z 1.0 na 0.1 za ~150 ms). Výsledek se uloží atomicky (relaxed) do `master_peak_l_` / `master_peak_r_`.

**Krok 5 — DSP load metr (Fáze 8):** `block_t0` se zachytí `nowMicros()` hned po Kroku 0 (bank guard); na konci se spočte `dt = nyní − block_t0`, `period_us = n_samples × 1e6 / sample_rate` (guard proti dělení nulou při SR ≤ 0), `load = dt / period`. Peak-hold s decay `exp(−n_samples / (0.5 × SR))` → `dsp_load_peak_` (relaxed). Při `load ≥ 1.0` (blok minul deadline) se orazítkuje `last_overload_us_`. GUI to čte přes `dspLoadPeak()` / `overloadRecent()` a kreslí jako dlaždici DSP LOAD v indicator stripu. Viz oblast C (Buffery) pro souvislost s runtime změnou bufferu.

---

## `engine/util/log.h` + `engine/util/log.cpp`

### Přehled

RT-safe logger se dvěma oddělnými cestami: non-RT (mutex, printf-style, přímý zápis do konzole/souboru + notifikace subscriberů) a RT (SPSC ring buffer 1 024 slotů, žádné alokace, žádný mutex; audio vlákno nikdy neblokuje). Subscriber API slouží GUI log strip panelu. Singleton `default_()` je sdílen napříč celou knihovnou přes `LOG_*` makra.

### Typy

- `Severity` — `Debug(0)`, `Info(1)`, `Warning(2)`, `Error(3)`, `Fatal(4)`, `Off(5)`. Filtr: `sev ≥ min_severity_`.
- `LogEntry` — strukturovaný event (timestamp_us, topic, sev, message) předávaný subscriberům.
- Vnitřní `Entry` — fixně alokovaný záznam v RT ring bufferu (component[32], message[256]).

### Tabulka funkcí `Logger`

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|--------------------|--------|----------------|---------|-------------|-----------|------------|
| `static Logger& default_()` | libovolné | — → singleton ref | všichni volající `LOG_*` maker, `engine.cpp`, `batch_renderer.cpp`, `app/cli/main.cpp` | — | — | Meyers singleton; thread-safe inicializace v C++11+. |
| `void setMinSeverity(Severity s)` | GUI / off-RT | `s` | `app/cli/main.cpp`, `app/gui/app_context.cpp`, `panel_topbar.cpp` | `min_severity_.store()` | `s` — minimální severity pro průchod filtrem | Atomický store relaxed; efekt okamžitý (nezamyká). |
| `Severity getMinSeverity() const` | libovolné | — → Severity | GUI | `min_severity_.load()` | — | Atomický load relaxed. |
| `void setOutputMode(bool useConsole, bool useFile)` | off-RT | useConsole, useFile | `app/cli/main.cpp` | `use_console_.store()`, `use_file_.store()` | — | Atomické store; bez locku. `useFile` má efekt jen pokud byl dříve otevřen soubor přes `setLogFile`. |
| `bool setLogFile(const std::string& path)` | off-RT | cesta → `true` pokud otevřeno | `app/cli/main.cpp` | `log_mutex_`, `log_file_.open()` | `path` — cílový soubor (append mode) | Zamkne `log_mutex_`, zavře předchozí soubor, otevře nový v append módu. |
| `void closeLogFile()` | off-RT | — | destruktor | `log_mutex_`, `log_file_.close()` | — | Zamkne `log_mutex_` a zavře soubor. |
| `void log(const char* component, Severity severity, const char* format, ...)` | non-RT (GUI/off-RT) | component, sev, format+args | `engine.cpp`, `voice_pool.cpp`, `stream_engine.cpp`, `batch_renderer.cpp`, `app/cli/main.cpp`, a další | `vlog()` | `component` — tag (napr. "midi_off"); `severity`; `format` — printf fmt string | Varargs wrapper nad `vlog`. GCC/Clang format attribute pro compile-time kontrolu. |
| `void vlog(const char* component, Severity severity, const char* format, va_list args)` | non-RT | va_list → výstup na konzoli/souboru | `log()` | `shouldLog()`, `writeEntry()`, subscriber notify | — | Filtruje severity, formátuje do `buf[256]`, drží `log_mutex_` jen na dobu `writeEntry` (I/O), pak pod `subscriber_mtx_` notifikuje subscribery. Double-check `subscribers_.empty()` bez locku jako fast path. |
| `void logRT(const char* component, Severity severity, const char* format, ...)` | **audio (RT)** | component, sev, format+args | `resonance_engine.cpp`, `resonance_voice.cpp` (přes `LOG_RT_*` makra) | `vlogRT()` | dtto jako log | RT varargs wrapper. |
| `void vlogRT(const char* component, Severity severity, const char* format, va_list args)` | **audio (RT)** | va_list → SPSC ring | `logRT()` | atomické load/store indexů, `vsnprintf` do slotu | — | SPSC: jeden producent (audio vlákno). Zkontroluje fill (`w − r ≥ 1024` → zahození + `rt_dropped_++`). Jinak formátuje přímo do `rt_buffer_[w % 1024]`, zapíše `timestamp_us`, poté `rt_write_idx_.store(w+1, release)` — release páruje s acquire ve `flushRTBuffer`. Žádná alokace, žádný mutex, žádné blokování. |
| `int flushRTBuffer()` | non-RT (log worker thread) | — → počet flushed | `app/cli/main.cpp` (log_thr ~100 Hz), `app/gui/app_context.cpp` | `log_mutex_`, `writeEntry()` | — | Zamkne `log_mutex_`, načte `rt_write_idx_` (acquire), iteruje od `rt_read_idx_` a volá `writeEntry` pro každý slot. Uloží nový `rt_read_idx_` (release). Vrátí počet flushed zpráv. **Nenotifikuje subscribery** (jen tiskne). |
| `uint64_t rtDroppedCount() const` | GUI / diagnostika | — → uint64 | GUI (diag panel) | `rt_dropped_.load()` | — | Počet zahozených RT zpráv kvůli plnému ring bufferu. |
| `void addSubscriber(Subscriber s)` | GUI / off-RT | callback | `app/gui/app_context.cpp` | `subscriber_mtx_` | `s` — `std::function<void(LogEntry const&)>` | Thread-safe přidání subscribera. Voláno z GUI threadu před `engine.init()`. |
| `void clearSubscribers()` | GUI / off-RT | — | `app/gui/app_context.cpp` (shutdown) | `subscriber_mtx_` | — | Thread-safe vymazání všech subscriberů. |
| `bool shouldLog(Severity s) const` | libovolné | sev → bool | `vlog()`, `vlogRT()` | `min_severity_.load()` | — | Soukromá filtrační funkce; atomický load relaxed. |
| `void writeEntry(...)` | non-RT (pod `log_mutex_`) | component, sev, msg, timestamp → výstup | `vlog()`, `flushRTBuffer()` | `use_console_.load()`, `use_file_.load()`, `fputs`, `log_file_` | — | Sestaví řádek `[HH:MM:SS.mmm] [component] [SEV]: message\n`. Warning+ jde na `stderr`, ostatní na `stdout`. Pokud `use_file_` a soubor otevřen, zapíše i tam (s flush). |
| `static uint64_t nowMicros()` | libovolné | — → us | `vlog()`, `vlogRT()` | `system_clock::now()` | — | Wall-clock mikrosekund od epochy. **Poznámka:** `vlogRT` volá `nowMicros()` uvnitř RT cesty — `system_clock` může na Linuxu přes `VDSO` být O(100 ns), ale volání stále není real-time guaranteed (může syscall). |
| `static std::string Logger::formatTimestamp(uint64_t micros)` | non-RT | micros → "HH:MM:SS.mmm" | `writeEntry()` | `localtime_r` / `localtime_s`, `snprintf` | — | Lokální čas; alokuje `std::string` — vhodné jen pro non-RT cestu. |
| `severity_to_string(Severity s)` | libovolné | Severity → const char* | `writeEntry()` | — | — | Switch; vrátí string literál. Při neplatném enum vrátí "INFO". |
| `severity_from_string(const char* s, Severity default_value)` | off-RT | string → Severity | `app/cli/main.cpp` | `std::tolower()` | `s` — vstupní řetězec; `default_value` | Case-insensitive parse; null-safe (vrátí `default_value` pro null). |

---

## `engine/util/version.h`

Jeden řádek: `#define ITHACA_VERSION "0.1.0-faze1"`. Jediný zdroj pravdy verze projektu. Zahrnují ho: `app/cli/main.cpp` (pro `--version` výstup a usage banner). Jiné soubory v `engine/` verzi nepoužívají.

---

## `app/cli/main.cpp`

### Přehled

Tenký headless CLI wrapper nad `libithaca_core`. Parsuje argumenty, konfiguruje logger, pak spustí jeden ze čtyř módů: `--selftest`, `--inspect`, `--render`, `--play`. Neimplementuje žádnou audio-RT logiku sám — audio vlákno obsluhuje lokální callback `playAudioCb`.

### Tabulka funkcí

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|--------------------|--------|----------------|---------|-------------|-----------|------------|
| `void playAudioCb(void* userdata, float* output, uint32_t frames)` | **audio** | userdata (PlayCtx*), interleaved output buffer, frames → vyplněný output | `AudioDevice` callback | `engine->processBlock(L, R, frames)`, interleave L+R | `userdata` — ukazatel na `PlayCtx`; `output` — interleaved stereo; `frames` — blok | Statický scratch `L`/`R` (resize při prvním volání nebo při změně velikosti bloku), volá `processBlock`, pak ručně interleave L/R do output bufferu. |
| `static void printUsage(const char* argv0)` | main | — → stdout | `main()` | `printf` | `argv0` — název programu | Vypíše kompletní usage banner s `ITHACA_VERSION`. |
| `int main(int argc, char* argv[])` | main | CLI args → exit code | OS | viz níže | — | Viz detailní popis níže. |

#### `main` — detailní popis

**Parsování argumentů:** Jednoduchý lineární scan `argv`. Rozpoznává: `--version`, `--help/-h`, `--log-level`, `--selftest`, `--inspect`, `--render`, `--out`, `--play`, `--midi-in`, `--midi-list`, `--block-size`, `--resonance-strength`, `--log-file`. Neznámá volba → stderr + usage + exit 1.

**Konfigurace loggeru:** `setMinSeverity`, volitelně `setLogFile` + `setOutputMode`. Konzole zůstává aktivní i při file logu.

**Mód `--midi-list`:** Volá `MidiInput::listPorts()`, vypíše indexy a jména portů, exit 0.

**Mód `--play`:** Vytvoří `Engine`, inicializuje `EngineConfig` (rozsah 21–108, block_size z CLI), `loadBank`. Spustí `AudioDevice` s callbackem `playAudioCb`. Volitelně otevře MIDI vstup přes `MidiInput` (index / virtual / substring name match). Bez MIDI zahraje testovací akord C-E-G (MIDI 60, 64, 67). Spustí background log flush thread (100 Hz, `flushRTBuffer()` každých 10 ms). Čeká na Enter, pak orderly shutdown: `log_run=false`, join, `midi.close()`, `dev.stop()`.

**Mód `--render`:** Vytvoří `Engine` s rozsahem 57–72 (okolí středu klaviatury), `loadBank`, zavolá `renderNotes(eng, [{60,100,1s},{64,100,1s},{67,100,1s}], render_out, tail=0.5s)`. Exit code dle výsledku.

**Mód `--inspect`:** Přímo volá volnou funkci `loadBank(inspect_dir, L)` (bez Engine, bez streaming). Vypíše: formát banky, počet not se samply, celkový počet samplu, frames, RAM (MB). Pro první notu se samply vypíše detail slotů (RMS, attack_end).

**Mód `--selftest`:** Cvičná sada `LOG_INFO/DEBUG/WARN/RT_INFO`, flush ring bufferu, ověří `flushed ≥ 1`. Exit 0 = OK, exit 1 = selhání.

---

## `engine/render/batch_renderer.h` + `engine/render/batch_renderer.cpp`

### Přehled

Offline single-threaded renderer not do stereo WAV souboru. Nepoužívá `AudioDevice`; volá `Engine::processBlock` přímo v pevném bloku 512 vzorků. Výstup akumuluje jako interleaved float vektor, na konci zapíše přes `writeWavStereo16`. Slouží pro `make smoke` testy a pro `--render` mód CLI.

### Tabulka funkcí

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|--------------------|--------|----------------|---------|-------------|-----------|------------|
| `int renderNotes(Engine& engine, const std::vector<BatchNote>& notes, const std::string& out_path, float tail_s = 0.5f)` | off-RT (main) | engine ref, seznam not, cesta k WAV, tail → počet zpracovaných not | `app/cli/main.cpp` (`--render` mód) | `engine.allNotesOff()`, `engine.noteOn()`, `engine.processBlock()`, `engine.noteOff()`, `writeWavStereo16()` | `engine` — aktivní engine s načtenou bankou; `notes` — seznam `{midi, velocity, duration_s}`; `out_path` — výstupní WAV; `tail_s` — dozvuk po note-off (výchozí 0.5 s) | Pro každou notu: `allNotesOff()` (vyčistí předchozí hlas), `noteOn`, sustain smyčka po `duration_s` v blocích po 512 vzorcích (s nulovými L/R buffery před každým blokem), `noteOff`, tail smyčka po `tail_s`. Výstup se interleave a appenduje do `out` vektoru. Po všech notách `writeWavStereo16` zapíše WAV; selhání → log error, return 0. Vrátí počet zpracovaných not (= `notes.size()` pokud vše OK). |

`BatchNote` je jednoduchá POD struktura `{int midi, int velocity, float duration_s}` bez metod.

---

## Křížové odkazy

| Oblast | Dokument | Vztah k Core |
|--------|----------|--------------|
| B — Zpracování eventů | [B-events.md](B-events.md) | `MidiQueue`, `MidiInput`, `PedalState` — drainuje je `Engine::processBlock`; `MidiInput` volá `Engine::noteOn/Off/sustainPedal/allNotesOff` |
| C — Zpracování bufferu | [C-buffers.md](C-buffers.md) | `AudioDevice` volá `playAudioCb` → `Engine::processBlock`; `StreamEngine` (×2) drží streaming ringy, Engine volá `start()/stop()/setRefillThresholdFrames()` |
| D — Polyfonie | [D-polyphony.md](D-polyphony.md) | `VoicePool` vlastní Engine; `selectVoice`/`patch_manager` vybírá `VoiceSpec` pro `noteOn`; `pool_->processBlock()` renderuje hlavní hlasy |
| E — Rezonance | [E-resonance.md](E-resonance.md) | `ResonanceEngine` vlastní Engine; Engine ho volá v drain smyčce (`onSelfNoteOn`, `onPlayedNoteOn`) a v render fázi (`processBlock`); separátní `stream_resonance_` |
| F — Loader | [F-loader.md](F-loader.md) | Volná funkce `ithaca::loadBank()` naplní `Bank`; Engine ji volá v `loadBank()` a `reloadBank()` |
| G — DSP | [G-dsp.md](G-dsp.md) | `DspChain` je členem Engine; `init`/`setBlockSize` volá `dsp_.prepare()`; `processBlock` volá `dsp_.process()` |
| H — GUI | [H-gui.md](H-gui.md) | `app/gui/app_context.cpp` drží `Engine` instanci; GUI panely volají Engine settery; `addSubscriber` napojí log strip na `Logger` |
| I — Multithreading | [I-multithreading.md](I-multithreading.md) | Veškerá mezivláknová synchronizace Core: `bank_loading_` flag, `MidiQueue` SPSC, atomické gettery, SPSC RT log ring |

---

## Nálezy revize

**P1 — `nowMicros()` v RT cestě (`vlogRT`) není real-time guaranteed:**
`Logger::nowMicros()` volá `std::chrono::system_clock::now()`, které může na Linuxu/macOS mimo VDSO přejít do syscallu a tím zablokovat audio vlákno. Správné řešení pro RT cestu: `CLOCK_MONOTONIC_RAW` nebo `clock_gettime` s VDSO zárukou, případně předat timestamp jako parametr z neRT kontextu.

**P1 — `reloadBank` nedostatečný guard při `bank_loading_` flagu:**
`reloadBank` nastaví `bank_loading_ = true` (krok 3) a pak spí 10 ms (krok 4). Pokud audio vlákno právě vstoupilo do `processBlock` *těsně před* store (vidělo `false`), může dobíhat celý blok se starou bankou. Zároveň `pool_->reset()` a `resonance_->reset()` (krok 5) se volají hned po spánku — ale voice, který prochází `processBlock`, stále drží ukazatele do staré banky a přistupuje k nim. Sleep 10 ms zajišťuje jen statistickou bezpečnost (ne garantovanou). Správné řešení: synchronizační bariéra (mutex/seqlock) namísto sleep.

**P2 — Subscriber callbacky nejsou volány z RT cesty (`vlogRT` / `flushRTBuffer`):**
Komentář v log.h říká, že subscribery nejsou volány z RT cesty a jejich napojení na flush by se přidalo zvlášť. Aktuálně `flushRTBuffer` jen tiskne (`writeEntry`), ale nenotifikuje subscribery. GUI log strip tak nevidí zprávy z audio vlákna (LOG_RT_*). Pokud je to záměr, měl by být zdokumentován jako kontrakt; jinak jde o chybějící funkci.

**P2 — `playAudioCb` používá `static` scratch buffery:**
`static std::vector<float> L, R` v `playAudioCb` znamená, že pokud by existovaly dvě instance `Engine` v témže procesu (například testovací scénář), sdílely by scratch — race condition. Pro CLI je to akceptovatelné (jedna instance), ale jde o skrytou spřaženost.

**Nit — `setReleaseMs` bez atomiku s komentářem „benigní race":**
Komentář v engine.cpp záměrně vysvětluje, proč plain float write postačuje. Je to srozumitelné, ale závisí na platformově specifickém chování (4-byte aligned write = atomic na x86/ARM). Přidání `std::atomic<float>` by bylo explicitnější bez měřitelné režie.

**Nit — blok 512 v `renderNotes` ignoruje `engine.blockSize()`:**
`batch_renderer.cpp` pevně kóduje `block = 512`. Pokud byl Engine inicializován s jiným `block_size` (např. 256), DSP řetěz a rezonance jsou připraveny na jiný block size než dostávají. V praxi DSP stage to zvládá (process přijme libovolný n_samples), ale nesoulad může maskovat problémy závislé na block size.
