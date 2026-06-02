# Polyfonie

Ithaca udrzuje pool az 128 (max 256) nezavislych hlasu (`Voice`), z nichz kazdy prehrava jeden `SampleAsset` pres prvni mikrofon (`mics[0]`). `VoicePool` rozhoduje o alokaci, retrigger-dampovani a kradenI hlasu; `patch_manager` vybira konkretni `SampleAsset` a velocity-gain pro danou (nota, velocity) kombinaci. Hlas sam riadi tri navrstene envelopy (onset anti-click, release, underrun-fade), damping crossfade pri retriggeru/kradenI, linearni interpolaci pri prehravani z preload-head i streamovaneho ringu a komunikaci se `StreamEngine` (refill requesty).

---

## Implementovano v souborech

| soubor | odpovednost | klicove typy |
|--------|-------------|--------------|
| `engine/voice/voice.h` / `voice.cpp` | Jeden prehravany hlas: sample read (head/ring), interpolace, onset/release/underrun envelope, damping crossfade, streaming refill | `Voice`, konstanty `kOnsetMs`, `kDampingMs`, `kDampMaxFrames`, `kUnderrunFadeMs` |
| `engine/voice/voice_pool.h` / `voice_pool.cpp` | Pool hlasu: alokace slotu, retrigger, steal, noteOn/Off s pedalem, pedal pending-release, blokovy render | `VoicePool`, `kDefaultPoolSize`, `kMaxPoolSize`; pomocna `panForNote()` |
| `engine/voice/patch_manager.h` / `patch_manager.cpp` | Vyber samplu pro (nota, velocity): velocity-slot mapping, round-robin varianta, vel_gain | `VoiceSpec`, `RoundRobinState`, `selectVoice()`, `slotIndexForVelocity()`, `lcgNext()` |
| `engine/voice/_reserved_resampling.h` | Rezervovany (neaktivni, do buildu nezapojen) kod puvodniho pitch-shift fallbacku pro chybejici noty — presunuto z `patch_manager.cpp` rozhodnutim 2026-05-30. | `ithaca::reserved::nearestRecordedNote()`, `semitonePitchRatio()` |

---

## voice.h / voice.cpp

### Konstanty

| konstanta | hodnota | vyznam |
|-----------|---------|--------|
| `kOnsetMs` | 3 ms | Delka onset rampy (anti-click pri note-on) |
| `kDampingMs` | 21 ms | Delka damping crossfade pri retriggeru/kradenI |
| `kDampMaxFrames` | 2048 frames | Horni strop delky damp bufferu |
| `kUnderrunFadeMs` | 5 ms | Rychly fade pri underrunu nebo konci streamu |

### Funkce

