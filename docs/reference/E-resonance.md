# Rezonance

Sympatická rezonance simuluje fyzikální jev, kdy zahrání noty M rozvibruje jiné struny klavíru, jejichž frekvence jsou blízké parciálům M. Engine prochází všech 128 MIDI not N ≠ M; struna N je eligibilní, pokud není tlumena pedálem (`pedal.isUndamped(N)`) a zároveň pro ni neexistuje žádný aktivní hlavní hlas (`!pool.hasActiveMainVoice(N)`) — tím je zachován invariant 5.5.1: na jedné strune v daný okamžik zní buď hlavní, nebo rezonantní hlas, nikdy oba. Síla rezonance se spočte z harmonické blízkosti `harmonicProximity(N, M)` (model partial-coincidence z ideální harmonické řady), velocity a lineárního gainu `gain_lin` (z dB; `excite = vel_norm × harm × gain_lin`). Rezonance je gated `enabled_` (toggle — při vypnutí `onPlayedNoteOn` hned vrací). **Výběr velocity vrstvy** (Fáze 8): místo natvrdo nejtišší `slots[0]` se použije `nearestSlotByRms(NoteSlots, layer_target_db_)` (čistá fce v `resonance_layer_select.{h,cpp}`) — slot, jehož peak `rms_db` je nejblíž uživatelskému cíli (`Resonance Layer` dB). Vzniklý `ResonanceVoice` nehraje vzorek od začátku, ale přeskakuje attack a čte z regionu `preload_resonance` (Streamed mic) nebo od `resonance_start_frame` v `preload_head` (FullyLoaded mic) — proto výběr hlasitější vrstvy nepřináší attack artefakty (skip-attack platí pro Streamed mic). Per-blok `processBlock` aplikuje exponenciální decay na `last_excite` a okamžitě aktualizuje `target_gain` každého aktivního hlasu podle aktuálního `pedal.dampingFor(N)` — změny pedálu (včetně half-pedalu) se tak plynule promítají do hlasitosti rezonance bez nutnosti dalšího note-on eventu. **Mute při uvolnění pedálu** je tím pádem implicitní a nepotřebuje zvláštní hook: jakmile `cc64` klesne do lost-motion dead-zony (`<= kDamperBiteCC`, viz [B-events](B-events.md)), `dampingFor(N)` nedržené struny = 0 → per-blok target = 0 → hlas dohaje na nulu a deaktivuje se. Dead-zona řeší i kontinuální pedál, který nedojede přesně na 0. (Klíčové: `setTargetGain(≤epsilon)` cílí na **pravou nulu** — jinak by se gain ustálil mezi gain- a target-epsilonem a hlas by se nikdy nedeaktivoval = stuck voice; viz `ResonanceVoice::setTargetGain`.)

---

## Implementováno v souborech

| soubor | odpovědnost | klíčové typy |
|--------|-------------|--------------|
| `engine/resonance/resonance_engine.h` | Deklarace koordinátora, invarianty 5.5.1, konstanta budgetu a decay | `ResonanceEngine`, `ExciteState`, `kDefaultMaxResonanceVoices`, `kResonanceExciteMinGain`, `kResonanceHarmonicMin`, `kDefaultExciteDecayMs`, `kResonanceKeyboardSpread` |
| `engine/resonance/resonance_engine.cpp` | Implementace eligibility filtru, budget gate, spawnu/excitace, per-blok update a výběru oběti | `ResonanceEngine` (plná implementace) |
| `engine/resonance/resonance_layer_select.h/.cpp` | Výběr velocity slotu pro rezonanci: slot s `rms_db` nejblíž cíli `target_db` (čistá funkce, testovatelná bez enginu; remíza → nižší index, prázdné slots → -1) | `nearestSlotByRms(const NoteSlots&, float target_db)` |
| `engine/resonance/harmonic_proximity.h` | Deklarace funkcí `harmonicProximity` a `initHarmonicProximity` (off-RT warm-up matice), popis partial-coincidence modelu, ladící parametry | `harmonicProximity(int target_midi, int source_midi)`, `initHarmonicProximity()` |
| `engine/resonance/harmonic_proximity.cpp` | Implementace modelu, výpočet raw coupling, normalizace 128×128 matice (warm-up volá `Engine::init` přes `initHarmonicProximity`) | `rawProx`, `couplingMatrix`, anonymní namespace s konstantami `kPartials`, `kDriveExp`, `kRecvExp`, `kBandwidthCents` |
| `engine/voice/resonance_voice.h` | Deklarace rezonantního hlasu, gain-ramp envelope; streaming přes sdílený `StreamedSampleReader` (viz [C-buffers](C-buffers.md)), v hlasu zůstává jen EOF-hold policy | `ResonanceVoice`, `kResonanceRampMs`, `kResonanceFadeOutMs`, `kResonanceTargetEpsilon`, `kResonanceGainEpsilon` |
| `engine/voice/resonance_voice.cpp` | Implementace start (skip-attack), gain rampy, streaming preload→ring (reader), EOF-hold/underrun policy | `ResonanceVoice` (plná implementace) |

