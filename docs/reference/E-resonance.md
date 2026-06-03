# Rezonance

Sympatická rezonance simuluje fyzikální jev, kdy zahrání noty M rozvibruje jiné struny klavíru, jejichž frekvence jsou blízké parciálům M. Engine prochází všech 128 MIDI not N ≠ M; struna N je eligibilní, pokud není tlumena pedálem (`pedal.isUndamped(N)`) a zároveň pro ni neexistuje žádný aktivní hlavní hlas (`!pool.hasActiveMainVoice(N)`) — tím je zachován invariant 5.5.1: na jedné strune v daný okamžik zní buď hlavní, nebo rezonantní hlas, nikdy oba. Síla rezonance se spočte z harmonické blízkosti `harmonicProximity(N, M)` (model partial-coincidence z ideální harmonické řady), velocity a lineárního gainu `gain_lin` (z dB; `excite = vel_norm × harm × gain_lin`). Rezonance je gated `enabled_` (toggle — při vypnutí `onPlayedNoteOn` hned vrací). **Výběr velocity vrstvy** (Fáze 8): místo natvrdo nejtišší `slots[0]` se použije `nearestSlotByRms(NoteSlots, layer_target_db_)` (čistá fce v `resonance_layer_select.{h,cpp}`) — slot, jehož peak `rms_db` je nejblíž uživatelskému cíli (`Resonance Layer` dB). Vzniklý `ResonanceVoice` nehraje vzorek od začátku, ale přeskakuje attack a čte z regionu `preload_resonance` (Streamed mic) nebo od `resonance_start_frame` v `preload_head` (FullyLoaded mic) — proto výběr hlasitější vrstvy nepřináší attack artefakty (skip-attack platí pro Streamed mic). Per-blok `processBlock` aplikuje exponenciální decay na `last_excite` a okamžitě aktualizuje `target_gain` každého aktivního hlasu podle aktuálního `pedal.dampingFor(N)` — změny pedálu (včetně half-pedalu) se tak plynule promítají do hlasitosti rezonance bez nutnosti dalšího note-on eventu. **Mute při uvolnění pedálu** je tím pádem implicitní a nepotřebuje zvláštní hook: jakmile `cc64` klesne do lost-motion dead-zony (`<= kDamperBiteCC`, viz [B-events](B-events.md)), `dampingFor(N)` nedržené struny = 0 → per-blok target = 0 → hlas dohaje na nulu a deaktivuje se. Dead-zona řeší i kontinuální pedál, který nedojede přesně na 0. (Klíčové: `setTargetGain(≤epsilon)` cílí na **pravou nulu** — jinak by se gain ustálil mezi gain- a target-epsilonem a hlas by se nikdy nedeaktivoval = stuck voice; viz `ResonanceVoice::setTargetGain`.)

---

## Implementováno v souborech

| soubor | odpovědnost | klíčové typy |
|--------|-------------|--------------|
| `engine/resonance/resonance_engine.h` | Deklarace koordinátora, invarianty 5.5.1, konstanta budgetu a decay | `ResonanceEngine`, `ExciteState`, `kDefaultMaxResonanceVoices`, `kResonanceExciteMinGain`, `kResonanceHarmonicMin`, `kDefaultExciteDecayMs`, `kResonanceKeyboardSpread` |
| `engine/resonance/resonance_engine.cpp` | Implementace eligibility filtru, budget gate, spawnu/excitace, per-blok update a výběru oběti | `ResonanceEngine` (plná implementace) |
| `engine/resonance/harmonic_proximity.h` | Deklarace funkce `harmonicProximity`, popis partial-coincidence modelu, ladící parametry | `harmonicProximity(int target_midi, int source_midi)` |
| `engine/resonance/harmonic_proximity.cpp` | Implementace modelu, výpočet raw coupling, normalizace a lazy-init 128×128 matice | `rawProx`, `couplingMatrix`, anonymní namespace s konstantami `kPartials`, `kDriveExp`, `kRecvExp`, `kBandwidthCents` |
| `engine/voice/resonance_voice.h` | Deklarace rezonantního hlasu, gain-ramp envelope, streaming interface | `ResonanceVoice`, `kResonanceRampMs`, `kResonanceFadeOutMs`, `kResonanceTargetEpsilon`, `kResonanceGainEpsilon` |
| `engine/voice/resonance_voice.cpp` | Implementace start (skip-attack), gain rampy, streaming preload→ring, underrun fade | `ResonanceVoice` (plná implementace) |