| funkce (signatura) | vlakno | vstup → vystup | vola ji | vola (proc) | parametry | vysvetleni |
|--------------------|--------|----------------|---------|-------------|-----------|------------|
| `void setStreamEngine(StreamEngine* se)` | off-RT | ukazatel na `StreamEngine` → ulozi do `stream_` | `VoicePool::setStreamEngine` | — | `se`: muze byt `nullptr` | Jednoduse priradi stream engine vsem hlasem v poolu. Kdyz `nullptr`, hlas po vyprehrani preload-head utichne bez crash. |
| `void start(const SampleAsset*, double pitch_ratio, float vel_gain, float pan_l, float pan_r, float engine_sr)` | audio | parametry tonu → inicializuje stav hlasu | `VoicePool::noteOn` | `stream_->acquireRing()`, `stream_->requestRead()` | `pitch_ratio`: SR konverze (sample_sr/engine_sr * transpozice); `vel_gain`: kvadraticky gain; `pan_l/pan_r`: jiz spoctene kosinus/sinus pan | Nuluje vsechny envelopy a streaming stav. Pokud sampl ma rezim `Streamed` a `stream_` je k dispozici, alokuje ring slot (`acquireRing`) a posle prvni prefetch request (`requestRead`) pro vsechny framy za `head_frames`. Pokud ring pool plny, hlas hraje pouze preload-head. `active_` se nastavi na `true` jen pokud `mic_->head_frames > 0`. |
| `void prepareDamp(float engine_sr)` | audio | soucasny stav hlasu → `damp_buf_` naplnen, hlas deaktivovan | `VoicePool::noteOn` (retrigger i steal) | `ring_->popFrame()`, `stream_->releaseRing()` | `engine_sr`: pro vypocet delky damp_frames | Klicova funkce click-free retriggeru/kradenI. Snima NADCHAZEJICI (neprehrany) vzorky do `damp_buf_` — nikoliv historii — aby doznivaci buffer navazal presne na posledni prehrany frame bez nespojitosti. Dve vetve: (a) hlas je v head regionu — kopirovani dopredu z `preload_head` s linearnim fade; (b) hlas je ve streamed regionu — popovani framu z ringu s fade. Po naplneni VZDY uvolni ring (`releaseRing`) a nastavi `active_ = false` — klic invariantu, ktery zabranuje ring leaku pri opakovanem retriggeru pod pedalem (drive byl bug: early-return pred cleanup → 2 aktivni hlasy pro jednu midi + ring leak → wycerpani pool 32 ringu). Pokud hlas neni aktivni, presto provede cleanup. |
| `void hardStop() noexcept` | audio / off-RT | — → vsechny staty vynulovany, ring vracen | `VoicePool::reset` (pri `reloadBank`) | `stream_->releaseRing()` | — | Okamzite zastavi hlas BEZ cteni sample pameti (vhodne pri reloadu banky, kdy `mic_->preload_*` mohou byt uvolneny). Uvolni ring, vynuluje vsechny flagy vcetne `damp_buf_`. |
| `void release(float release_ms, float engine_sr)` | audio | — → spusti release rampu | `VoicePool::noteOff`, `noteOffWithPedal`, `releasePendingNotes`, `allNotesOff` | — | `release_ms`: delka fade-out; `engine_sr` | Nastavi `releasing_ = true` a vypocita `rel_step_`. Pokud je hlas v onset fazi, zacina ramp od aktualni `onset_gain_` (ne od 1.0), aby nebyl skok. Guard: kdyz uz releasing nebo neni aktivni, nic nedela. |
| `void markPendingRelease()` / `isPendingRelease()` / `clearPendingRelease()` | audio | — → flag `pending_release_` | `VoicePool::noteOffWithPedal` / `releasePendingNotes` | — | — | Trivialni gettery/settery. `markPendingRelease` znamena: klavesa pustena, pedal ji drzi v sustainu, sample hraje dal prirozene — release ramp se spusti az pri pedal-up pres `releasePendingNotes`. |
| `bool process(float* out_l, float* out_r, int n_samples) noexcept` | audio | buffery → vzorky additivne pridany | `VoicePool::processBlock` | `ring_->popFrame()`, `stream_->requestRead()`, `stream_->releaseRing()`, `stream_->noteUnderrun()` | `out_l/out_r`: vystupni buffery (additivne); `n_samples`: pocet framu | Hlavni vykonna smycka hlasu. Pro kazdy frame: (1) Damping crossfade — pokud `damping_` a `damp_pos_ < damp_len_`, prida fade-out z `damp_buf_`; probihá i po `active_ = false`. (2) Vzorkovani: v **head** regionu (`p0 < head_frames - 1`) linearni interpolace mezi `p0` a `p0+1` z `preload_head`. V **ring** regionu (`ring_` != nullptr) udrzuje lo/hi sliding window (`ring_lo_idx_`): seed pri prvnim vstupu = posledni head frame jako lo, prvni ring pop jako hi (plynuly sev); per-frame posouva okno popovanim ringu; frac interpolace pres okno. Pokud ring prazdny, spusti `underrun_fading_`: rozlisi cisty konec souboru (vsechna data jiz pozadana → `Info`) od skutecneho underrunu (`Warning` + `stream_->noteUnderrun()`). FullyLoaded sampl bez ringu: po prekroceni head_frames deaktivace. (3) Envelopy aplikovany v poradi: `onset_gain_` (linearni ramp 0→1), `rel_gain_` (linearni ramp 1→0), `underrun_gain_` (linearni ramp 1→0 pri underrunu). Vysledny gain = `vel_gain_ * env * pan`. (4) Streaming refill heuristika po smycce: pokud ring klesl pod `refillThresholdFrames()` a soubor jeste nebyl cely pozadan, posle dalsi `requestRead`. `stream_pending_` flag zabranuje spamovani; shazuje se kdyz `avail >= half_cap` (worker dohnal predchozi request). (5) Navratova hodnota: `active_ || damping_ || produced` — hlas zustava v poolu dokud dozniva damping crossfade i po deaktivaci. |
| `float currentLevel() const noexcept` | audio | — → soucasna obalova hlasitost | `VoicePool::findSlot` (pro kradez) | — | — | `vel_gain_ * onset_gain_` (pokud v onset) * `rel_gain_` (pokud releasing). Pouziva se pro porovnani hlasu pri kradenI — nejtissi = kandidat ke kradenI. |