---

## Soubory — přehled funkcí

### `engine/resonance/resonance_engine.h` / `engine/resonance/resonance_engine.cpp`

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) |
|---|---|---|---|---|
| `ResonanceEngine(int max_resonance_voices)` | off-RT (init) | počet hlasů → objekt; **předalokuje všech 128 `ResonanceVoice`** (žádný `make_unique` později na audio vlákně) | `Engine::init` (přes `std::make_unique`) | `setMaxVoices` (clamp + uložení) |
| `setStreamEngine(StreamEngine* se)` | off-RT | pointer na stream engine → void | `Engine::init` | `v->setStreamEngine` na existující hlasy |
| `reset()` noexcept | audio nebo off-RT (reload) | — → void (hard-stop všech hlasů) | `Engine::reloadBank` | `slot->hardStop()`, vynuluje `excite_state_` |
| `setGainDb(float db)` / `gainLinear()` | off-RT/GUI / audio | dB → `gain_lin_` (10^(db/20)) / read | `Engine::setResonanceGainDb` / `onPlayedNoteOn` | — |
| `setLayerTargetDb(float db)` / `layerTargetDb()` | off-RT/GUI / audio | dB cíl pro výběr vrstvy → `layer_target_db_` / read | `Engine::setResonanceLayerDb`; GUI cesta navíc po 400ms debounce v `main.cpp` volá `Engine::rebuildResonanceCache` (ten volá `setLayerTargetDb` znovu + přestaví cache) / `onPlayedNoteOn` (`nearestSlotByRms`) | — |
| `setEnabled(bool)` / `enabled()` | off-RT/GUI / audio | bool → `enabled_` / read | `Engine::setResonanceEnabled` / `onPlayedNoteOn` (early-return) | — |
| `setMaxVoices(int n)` noexcept | off-RT nebo GUI | 1..64 (clamp) → atomic | konstruktor; `Engine::setMaxResonanceVoices` | — |
| `maxVoices()` noexcept | libovolné | — → int (atomic read) | testy, diagnostika | — |
| `setExciteDecayTimeMs(float tau_ms, int block_size, float engine_sr)` | off-RT/GUI | tau [ms], blok, sr → `decay_per_block_` (atomic — čte audio thread v `processBlock`) | `Engine::init`, `Engine::setBlockSize`, `Engine::setExciteDecayMs` | `std::exp(-block_ms / tau_ms)` |
| `exciteDecayPerBlock()` | libovolné | — → float | testy, diagnostika | — |
| `onSelfNoteOn(int played_midi, float engine_sr)` | audio | MIDI noty N, sr → void | MIDI drain inline v `Engine::processBlock` (před `voice_pool.noteOn`) | `slot->fadeOut`, vynuluje `excite_state_[N].last_excite` |
| `onPlayedNoteOn(int played_midi, int velocity, const Bank&, const VoicePool&, const PedalState&, float engine_sr)` | audio | zahraná nota + kontext → spawn/excite rezonancí | MIDI drain inline v `Engine::processBlock` (po `voice_pool.noteOn`) | `harmonicProximity`, `pedal.isUndamped`, `pool.hasActiveMainVoice`, `slot->fadingOut`, `slot->addExcitation`, `slot->currentLevel`/`slot->targetGain` (budget gate), `slot->start`, `enforceVoiceBudget` |
| `processBlock(float* out_l, float* out_r, int n_samples, const PedalState&)` noexcept | audio | výstupní buffery + pedál → bool (some produced) | `Engine::processBlock` | recache fade request → `slot->fadeOut` všech aktivních ne-fading hlasů; `excite_state_[N].last_excite *= decay_per_block_`, `slot->setTargetGain`, `slot->process` |
| `setCacheReady(const std::array<bool,128>&)` / `clearCacheReady()` / `requestRecacheFade()` | off-RT (loadBank / recache bg vlákno) | ready flagy → `reso_cache_ready_` atomics / vše false / atomic fade request | `Engine::loadBank`, bg smyčka `Engine::rebuildResonanceCache` | atomic store `reso_cache_ready_[128]`, `recache_fade_request_` |
| `activeCount()` noexcept | diagnostika | — → int | testy, GUI diag panel | iterace `voices_` |
| `isResonating(int midi)` noexcept | diagnostika | MIDI → bool | testy | `slot->active()` |
| `currentLevelFor(int midi)` noexcept | diagnostika | MIDI → float | testy, GUI | `slot->currentLevel()` |
| `enforceVoiceBudget(float engine_sr)` | audio (privátní) | sr → void (fadeOut oběti) | `onPlayedNoteOn` (po smyčce spawnu) | `liveCount` lambda, `slot->active()`, `slot->fadingOut()`, `slot->currentLevel()`, `slot->fadeOut` |
| `panForNote(int midi, float& pan_l, float& pan_r)` | audio (privátní, static) | MIDI → stereo koeficienty | `onPlayedNoteOn` (před `slot->start`) | `std::cos/sin` se vzorcem z VoicePool |