---

## Soubory — přehled funkcí

### `engine/resonance/resonance_engine.h` / `engine/resonance/resonance_engine.cpp`

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) |
|---|---|---|---|---|
| `ResonanceEngine(int max_resonance_voices)` | off-RT (init) | počet hlasů → objekt | `Engine::Engine` (přes `std::make_unique`) | `setMaxVoices` (clamp + uložení) |
| `setStreamEngine(StreamEngine* se)` | off-RT | pointer na stream engine → void | `Engine::Engine` (init) | `v->setStreamEngine` na existující hlasy |
| `reset()` noexcept | audio nebo off-RT (reload) | — → void (hard-stop všech hlasů) | `Engine::reloadBank` | `slot->hardStop()`, vynuluje `excite_state_` |
| `setGainDb(float db)` / `gainLinear()` | off-RT/GUI / audio | dB → `gain_lin_` (10^(db/20)) / read | `Engine::setResonanceGainDb` / `onPlayedNoteOn` | — |
| `setLayerTargetDb(float db)` / `layerTargetDb()` | off-RT/GUI / audio | dB cíl pro výběr vrstvy → `layer_target_db_` / read | `Engine::setResonanceLayerDb` / `onPlayedNoteOn` (`nearestSlotByRms`) | — |
| `setEnabled(bool)` / `enabled()` | off-RT/GUI / audio | bool → `enabled_` / read | `Engine::setResonanceEnabled` / `onPlayedNoteOn` (early-return) | — |
| `setMaxVoices(int n)` noexcept | off-RT nebo GUI | 1..64 (clamp) → atomic | konstruktor; `Engine::setMaxResonanceVoices` | — |
| `maxVoices()` noexcept | libovolné | — → int (atomic read) | testy, diagnostika | — |
| `setExciteDecayTimeMs(float tau_ms, int block_size, float engine_sr)` | off-RT | tau [ms], blok, sr → `decay_per_block_` | `Engine::Engine`, `Engine::setBlockSize`, `Engine::setExciteDecayMs` | `std::exp(-block_ms / tau_ms)` |
| `exciteDecayPerBlock()` | libovolné | — → float | testy, diagnostika | — |
| `onSelfNoteOn(int played_midi, float engine_sr)` | audio | MIDI noty N, sr → void | `Engine::processNoteQueue` (před `voice_pool.noteOn`) | `slot->fadeOut`, vynuluje `excite_state_[N].last_excite` |
| `onPlayedNoteOn(int played_midi, int velocity, const Bank&, const VoicePool&, const PedalState&, float engine_sr)` | audio | zahraná nota + kontext → spawn/excite rezonancí | `Engine::processNoteQueue` (po `voice_pool.noteOn`) | `harmonicProximity`, `pedal.isUndamped`, `pool.hasActiveMainVoice`, `slot->fadingOut`, `slot->addExcitation`, `slot->start`, `enforceVoiceBudget` |
| `processBlock(float* out_l, float* out_r, int n_samples, const PedalState&)` noexcept | audio | výstupní buffery + pedál → bool (some produced) | `Engine::processBlock` | `excite_state_[N].last_excite *= decay_per_block_`, `slot->setTargetGain`, `slot->process` |
| `activeCount()` noexcept | diagnostika | — → int | testy, GUI diag panel | iterace `voices_` |
| `isResonating(int midi)` noexcept | diagnostika | MIDI → bool | testy | `slot->active()` |
| `currentLevelFor(int midi)` noexcept | diagnostika | MIDI → float | testy, GUI | `slot->currentLevel()` |
| `enforceVoiceBudget(float engine_sr)` | audio (privátní) | sr → void (fadeOut oběti) | `onPlayedNoteOn` (po smyčce spawnu) | `liveCount` lambda, `slot->active()`, `slot->fadingOut()`, `slot->currentLevel()`, `slot->fadeOut` |
| `panForNote(int midi, float& pan_l, float& pan_r)` | audio (privátní, static) | MIDI → stereo koeficienty | `onPlayedNoteOn` (před `slot->start`) | `std::cos/sin` se vzorcem z VoicePool |