---

## voice_pool.h / voice_pool.cpp

### Pomocna funkce (anonymni namespace)

| funkce (signatura) | vlakno | vstup → vystup | vola ji | vysvetleni |
|--------------------|--------|----------------|---------|------------|
| `void panForNote(int midi, float spread, float& pan_l, float& pan_r)` | audio | midi, spread → pan_l, pan_r | `VoicePool::noteOn` | Konstantni uhel pi/4 (stred) posunuti o vzdalenost od MIDI 64.5 dle `spread`. Kosinus/sinus: zaokrouhleny power-pan (zachovava soucet energie). |

### Clenske funkce VoicePool

| funkce (signatura) | vlakno | vstup → vystup | vola ji | vola (proc) | parametry | vysvetleni |
|--------------------|--------|----------------|---------|-------------|-----------|------------|
| `VoicePool(int pool_size)` | off-RT | pool_size → alokace `voices_` | `Engine::init` | — | `pool_size`: clampovano na `[1, kMaxPoolSize]` | Prealokuje vektor hlasu. Zadny hlubsi init — `Voice` je value-initialized. |
| `void setStreamEngine(StreamEngine* se)` | off-RT | ukazatel → propagace do vsech hlasu | `Engine::init` | `Voice::setStreamEngine` pro kazdy hlas | `se` | Iteruje pres vsechny sloty; `nullptr` OK. |
| `void reset() noexcept` | audio / off-RT | — → vsechny hlasy hard-stop | `Engine::reloadBank` (bank reload safety) | `Voice::hardStop` pro kazdy hlas | — | Pouziva se pri reloadu banky, kdy sample pamet se meni — `hardStop` zajisti, ze hlasy nesahaji na uvolnenou pamet. |
| `int findSlot(const PedalState* pedal)` | audio | pedal stav → index slotu | `VoicePool::noteOn` | `Voice::active()`, `Voice::releasing()`, `Voice::currentLevel()`, `PedalState::isHeld()` | `pedal`: muze byt `nullptr` | Ctyrfazova steal logika (priorita shora): **(1) volny slot** — preferovany, zadny damp; **(2) nejtissi releasing hlas** — mizi sam od sebe (po note-off), bezpecna kradez; **(3) nejtissi ne-held hlas** (pedal-sustained) — klavesa uzitelem jiz pustena, pedal ho jen drzi, uzivatel ho "nepotrebuje"; dostupne jen kdyz `pedal != nullptr`; **(4) nejtissi z celeho poolu** — krajni pripad (vsechny klaves drzeny a pool plny). Faze 3 je klic pri dlouhych pedalovych pasazich: bez ni by kradl drzene hlasy pred opustenymi. |
| `void noteOn(int midi, const VoiceSpec& spec, float engine_sr, float keyboard_spread, const PedalState* pedal)` | audio | (midi, spec, sr, spread, pedal) → novy hlas spusten | `Engine::processBlock` (z MIDI fronty) | `Voice::prepareDamp`, `findSlot`, `panForNote`, `Voice::start`, `Voice::setMidi` | `midi`: 0–127; `spec`: vystup `selectVoice`; `keyboard_spread`: sirka stereo rozprostreni | Nejdriv projede cely pool a dampne KAZDY aktivni hlas stejne midi (retrigger invariant 5.5.1: NIKDY 2 hlavni hlasy pro jednu midi). Pak `findSlot` pro novy slot; pokud slot obsahuje aktivni hlas jineho tonu, dampne ho take. Spusti novy hlas s preloadenym `spec.asset`. Ladici log (non-RT, mutex) zaznamenava steal vs. normalni noteOn. |
| `void noteOff(int midi, float release_ms, float engine_sr)` | audio | midi → release ramp | `Engine::processBlock` | `Voice::release` | — | Iteruje pool, spusti release na vsech aktivnich ne-releasing hlasech dane midi. Pouziva se pri pedalOff nebo kdyz pedal neni aktivni. |
| `void noteOffWithPedal(int midi, const PedalState& pedal, float release_ms, float engine_sr)` | audio | midi + pedal stav → pending nebo release | `Engine::processBlock` | `PedalState::isUndamped`, `Voice::markPendingRelease`, `Voice::release` | `pedal`: fyzikalni model tlumitka | Implementuje fyzikalni model pianova pedalu (spec 5.4): pokud `pedal.isUndamped(midi)` (tlumitko v suspenzi = struna muze zvucet), oznaci hlasy jako `pendingRelease` — zadny fade-out, sample hraje dal prirozene. Kdyz pedal nesustainuje strunu dane noty (tlumitko dosedlo), normalni release ihned. |
| `void releasePendingNotes(const PedalState& pedal, float release_ms, float engine_sr)` | audio | pedal stav → release pending hlasu | `Engine::processBlock` (pri pedal-up eventu) | `Voice::isPendingRelease`, `PedalState::isHeld`, `Voice::clearPendingRelease`, `Voice::release` | — | Pri uvolneni pedalu: pro kazdy hlas s `pending_release_` zkontroluje, zda uzivatel klavesu mezitim znovu stiskl (`isHeld`). Pokud ano, zruSi pending (nota hraje dal). Pokud ne, spusti release ramp. |
| `void allNotesOff(float release_ms, float engine_sr)` | audio | — → release vsechny aktivni hlasy | `Engine::processBlock` (panic / all-notes-off MIDI) | `Voice::release` | — | Jednoducha iterace — release na vsech aktivnich ne-releasing hlasech bez ohledu na midi. |
| `bool processBlock(float* out_l, float* out_r, int n_samples, float engine_sr) noexcept` | audio | buffery → vzorky additivne vyrendrovany | `Engine::processBlock` | `Voice::process` pro kazdy aktivni hlas | `engine_sr` momentalne nevyuzito (predano do `process` budoucne) | Iteruje vsechny sloty (typicky 128–256, trivialne rychle); skipuje neaktivni; scitava vystupy additivne. Vrati `true` kdyz alespon jeden hlas neco vyprodukoval (vcetne doznikajiciho damp crossfade). |
| `int activeCount() const noexcept` | audio / GUI | — → pocet aktivnich hlasu | `Engine::activeVoices()` (GUI, diagnostika) | `Voice::active()` | — | Jednoducha linearni iterace. |
| `bool hasActiveMainVoice(int midi) const noexcept` | audio | midi → bool | `ResonanceEngine::processBlock` (eligibility filter) | `Voice::active()` | — | Vrati `true` kdyz existuje alespon jeden aktivni hlas (v libovolnem stavu: held, releasing, pedal-sustained) pro danou midi notu. Pouziva `ResonanceEngine` jako eligibility filtr 5.5.1 (1): rezonancni hlas pro notu N se nealokuje, dokud zije hlavni hlas N. |
| `const std::vector<Voice>& voicesView() const noexcept` | GUI (read-only) | — → const ref na pool | `Engine::activeMidiNotes()`, `Engine::currentGainFor()` | — | — | Const view pro GUI (vizualizace klavesnice, polyphony indikator). Nikdy nesmeni vraceny vektor. |