**Detailní popis klíčových funkcí:**

**`onPlayedNoteOn` — eligibilita + budget gate + spawn/excite**

Funkce iteruje všech 128 MIDI not N a pro každou provádí sérii testů řazených od nejlevnějších po (historicky) nejdražší — ~90 % not vyřadí hned první test; všechny testy jsou dnes O(1):

1. *Harmonický práh:* `harmonicProximity(N, M) < kResonanceHarmonicMin` (0.05) → vynechá (sekunda, tritonus — téměř nulová hodnota).
2. *Excitační práh:* `excite = vel_norm * harm * gain_lin < kResonanceExciteMinGain` (0.001) → vynechá (neslyšitelná rezonance by jen kradla sloty).
3. *Eligibility filter 5.5.1(1) — pedál:* `pedal.isUndamped(N)` musí být true (jinak struna tlumena).
4. *Rule B in-progress:* pokud `slot->active() && slot->fadingOut()`, přeskočí — nová excitace by přerušila probíhající fade-out z rule B.
5. *Eligibility filter 5.5.1(1) — main voice:* `pool.hasActiveMainVoice(N)` musí být false (jinak by rezonance koexistovala s hlavním hlasem na téže struně — porušení invariantu). Záměrně poslední (historicky šlo o jediný test s O(pool) scanem); od feat/async-loader-a-stream-refactor je O(1) přes per-nota čítač `note_active_count_` ve `VoicePool`, takže pořadí už je jen konvence — test je levný.

**RAM cache mód (fáze 8):** `ResonanceEngine` drží per-notě `reso_cache_ready_[128]` (atomic; mimo movable `MicLayer`). `onPlayedNoteOn` přečte `reso_cache_ready_[N]` (acquire) a předá `use_cache` do `slot->start(...)`: `true` → hraj z RAM `preload_resonance` (12 s cílové vrstvy); `false` → **stream mód** (ignoruj cache, streamuj z `resonance_start_frame` přes ring). Při změně „Resonance Layer" slideru běží `Engine::rebuildResonanceCache` přes **stavový automat {running, pending} pod `recache_mtx_`** (opakované pohyby slideru se coalescují přes `pending`; stav GUI čte přes `Engine::recacheInProgress()`). Bg smyčka v **každé iteraci** (i coalescované) volá `clearCacheReady()` (nové hlasy → stream) + `requestRecacheFade()` → `processBlock` na začátku fadene **všechny aktivní ne-fading hlasy** (i stream-mód) → `waitForAudioQuiesce(2, 500)` + 15 ms doběh fade; teprve pak přestaví cache (`buildResonanceCache`) a `setCacheReady()`. Realloc `preload_resonance` je bezpečný — faded hlasy ho nečtou, stream-mód ho ignoruje. Detail stavového automatu viz [A-core](A-core.md).

Před iterací: `if (!enabled_) return;`. Excitace se počítá jako `excite = vel_norm * harm * gain_lin` (kde `gain_lin = 10^(resonance_gain_db/20)`) bez dampingového multiplikátoru — ten se aplikuje až v `processBlock` per-blok přes `setTargetGain`, aby se změny pedálu plynule promítaly do hlasitosti existující rezonance. Sample pro N se vybere `nearestSlotByRms(ns, layer_target_db_)` (nejbližší peak RMS k cíli), pak `variants[0]`, `mics[0]`.

Pokud slot pro N již existuje a je aktivní (per-nota uniqueness 5.5.1(2)), volá `slot->addExcitation(excite)` a aktualizuje `excite_state_[N].last_excite = max(...)`. Nealokuje druhý hlas.

Při spawnu nového hlasu se nejdřív spočte `init_gain = excite * pedal.dampingFor(N)`; pokud `init_gain < kResonanceExciteMinGain` (half-pedal může damping srazit hluboko pod slyšitelnost), `continue` — hlas se vůbec nealokuje (žádný ring, žádná disková čtení). Pak následuje **budget gate**: spočte počet živých (ne-fadingOut) hlasů `live` a najde nejtišší podle `lvl = max(slot->currentLevel(), slot->targetGain())` — čerstvě spawnutý hlas má `gain_` ještě ~0 (rampa ~30 ms), ale target už plný; srovnání čistě přes `currentLevel()` dělalo z právě spawnutých hlasů „nulové oběti". Pokud `live >= cap`:
- Pokud je `init_gain <= qlevel` (nový by nezněl hlasitěji než nejtišší existující) → `continue` — hlas se vůbec nespawnuje.
- Jinak `fadeOut` na nejtišším existujícím hlasu a pokračuje na spawn. Při plném budgetu tak přežívá nejsilnější harmonika.