**Detailní popis klíčových funkcí:**

**`onPlayedNoteOn` — eligibilita + budget gate + spawn/excite**

Funkce iteruje všech 128 MIDI not N a pro každou provádí třívrstvý filtr:

1. *Eligibility filter 5.5.1(1):* `pedal.isUndamped(N)` musí být true (jinak struna tlumena) a `pool.hasActiveMainVoice(N)` musí být false (jinak by rezonance koexistovala s hlavním hlasem na téže struně — porušení invariantu).
2. *Rule B in-progress:* pokud `slot->active() && slot->fadingOut()`, přeskočí — nová excitace by přerušila probíhající fade-out z rule B.
3. *Harmonický práh:* `harmonicProximity(N, M) < kResonanceHarmonicMin` (0.05) → vynechá (sekunda, tritonus — téměř nulová hodnota).

**RAM cache mód (fáze 8):** `ResonanceEngine` drží per-notě `reso_cache_ready_[128]` (atomic; mimo movable `MicLayer`). `onPlayedNoteOn` přečte `reso_cache_ready_[N]` (acquire) a předá `use_cache` do `slot->start(...)`: `true` → hraj z RAM `preload_resonance` (12 s cílové vrstvy); `false` → **stream mód** (ignoruj cache, streamuj z `resonance_start_frame` přes ring). Při změně „Resonance Layer" slideru `Engine::rebuildResonanceCache` zavolá `clearCacheReady()` (nové hlasy → stream) + `requestRecacheFade()` → `processBlock` na začátku fadene aktivní cache-mód hlasy; background vlákno pak přestaví cache a `setCacheReady()`. Realloc `preload_resonance` je bezpečný — faded hlasy ho nečtou, stream-mód ho ignoruje.

Před iterací: `if (!enabled_) return;`. Excitace se počítá jako `excite = vel_norm * harm * gain_lin` (kde `gain_lin = 10^(resonance_gain_db/20)`) bez dampingového multiplikátoru — ten se aplikuje až v `processBlock` per-blok přes `setTargetGain`, aby se změny pedálu plynule promítaly do hlasitosti existující rezonance. Sample pro N se vybere `nearestSlotByRms(ns, layer_target_db_)` (nejbližší peak RMS k cíli), pak `variants[0]`, `mics[0]`.

Pokud slot pro N již existuje a je aktivní (per-nota uniqueness 5.5.1(2)), volá `slot->addExcitation(excite)` a aktualizuje `excite_state_[N].last_excite = max(...)`. Nealokuje druhý hlas.

Při spawnu nového hlasu předchází **budget gate**: spočte počet živých (ne-fadingOut) hlasů `live` a porovná s `max_voices_`. Pokud `live >= cap`:
- Pokud je `init_gain <= qlevel` (nový by nezněl hlasitěji než nejtišší existující) → `continue` — hlas se vůbec nespawnuje.
- Jinak `fadeOut` na nejtišším existujícím hlasu a pokračuje na spawn.

Tím se eliminuje spawn-churn (dřívější bug: spawn všech harmonik → `acquireRing + requestRead` → okamžitý `fadeOut` přes budget → zbytečná disková čtení hladověla streamující hlasy → underrun i při `MAX_RESONANCE=1`).

Po celé smyčce volá `enforceVoiceBudget` pro dorování při živém snížení slideru `MAX_RESONANCE`.

**`enforceVoiceBudget`**