---

## patch_manager.h / patch_manager.cpp

### Struktury

| typ | popis |
|-----|-------|
| `VoiceSpec` | Vysledek `selectVoice`: `asset` (nullptr = ticho), `pitch_ratio` (vzdy 1.0 — bez resamplingu), `vel_gain` (kvadraticky z velocity) |
| `RoundRobinState` | Pamet round-robin: `last[128][16]` (posledni zvolena varianta pro kazdou notu×slot), `rng` (LCG seed). Hodnoty -1 = jeste nehrano. |

### Funkce

| funkce (signatura) | vlakno | vstup → vystup | vola ji | vola (proc) | parametry | vysvetleni |
|--------------------|--------|----------------|---------|-------------|-----------|------------|
| `int slotIndexForVelocity(int velocity, int nslots)` (anonymni namespace) | audio | velocity 0–127, pocet slotu → index slotu | `selectVoice` | — | `velocity`: MIDI velocity; `nslots`: pocet nah. dynamik pro notu | Linearni mapovani velocity na slot: `t = velocity/127`, `idx = round(t * (nslots-1))`. Sloty jsou serazeny vzestupne dle RMS (tichy→hlasity), takze vyssi velocity → vyssi slot (hlasitejsi nahravka). Clamp na [0, nslots-1]. |
| `uint32_t lcgNext(uint32_t& state)` (anonymni namespace) | audio | stav RNG → dalsi pseudonahodne cislo | `selectVoice` | — | `state`: in-out LCG stav | LCG krok (konstanty Numerical Recipes: `*1664525 + 1013904223`). Jednoduchy, deterministicky, testovatelny — bez globalniho stavu. |
| `VoiceSpec selectVoice(const Bank& bank, int midi, int velocity, RoundRobinState& rr)` | audio | (bank, midi, velocity, rr) → VoiceSpec | `Engine::processBlock` (pri zpracovani noteOn z fronty) | `slotIndexForVelocity`, `lcgNext` | `bank`: nahrana banka; `midi`: 0–127; `velocity`: MIDI velocity; `rr`: round-robin stav (in-out) | Hlavni vyberni logika. **(1)** Ověri `bank.notes[midi].recorded` — pokud nota neni nahrana, vrati prazdny `VoiceSpec` (ticho, bez pitch-shift fallbacku; drive byl fallback presunut do `_reserved_resampling.h`). **(2)** `slotIndexForVelocity` vybere velocity vrstvu. **(3)** Round-robin: pokud vice variant, vybere nahodnou jinou nez naposledy hranou (retry smycka s LCG; pri 2+ variantach vzdy dokonci v O(1)). Pamatuje si `last[midi][sidx]` — clampovano na sidx < 16. **(4)** `vel_gain = (velocity/127)^2` (kvadraticky = percepcni). `pitch_ratio` vzdy 1.0 (bez transpozice). |