Tím se eliminuje spawn-churn (dřívější bug: spawn všech harmonik → `acquireRing + requestRead` → okamžitý `fadeOut` přes budget → zbytečná disková čtení hladověla streamující hlasy → underrun i při `MAX_RESONANCE=1`).

Po celé smyčce volá `enforceVoiceBudget` pro dorování při živém snížení slideru `MAX_RESONANCE`.

**`enforceVoiceBudget`**

Počítá jen hlasy, které jsou `active() && !fadingOut()` (živý počet). Klíčová podmínka: hlasy ve fade-out se nepočítají — jinak by jedno překročení budgetu spustilo `fadeOut` na všechny hlasy (po `fadeOut` `active()` zůstává `true` dokud `gain` nedoklesne, takže `activeCount` by neklesal → smyčka by fade-ovala dál, dokud by nezbyl jen fading → rezonance by jen problikla). Smyčka opakovaně hledá nejtišší ne-fadingOut hlas a volá `fadeOut`, dokud `liveCount() <= cap`.

**`onSelfNoteOn` — pravidlo B**

Voláno z MIDI drainu (inline v `Engine::processBlock`) *před* `voice_pool.noteOn(N)`. Pokud `voices_[N]->active() && !voices_[N]->fadingOut()`, spustí `slot->fadeOut(engine_sr)` a vynuluje `excite_state_[N].last_excite`. Tím eligibility filter (1) automaticky zablokuje další excitaci struny N, dokud existuje hlavní hlas N (dokud `pool.hasActiveMainVoice(N)` vrací true).

**`processBlock` — per-blok decay + target update**

Krok 0: pokud bg recache vlákno nastavilo `recache_fade_request_` (atomic exchange), spustí `fadeOut` na **všech aktivních ne-fading hlasech** — bez ohledu na cache/stream mód.

Krok 1: pro každý aktivní ne-fadingOut hlas: `excite_state_[N].last_excite *= decay_per_block_` a `slot->setTargetGain(last_excite * pedal.dampingFor(N))`. Hlasy ve fade-out (rule B nebo `target → 0`) se přeskakují — ty si drží svůj ostrý step po celou dobu fade-out.

Krok 2: volá `slot->process(out_l, out_r, n_samples)` na všech alokovaných slotech; vrátí true pokud kterýkoli produkoval vzorky.

---

### `engine/resonance/harmonic_proximity.h` / `engine/resonance/harmonic_proximity.cpp`

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) |
|---|---|---|---|---|
| `initHarmonicProximity()` | off-RT (`Engine::init`) | — → warm-up 128×128 matice | `Engine::init` (před prvním note-on) | `couplingMatrix()` (vynutí jednorázový build mimo audio vlákno) |
| `harmonicProximity(int target_midi, int source_midi)` | audio (matice už předpočítaná warm-upem v `Engine::init`) | MIDI noty target, source → float [0, 1] | `ResonanceEngine::onPlayedNoteOn` | `couplingMatrix()` (lookup do předpočítané matice) |
| `rawProx(int target, int source)` (anonymní namespace) | off-RT (init) | MIDI noty → float (raw coupling) | `couplingMatrix` lambda při inicializaci | `midiHz`, nested loop přes parciály |
| `couplingMatrix()` (anonymní namespace) | off-RT (první volání = `initHarmonicProximity` z `Engine::init`) | — → const 128×128 matice | `initHarmonicProximity`, `harmonicProximity` | `rawProx` pro všechny páry, normalizace |
| `midiHz(int n)` (anonymní namespace) | off-RT (init) | MIDI nota → Hz (12-TET) | `rawProx` | `440 * pow(2, (n-69)/12)` |

**Partial-coincidence model — detailní popis**

Model počítá sílu rezonance struny N buzené hranou notou P jako součet přes všechny páry parciálů (k-tý parciál P, m-tý parciál N):

```
rawProx(N, P) = Σ_{k=1..K} Σ_{m=1..K}  A(k) · R(m) · exp(-(Δcent / σ)²)
```

kde:
- `K = kPartials = 16` — počet uvažovaných parciálů
- `A(k) = 1/k^a`, kde `a = kDriveExp = 1.0` — energetické váhy parciálů hráté noty P (1. parcial = 1, 2. = 0.5, …)
- `R(m) = 1/m^b`, kde `b = kRecvExp = 2.0` — receptivita m-tého parciálu struny N (`b > a` → up/down asymetrie)
- `Δcent = 1200 · |log₂(k·fP / m·fN)|` — rozladění v centech mezi parciálem k·fP a m·fN
- `σ = kBandwidthCents = 12.0` cent — rezonanční šířka pásma (exp(-1) ≈ 0.37 při Δcent = σ)
- Člen s `g < 1e-4` se přeskakuje (zanedbatelný příspěvek)