Počítá jen hlasy, které jsou `active() && !fadingOut()` (živý počet). Klíčová podmínka: hlasy ve fade-out se nepočítají — jinak by jedno překročení budgetu spustilo `fadeOut` na všechny hlasy (po `fadeOut` `active()` zůstává `true` dokud `gain` nedoklesne, takže `activeCount` by neklesal → smyčka by fade-ovala dál, dokud by nezbyl jen fading → rezonance by jen problikla). Smyčka opakovaně hledá nejtišší ne-fadingOut hlas a volá `fadeOut`, dokud `liveCount() <= cap`.

**`onSelfNoteOn` — pravidlo B**

Voláno z `Engine::processNoteQueue` *před* `voice_pool.noteOn(N)`. Pokud `voices_[N]->active() && !voices_[N]->fadingOut()`, spustí `slot->fadeOut(engine_sr)` a vynuluje `excite_state_[N].last_excite`. Tím eligibility filter (1) automaticky zablokuje další excitaci struny N, dokud existuje hlavní hlas N (dokud `pool.hasActiveMainVoice(N)` vrací true).

**`processBlock` — per-blok decay + target update**

Krok 1: pro každý aktivní ne-fadingOut hlas: `excite_state_[N].last_excite *= decay_per_block_` a `slot->setTargetGain(last_excite * pedal.dampingFor(N))`. Hlasy ve fade-out (rule B nebo `target → 0`) se přeskakují — ty si drží svůj ostrý step po celou dobu fade-out.

Krok 2: volá `slot->process(out_l, out_r, n_samples)` na všech alokovaných slotech; vrátí true pokud kterýkoli produkoval vzorky.

---

### `engine/resonance/harmonic_proximity.h` / `engine/resonance/harmonic_proximity.cpp`

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) |
|---|---|---|---|---|
| `harmonicProximity(int target_midi, int source_midi)` | audio (first call: off-RT lazy init) | MIDI noty target, source → float [0, 1] | `ResonanceEngine::onPlayedNoteOn` | `couplingMatrix()` (lookup do předpočítané matice) |
| `rawProx(int target, int source)` (anonymní namespace) | off-RT (init) | MIDI noty → float (raw coupling) | `couplingMatrix` lambda při inicializaci | `midiHz`, nested loop přes parciály |
| `couplingMatrix()` (anonymní namespace) | off-RT při prvním volání | — → const 128×128 matice | `harmonicProximity` | `rawProx` pro všechny páry, normalizace |
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

**Lazy init a normalizace:** `couplingMatrix()` je function-local static lambda — inicializuje se právě jednou (thread-safe dle C++11). Vypočte `rawProx` pro všech 128×128 párů (≈4M operací), najde globální maximum a vydělí jím celou matici → oktáva nahoru normalizována na 1.0. Hodnota `rawProx(N, N) = 0` (self — ošetřeno explicitním `if (target == source) return 0.f`).

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
| `start(int midi, const MicLayer*, float initial_gain, float pan_l, float pan_r, float engine_sr)` | audio | parametry hlasu → aktivace | `ResonanceEngine::onPlayedNoteOn` | `stream_->acquireRing`, `stream_->requestRead`, `recomputeGainStep` |
| `addExcitation(float excitation_gain)` | audio | relativní přírůstek gainu → aktualizace target | `ResonanceEngine::onPlayedNoteOn` (existující hlas) | `recomputeGainStep` |
| `setTargetGain(float target)` | audio | absolutní cílový gain → ramp k cíli | `ResonanceEngine::processBlock` | `recomputeGainStep` |
| `fadeOut(float engine_sr)` | audio | sr → ostrý ramp na 0 | `ResonanceEngine::onSelfNoteOn` (rule B), `ResonanceEngine::enforceVoiceBudget`, `ResonanceEngine::onPlayedNoteOn` (budget gate) | nastaví `is_fading_out_=true`, přepočítá `gain_step_` |
| `hardStop()` noexcept | audio/off-RT | — → okamžitá deaktivace | `ResonanceEngine::reset` | `stream_->releaseRing` |
| `process(float* out_l, float* out_r, int n_samples)` noexcept | audio | výstupní buffery → bool (active \|\| produced) | `ResonanceEngine::processBlock` | čte preload_resonance nebo ring, aplikuje gain rampu, refill heuristika |
| `recomputeGainStep()` noexcept | audio (privátní) | — → přepočet `gain_step_` | `start`, `addExcitation`, `setTargetGain` | `(target_gain_ - gain_) / ramp_frames_` |
| `active()` | libovolné | — → bool | `ResonanceEngine` (smyčky) | přímý read `active_` |
| `fadingOut()` noexcept | audio | — → bool | `ResonanceEngine::onPlayedNoteOn`, `enforceVoiceBudget`, `processBlock` | přímý read `is_fading_out_` |
| `currentLevel()` noexcept | audio | — → float (`gain_`) | `ResonanceEngine` (budget gate, enforceVoiceBudget) | přímý read `gain_` |