---

## Krizove odkazy

| Oblast | Vazba |
|--------|-------|
| **Events — noteOn/Off zdroj** | `Engine::processBlock` drainuje lock-free MIDI frontu a vola `selectVoice` + `VoicePool::noteOn` / `noteOff` / `noteOffWithPedal` / `releasePendingNotes`. Hlasy tedy vznika vzdy na audio threadu, i kdyz MIDI event pridal jiny thread. |
| **Buffers — streaming ring** | `Voice::start` alokuje `RingHandle` pres `StreamEngine::acquireRing`; `Voice::process` cte z ringu (`popFrame`) a posila refill requesty (`requestRead`); `Voice::prepareDamp` a `hardStop` vracejI ring (`releaseRing`). Ring je sdileny zdroj — pool 32 slotu; wycerpani zpusobuje degradaci na preload-only rezim. |
| **Loader — Bank / sample_types** | `selectVoice` prijima `const Bank&` (struktura `NoteSlots`, `VelocitySlot`, `SampleAsset` definovane v `engine/sample/sample_types.h`). `Voice::start` uklada `const SampleAsset*` a `const MicLayer*` (mics[0]); nasledne cte `mic_->preload_head`, `mic_->head_frames`, `mic_->file.frames`, `mic_->file.sample_rate`, `mic_->file.path`, `mic_->mode`. Pri reloadu banky `VoicePool::reset` (hard-stop vsech hlasu) musi probehnout PRED uvolnenim stare banky pameti. |
| **Resonance — eligibility `hasActiveMainVoice`** | `ResonanceEngine::processBlock` vola `VoicePool::hasActiveMainVoice(N)` jako prvni krok eligibility filtru (spec 5.5.1 pravidlo 1): rezonancni hlas pro notu N je ineligibilni, pokud `Voice::active()` pro jakykoliv hlas midi=N vrati `true` (zahrnuje held, releasing i pedal-sustained stavy). |