**Asymetrie up/down:** Protože `b > a` (2.0 > 1.0), receptivita vyšších parciálů N klesá rychleji než energie parciálů P. Oktáva nahoru (N = P+12): parciál k=1 P padne na parcial m=2 N → `A(1)·R(2) = 1·0.25 = 0.25`. Oktáva dolů (N = P−12): parciál k=2 P padne na parcial m=1 N → `A(2)·R(1) = 0.5·1.0 = 0.5`, ale celkový součet je nižší, protože méně párů parciálů koinciduje. Výsledek po normalizaci: oktáva nahoru ≈ 1.0, oktáva dolů ≈ 0.5 — fyzikálně věrohodné.

**Init a normalizace:** `couplingMatrix()` je function-local static lambda — inicializuje se právě jednou (thread-safe dle C++11). Build je drahý (≈4M operací s `log2f`/`expf`, na RPi5 až ~1 s), proto `Engine::init` volá `initHarmonicProximity()` jako **off-RT warm-up** před prvním použitím — dřívější lazy init při prvním note-onu běžel na audio vlákně a způsoboval deterministický dropout (viz Nálezy revize). Vypočte `rawProx` pro všech 128×128 párů, najde globální maximum a vydělí jím celou matici → oktáva nahoru normalizována na 1.0. Hodnota `rawProx(N, N) = 0` (self — ošetřeno explicitním `if (target == source) return 0.f`).

**Parametry a jejich fyzikální smysl:**

| parametr | konstanta | hodnota | vliv na zvuk |
|----------|-----------|---------|--------------|
| `a` (drive exponent) | `kDriveExp` | 1.0 | strmost poklesu energie parciálů hráté noty |
| `b` (recv exponent) | `kRecvExp` | 2.0 | strmost poklesu receptivity parciálů struny N; `b > a` → up/down asymetrie |
| `σ` (bandwidth) | `kBandwidthCents` | 12.0 ct | šířka okna koincidence; užší = jen unison/oktávy slyšitelně rezonují; širší = více intervalů |
| `K` (partialy) | `kPartials` | 16 | horní mez parciálů; nad 16 příspěvky zanedbatelné pro 12-TET |

---

### `engine/voice/resonance_voice.h` / `engine/voice/resonance_voice.cpp`

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) |
|---|---|---|---|---|
| `setStreamEngine(StreamEngine* se)` | off-RT | pointer → void | `ResonanceEngine::setStreamEngine`, `ResonanceEngine::onPlayedNoteOn` (před `start`) | přímé přiřazení `stream_` |
| `start(int midi, const MicLayer*, float initial_gain, float pan_l, float pan_r, float engine_sr, bool use_cache = true)` | audio | parametry hlasu + cache/stream mód → aktivace | `ResonanceEngine::onPlayedNoteOn` | `reader_.release` (defensive), `reader_.begin` / `reader_.beginEofOnly`, `recomputeGainStep` |
| `addExcitation(float excitation_gain)` | audio | relativní přírůstek gainu → aktualizace target | `ResonanceEngine::onPlayedNoteOn` (existující hlas) | `recomputeGainStep` |
| `setTargetGain(float target)` | audio | absolutní cílový gain → ramp k cíli | `ResonanceEngine::processBlock` | `recomputeGainStep` |
| `fadeOut(float engine_sr)` | audio | sr → ostrý ramp na 0 | `ResonanceEngine::onSelfNoteOn` (rule B), `ResonanceEngine::enforceVoiceBudget`, `ResonanceEngine::onPlayedNoteOn` (budget gate) | nastaví `is_fading_out_=true`, přepočítá `gain_step_` |
| `hardStop()` noexcept | audio/off-RT | — → okamžitá deaktivace | `ResonanceEngine::reset` | `reader_.release` |
| `process(float* out_l, float* out_r, int n_samples)` noexcept | audio | výstupní buffery → bool (active \|\| produced) | `ResonanceEngine::processBlock` | čte preload_resonance nebo ring přes `reader_` (bulk `beginBlock`/`endBlock`, `seed`/`advance`), EOF-hold policy (`eofAcquire`/`holdHiFromLo`/`bumpLoIdx`), gain rampa, `reader_.refill` |
| `recomputeGainStep()` noexcept | audio (privátní) | — → přepočet `gain_step_` | `start`, `addExcitation`, `setTargetGain` | `(target_gain_ - gain_) / ramp_frames_` |
| `active()` | libovolné | — → bool | `ResonanceEngine` (smyčky) | přímý read `active_` |
| `fadingOut()` noexcept | audio | — → bool | `ResonanceEngine::onPlayedNoteOn`, `enforceVoiceBudget`, `processBlock` | přímý read `is_fading_out_` |
| `currentLevel()` noexcept | audio | — → float (`gain_`) | `ResonanceEngine` (budget gate, enforceVoiceBudget) | přímý read `gain_` |
| `targetGain()` noexcept | audio | — → float (`target_gain_`) | `ResonanceEngine::onPlayedNoteOn` (budget gate — `max(currentLevel, targetGain)`; čerstvě spawnutý hlas má `gain_` ~0, ale target už plný) | přímý read `target_gain_` |

**Detailní popis klíčových funkcí:**