**Detailní popis klíčových funkcí:**

**`start` — skip-attack, streaming init**

Nastavuje `position_ = mic_->resonance_start_frame` (FILE-GLOBAL offset) — přeskakuje attack a čte od začátku rezonantní části vzorku. `pos_inc_ = sample_sr / engine_sr` (SR konverze, pitch_ratio = 1.0 vždy — rezonance není transponována). Gain ramp: `gain_ = 0`, `target_gain_ = initial_gain`, `ramp_frames_ = kResonanceRampMs * 0.001 * engine_sr` → fade-in ~30 ms.

Pro Streamed mic: `acquireRing()` a `requestRead(ring, path, res_end, want, eof_done)` kde `res_end = resonance_start_frame + resonance_frames` — požaduje data za preload_resonance regionem. Pokud `acquireRing` selže (pool plný), hlas dohraje jen preload_resonance a tiše skončí (rezonance je luxus, nekritická jako hlavní hlas).

`active_` se nastaví na true jen pokud `mic_` je platný a obsahuje data: pro FullyLoaded ověří `head_frames > resonance_start_frame`, pro Streamed `resonance_frames > 0 || head_frames > resonance_start_frame`.

**`process` — gain ramp + streaming preload→ring + underrun**

Sample-by-sample smyčka pro `n_samples` vzorků. Dvě větve dle `mic_->mode`:

*Streamed:* Nejprve region `preload_resonance` (lokální index `p0 - res_start`, lineární interpolace). Na hranici `p0 >= res_end - 1` přepne na ring. Seed okna (lo/hi sliding window): `ring_lo_idx_ = res_end - 1`, `ring_lo_l/r` = poslední frame preload_resonance → plynulý šev. `ring_hi` se plní z `ring_->popFrame`. Každý vzorek: pokud `ring_lo_idx_ < target (= int(position_))`, posouvá okno pop-frame smyčkou. Pokud `popFrame` selže a není EOF → **underrun**: nastaví `underrun_fading_ = true`, volá `stream_->noteUnderrun()`, sL/sR = 0. Po dobu underrunu se přes `underrun_gain_` (krok `underrun_step_` = `-1/(kUnderrunFadeMs * 0.001 * sr)`) multiplicativně ztlumí výstup → `active_ = false` při `underrun_gain_ <= 0`. EOF bez dat: `active_ = false` po doběhnutí.

*FullyLoaded:* Čte přímo z `preload_head` od `position_` (= res_start), lineární interpolace, `active_ = false` při přesahu `head_frames`.

Gain ramp (společná pro obě větve): `gain_ += gain_step_`, clamp na `target_gain_` (v obou směrech), clamp na 0 (bezpečnost při fadeOut overshoot). Finální výstup: `out_l[i] += sL * env * pan_l_` kde `env = gain_ * underrun_gain_` (pokud underrun).

Deaktivace při fade-out: `is_fading_out_ && gain_ <= kResonanceGainEpsilon (1e-5)` → `active_ = false`, break.

Refill heuristika (Streamed, po smyčce): pokud `avail < refillThresholdFrames()` a není čekající request, volá `stream_->requestRead` pro další blok dat.