---

## Nalezy revize

### 5.5.1 retrigger invariant — potencialni race v damping smycce

`VoicePool::noteOn` iteruje pool a dampne vsechny aktivni hlasy se shodnou midi **pred** volanim `findSlot`. `prepareDamp` nastavi `active_ = false`. Problem: pokud by `findSlot` nasledne vybral slot, ktery prave dampnula ta smycka (slot uz je `active_ = false` → zvolen jako "volny"), dostane ho novy hlas — to je spravne. Pokud ale banka (v jinak prazdnem poolu) vrati ten same slot nejakou jinou cestou, `damp_buf_` z `prepareDamp` se prepise v `start`. `start` ale `damp_buf_` **nemazе** a `damp_len_` / `damping_` **neresеtuje** — pouze resetuje streaming stav. V praxi to vadi jen kdyz: (a) retrigger vybere prave dumpnuty slot (mozne pri plnem poolu), a (b) `prepareDamp` naplnil `damp_buf_` (hlas byl aktivni). V tom pripade novy hlas zddedI stary `damp_buf_` a `damp_len_`, coz vede k kratkemu (<=21 ms) ghost crossfade z predchoziho tonu na neocekavanem miste. **Doporuceni** (neupravovat ted): `Voice::start` by mel resetovat `damping_ = false; damp_len_ = 0; damp_pos_ = 0` — nebo `prepareDamp` a `start` musi byt vzdy volany jako par (prepareDamp → start bezi na stejnem slotu), coz je v soucasnem kodu zaruceno logiku `VoicePool::noteOn`, ale neni to vynuceno typovym systemem.

### Underrun rozlisovani — heuristika `file_request_off_`

Cisty konec vs. skutecny underrun se rozlisuje porovnanim `file_request_off_ >= total_frames`. Pokud `StreamEngine::requestRead` selze tisnout (napr. fronta plna), `file_request_off_` se navysuje i kdyz data nedorazila → motor muze nespravne klasifikovat skutecny underrun jako "cisty konec" a nevola `noteUnderrun()`. Pro ladici fazi dostacujici; v produkci by si `file_request_off_` mel navysovat pouze po potvrzenem prevzeti requestu workerem.

### `processBlock` ignoruje `engine_sr` parametr

`VoicePool::processBlock` prijima `engine_sr` ale okamzite provede `(void)engine_sr` a nepredava ho do `Voice::process`. `Voice::process` `engine_sr` fakticky nepotrebuje (vse per-frame je vypocteno pri `start`/`release`), takze to neni bug — ale signaturu lze zjednodusit a zbavit se mrtvého parametru.

### Release ve `start` neobnovuje damp stav

Viz poznamka k invariantu vyse. Zadna dalsi logicka chyba v envelope sekvencovani nenalezena: onset, release a underrun-fade jsou independent rampy spravne nasazovane multiplikativne. `release` spravne bere `onset_gain_` jako startovni hodnotu kdyz `in_onset_`. Pedal pending-release je pri retriggeru spravne cisteny (`pending_release_ = false` ve `start`).