**`start` — skip-attack, streaming init, cache vs stream mód**

Nastavuje `position_ = mic_->resonance_start_frame` (FILE-GLOBAL offset) — přeskakuje attack a čte od začátku rezonantní části vzorku. `pos_inc_ = sample_sr / engine_sr` (SR konverze, pitch_ratio = 1.0 vždy — rezonance není transponována). Gain ramp: `gain_ = 0`, `target_gain_ = initial_gain`, `ramp_frames_ = kResonanceRampMs * 0.001 * engine_sr` → fade-in ~30 ms.

Parametr `use_cache` určuje mód: `true` = čti RAM `preload_resonance`; `false` = **stream mód** — `preload_resonance` ani `resonance_frames` se vůbec nečtou (pole právě přepisuje recache bg vlákno), efektivní délka cache `eff_res_frames = use_cache ? resonance_frames : 0`.

`active_` (viabilita) se nastaví na true jen pokud `mic_` je platný a obsahuje data: pro FullyLoaded ověří `head_frames > resonance_start_frame`; pro Streamed `eff_res_frames0 > 0 || file.frames > resonance_start_frame` — stream mód tedy rozhoduje výhradně podle immutable `file.frames`, nikdy podle `resonance_frames` přepisovaného rebuildem.

Pro Streamed mic se streaming inicializuje přes sdílený `StreamedSampleReader` (viz [C-buffers](C-buffers.md)): pokud `total_after = file.frames − res_end <= 0` (rezonanční okno pokrývalo konec souboru), volá se `reader_.beginEofOnly(stream_, res_end)` — žádný request, ring se rovnou označí EOF. Jinak `reader_.begin(stream_, path, res_end, file.frames)` = `acquireRing()` + první request od `res_end = resonance_start_frame + eff_res_frames`; `requestRead` uvnitř vrací bool a při dropu (plná fronta workeru) se `file_request_off_` NEposouvá — `reader_.refill()` v `process()` request zopakuje (no-advance-on-drop). Pokud acquire selže (pool plný, `begin`/`beginEofOnly` vrátí false), hlas dohraje jen preload_resonance a tiše skončí (rezonance je luxus, nekritická jako hlavní hlas).

**`process` — gain ramp + streaming preload→ring + underrun**

Sample-by-sample smyčka pro `n_samples` vzorků. Dvě větve dle `mic_->mode`:

Před smyčkou `reader_.beginBlock()` (bulk čtení ringu: 1 acquire + 1 release za blok místo 3 atomik/vzorek), po smyčce `reader_.endBlock()` — viz [C-buffers](C-buffers.md).

*Streamed:* Nejprve region `preload_resonance` (lokální index `p0 - res_start`, lineární interpolace; ve stream módu je `res_frames = 0`, takže se jde rovnou do ringu). Na hranici `p0 >= res_end - 1` přepne na ring. Seed okna v readeru: `reader_.seed(lo_l, lo_r, res_end − 1)` — lo = poslední frame preload_resonance → plynulý šev, hi = první ring pop. Každý vzorek: `reader_.advance(target = int(position_))` posouvá lo/hi okno. **EOF-hold policy zůstává v hlasu:** při `Advance::RingEmpty` s `reader_.eofAcquire()` drží poslední vzorek (`holdHiFromLo()` + `bumpLoIdx()`) a dojede indexem k `total_frames − 1` → deaktivace u konce souboru; při `RingEmpty` **bez** EOF → **underrun**: nastaví `underrun_fading_ = true`, volá `stream_->noteUnderrun()` a **drží poslední známý vzorek** (`reader_.loL()/loR()`) — underrun fade ho tvaruje (nuly by rampu obešly = tvrdý střih/klik). Po dobu underrunu se přes `underrun_gain_` (krok `underrun_step_` = `-1/(kUnderrunFadeMs * 0.001 * sr)`) multiplicativně ztlumí výstup → `active_ = false` při `underrun_gain_ <= 0`.

*FullyLoaded:* Čte přímo z `preload_head` od `position_` (= res_start), lineární interpolace, `active_ = false` při přesahu `head_frames`.

Gain ramp (společná pro obě větve): `gain_ += gain_step_`, clamp na `target_gain_` (v obou směrech), clamp na 0 (bezpečnost při fadeOut overshoot). Finální výstup: `out_l[i] += sL * env * pan_l_` kde `env = gain_ * underrun_gain_` (pokud underrun).

Deaktivace při fade-out: `is_fading_out_ && gain_ <= kResonanceGainEpsilon (1e-5)` → `active_ = false`, break.

Refill heuristika (Streamed, po smyčce a po `endBlock()`): žije ve sdíleném readeru — `reader_.refill(stream_, path)`. `stream_pending_` se resetuje, jakmile `avail >= capacity/2` (worker prokazatelně dodal data); pokud pak `avail < refillThresholdFrames()` a není čekající request, volá `requestRead` pro další blok dat. `requestRead` vrací bool — při dropu (plná fronta) se `file_request_off_` neposouvá a request se přirozeně zopakuje příští blok (žádná díra v datech). Stejný vzor jako `Voice`.