Při deaktivaci uvolní ring: `stream_->releaseRing(ring_)`.

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
- **Polyfonie — `hasActiveMainVoice`:** `VoicePool::hasActiveMainVoice(int midi)` (viz `engine/voice/voice_pool.h`) je druhá podmínka eligibility filtru 5.5.1(1). Zabraňuje koexistenci hlavního a rezonantního hlasu na téže struně.
- **Buffers — resonance stream pool:** `Engine` drží oddělený `stream_resonance_` (`std::unique_ptr<StreamEngine>`) nezávislý na `stream_main_` pro hlavní hlasy (viz `engine/engine.h`). `ResonanceVoice` používá `acquireRing`/`releaseRing`/`requestRead` z tohoto poolu. Underrun rezonance neovlivní hlavní pool.
- **Loader — Bank:** `onPlayedNoteOn` přistupuje k `bank.notes[N].slots[0].variants[0].mics[0]` pro získání `MicLayer` s `resonance_start_frame`, `resonance_frames`, `preload_resonance`, `preload_head`. Pokud záznam pro N chybí (`!ns.recorded || ns.slots.empty()`), rezonance pro N nevznikne. Typy definovány v `engine/sample/sample_types.h`.

---

## Nálezy revize

**5.5.1 eligibility — logická správnost:** Filtr je konzistentní. Tři podmínky (damping, `hasActiveMainVoice`, `fadingOut` z rule B) se navzájem doplňují bez mezer. Rule B správně nuluje `last_excite_[N]` — eligibility filter (1) pak skutečně blokuje, protože `processBlock` přestane volat `setTargetGain` na deaktivovaném slotu a při dalším note-on M platí `pool.hasActiveMainVoice(N) = true`.

**Budget gate — spawn-churn:** Logika je správná. Gate porovnává `init_gain` (= `excite * pedal.dampingFor(N)`, tedy s dampingem) s `qlevel` (= `slot->currentLevel()` = aktuální `gain_` existujícího hlasu). Srovnání je tedy na stejné škále. Hraniční případ: pokud všechny živé hlasy jsou ve fade-out (qlevel = 1e30, live = 0 po filtru), `live >= cap` je false — spawn pokračuje bez krádeže. Správné chování.

**`enforceVoiceBudget` — počítání živých hlasů:** Oddělení `active() && !fadingOut()` od celkového `active()` je kriticky důležité (popsáno v implementačním komentáři) a implementace je správná. Lambda `liveCount` se volá znovu v každé iteraci while — O(128 * překročení budgetu), v praxi překročení bývají malá.

**Partial-coincidence model — věrnost:** Model zachycuje fyzikálně relevantní jevy: asymetrie up/down díky `b > a`, šířka pásma 12 ct odpovídá měřeným hodnotám inharmonicity pianových strun. Normalizace na octave-up ≈ 1.0 je konzistentní s požadavkem spec. Potenciální nepřesnost: model ignoruje inharmonicitu (`B = 0`), takže pro nízké noty (kde fyzická inharmonicita je výrazná) může přeceňovat koincidenci vzdálených parciálů. `.h` tuto omezení explicitně dokumentuje jako FUTURE.

**Gain ramp — správnost:** `recomputeGainStep` se volá konzistentně při každé změně `target_gain_` (`start`, `addExcitation`, `setTargetGain`). `fadeOut` přepisuje `gain_step_` vlastním výpočtem a korektem guardí `gain_step_ > 0 → 0`. Clamp v `process` zabraňuje overshoot v obou směrech. Drobnost: `recomputeGainStep` počítá `(target_gain_ - gain_) / ramp_frames_` — pokud se `target_gain_` mění každý blok (per-blok `setTargetGain`), ramp může být zdánlivě kratší než `kResonanceRampMs`, protože každý blok přepočítá step od aktuálního `gain_`. To je záměrné a žádoucí (plynulé sledování pedálu).

**Underrun reakce:** `underrun_fading_` se nastaví jednou a pak multiplicuje gain lineárně k nule (délka ~5 ms = `kUnderrunFadeMs`). Po té `active_ = false`. Hlasy s underrunem nesignalizují streamu pro retry — správně, ring je v neznámém stavu. Bez nálezů.