Při deaktivaci uvolní ring: `reader_.release(stream_)`.

**`setTargetGain` — absolutní nastavení targetu (per-blok z processBlock)**

Nastaví `target_gain_ = target` (clamp na 0). Pokud `target <= kResonanceTargetEpsilon` (1e-4), přepne `is_fading_out_ = true` — hlas po doběhnutí gain rampy k 0 se deaktivuje. Jinak `is_fading_out_ = false`. Vždy volá `recomputeGainStep`.

**`addExcitation` — relativní excitace (multi-source)**

Nový target = `max(target_gain_, gain_ + excitation_gain)` — nová nota může jen zvýšit target. Pokud `new_target > target_gain_`, zruší `is_fading_out_` (rezonance znovu nabuzena) a volá `recomputeGainStep`.

**`fadeOut` — ostrý ramp (rule B, budget)**

`target_gain_ = 0`, `is_fading_out_ = true`. Ostrý step: `gain_step_ = -gain_ / fade_frames`, kde `fade_frames = kResonanceFadeOutMs * 0.001 * engine_sr` (~5 ms). Bezpečnostní clamp: `if (gain_step_ > 0) gain_step_ = 0` (fadeOut nesmí gain zvyšovat).

**`recomputeGainStep` — přepočet rampy**

`gain_step_ = (target_gain_ - gain_) / ramp_frames_`. Při `is_fading_out_` se nevolá z `setTargetGain` (tam se volá), ale komentář v .h říká že při `is_fading_out_` se nepočítá znovu — ve skutečnosti `setTargetGain` vždy volá `recomputeGainStep` bez podmínky; `fadeOut` přepíše `gain_step_` svým vlastním výpočtem a `setTargetGain` se v `processBlock` přeskakuje pro fadingOut hlasy, takže step z `fadeOut` zůstane nedotčen.

---

## Křížové odkazy

- **Events — pedal damping:** `PedalState::isUndamped(N)` a `PedalState::dampingFor(N)` jsou primární vstup eligibility filtru a per-blok aktualizace `target_gain`. Definováno v `engine/pedal/pedal_state.h`; práh `kDampingEpsilon` tam.
- **Polyfonie — `hasActiveMainVoice`:** `VoicePool::hasActiveMainVoice(int midi)` (viz `engine/voice/voice_pool.h`) je druhá podmínka eligibility filtru 5.5.1(1). Zabraňuje koexistenci hlavního a rezonantního hlasu na téže struně. Od feat/async-loader-a-stream-refactor je O(1) přes per-nota čítač `note_active_count_` (viz [D-polyphony](D-polyphony.md)).
- **Buffers — resonance stream pool:** `Engine` drží oddělený `stream_resonance_` (`std::unique_ptr<StreamEngine>`) nezávislý na `stream_main_` pro hlavní hlasy (viz `engine/engine.h`). `ResonanceVoice` na něj sahá přes sdílený `StreamedSampleReader` (`reader_.begin`/`beginEofOnly`/`refill`/`release`, bulk `beginBlock`/`endBlock` — viz [C-buffers](C-buffers.md)). Underrun rezonance neovlivní hlavní pool.
- **Loader — Bank:** `onPlayedNoteOn` přistupuje k `bank.notes[N].slots[nearestSlotByRms(ns, layer_target_db_)].variants[0].mics[0]` pro získání `MicLayer` s `resonance_start_frame`, `resonance_frames`, `preload_resonance`, `preload_head`. Pokud záznam pro N chybí (`!ns.recorded` nebo `nearestSlotByRms` vrátí -1), rezonance pro N nevznikne. Typy definovány v `engine/sample/sample_types.h`.

---

## Nálezy revize

**5.5.1 eligibility — logická správnost:** Filtr je konzistentní. Tři podmínky (damping, `hasActiveMainVoice`, `fadingOut` z rule B) se navzájem doplňují bez mezer. Rule B správně nuluje `last_excite_[N]` — eligibility filter (1) pak skutečně blokuje, protože `processBlock` přestane volat `setTargetGain` na deaktivovaném slotu a při dalším note-on M platí `pool.hasActiveMainVoice(N) = true`.

**Pořadí eligibility testů:** ✅ VYŘEŠENO (branch `fix/revize-2026-06-10`). Dříve běžel O(pool) scan `hasActiveMainVoice` před levnými testy. Nyní pořadí: harmonická blízkost (vyřadí ~90 % not) → excitační práh → `pedal.isUndamped` → `fadingOut` → `hasActiveMainVoice` (dříve jediný ne-O(1) test, proto až nakonec). Nový práh `init_gain < kResonanceExciteMinGain` navíc brání alokaci ringu a diskových čtení pro half-pedalem neslyšitelné hlasy. Aktualizace (feat/async-loader-a-stream-refactor): `hasActiveMainVoice` je nyní O(1) přes per-nota čítač ve `VoicePool` — poznámka o pořadí testů zůstává platná, poslední test je už jen levný.

**Budget gate — srovnání úrovní:** ✅ VYŘEŠENO (branch `fix/revize-2026-06-10`). Původní text tohoto nálezu („srovnání je na stejné škále — správné chování") byl **nesprávný**: `qlevel` se počítal čistě z `currentLevel()`, jenže čerstvě spawnutý hlas má `gain_` ještě ~0 (30ms rampa) — byl tedy vždy „nulovou obětí" a spawn-churn se vracel už uvnitř jediného `onPlayedNoteOn`. Oprava: úroveň oběti = `max(slot->currentLevel(), slot->targetGain())` (nový getter `ResonanceVoice::targetGain`); při plném budgetu přežívá nejsilnější harmonika, ne poslední spawnutá. Hraniční případ zůstává v pořádku: pokud všechny živé hlasy jsou ve fade-out (live = 0 po filtru), `live >= cap` je false — spawn pokračuje bez krádeže.

**Lazy init harmonické matice na audio vlákně:** ✅ VYŘEŠENO (branch `fix/revize-2026-06-10`). Build 128×128 matice (≈4M iterací s `log2f`/`expf`, na RPi5 až ~1 s) běžel lazy při prvním note-onu **na audio vlákně** → deterministický dropout první noty každé session; dřívější tvrzení dokumentu „first call: off-RT lazy init" bylo nepravdivé. Nyní `Engine::init` volá `initHarmonicProximity()` (off-RT warm-up) před prvním použitím — off-RT init je realita.

**`decay_per_block_` bez atomicity:** ✅ VYŘEŠENO (branch `fix/revize-2026-06-10`). Pole zapisuje GUI (`setExciteDecayTimeMs` přes slider) a čte audio thread v `processBlock` — bylo jediné ne-atomic pole mezi settery. Nyní `std::atomic<float>`.

**`make_unique` na audio vlákně:** ✅ VYŘEŠENO (branch `fix/revize-2026-06-10`). `onPlayedNoteOn` dříve lazy-alokoval `ResonanceVoice` přes `std::make_unique` (malloc na audio vlákně). Konstruktor `ResonanceEngine` nyní předalokuje všech 128 hlasů (~20 kB celkem).

**Recache — UAF okno při coalesce:** ✅ VYŘEŠENO (branch `fix/revize-2026-06-10`). Coalescovaná iterace recache smyčky dříve běžela bez `clearCacheReady`/`requestRecacheFade`/quiesce → audio thread mohl číst `preload_resonance` během realokace (UAF okno); dvojice atomiků měla navíc lost-update na `pending`. Nyní stavový automat {running, pending} pod `recache_mtx_`, fade + `waitForAudioQuiesce(2, 500)` + 15 ms doběh v **každé** iteraci; `pending` se vyzvedá pod týmž mutexem. Detail v [A-core](A-core.md).

**`enforceVoiceBudget` — počítání živých hlasů:** Oddělení `active() && !fadingOut()` od celkového `active()` je kriticky důležité (popsáno v implementačním komentáři) a implementace je správná. Lambda `liveCount` se volá znovu v každé iteraci while — O(128 * překročení budgetu), v praxi překročení bývají malá.

**Partial-coincidence model — věrnost:** Model zachycuje fyzikálně relevantní jevy: asymetrie up/down díky `b > a`, šířka pásma 12 ct odpovídá měřeným hodnotám inharmonicity pianových strun. Normalizace na octave-up ≈ 1.0 je konzistentní s požadavkem spec. Potenciální nepřesnost: model ignoruje inharmonicitu (`B = 0`), takže pro nízké noty (kde fyzická inharmonicita je výrazná) může přeceňovat koincidenci vzdálených parciálů. `.h` tuto omezení explicitně dokumentuje jako FUTURE.

**Gain ramp — správnost:** `recomputeGainStep` se volá konzistentně při každé změně `target_gain_` (`start`, `addExcitation`, `setTargetGain`). `fadeOut` přepisuje `gain_step_` vlastním výpočtem a korektem guardí `gain_step_ > 0 → 0`. Clamp v `process` zabraňuje overshoot v obou směrech. Drobnost: `recomputeGainStep` počítá `(target_gain_ - gain_) / ramp_frames_` — pokud se `target_gain_` mění každý blok (per-blok `setTargetGain`), ramp může být zdánlivě kratší než `kResonanceRampMs`, protože každý blok přepočítá step od aktuálního `gain_`. To je záměrné a žádoucí (plynulé sledování pedálu).

**Underrun reakce:** `underrun_fading_` se nastaví jednou a pak multiplicuje gain lineárně k nule (délka ~5 ms = `kUnderrunFadeMs`). Po té `active_ = false`. Hlasy s underrunem nesignalizují streamu pro retry — správně, ring je v neznámém stavu. Bez nálezů.
