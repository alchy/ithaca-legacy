# Zpracování eventů

MIDI události vstupují do systému přes `MidiInput` (RtMidi callback na vlastním vlákně), jsou okamžitě převedeny na strukturu `MidiEvent` a zapsány do lock-free **MPSC** fronty `MidiQueue` (Vyukov bounded queue — producentů je legálně víc: RtMidi callback, GUI `allNotesOff`, CLI/main). Audio thread ji vyprazdňuje na začátku každého volání `Engine::processBlock` (viz [A — Jádro enginu](A-core.md)), kde přeloží každou událost na volání `VoicePool`, `PedalState` a `ResonanceEngine`. Stav pedálu udržuje `PedalState`: CC64 (0–127) se ukládá jako spojitá hodnota a z ní se per-strunu přepočítává `damping_[128]` — koeficient tlumení pro half-pedal model. Plně sešlápnutý pedál nastaví damping každé nedržené struny na 1,0 (žádné tlumení), uvolněný pedál na 0,0 (plné tlumení); přechodné hodnoty odpovídají polovičnímu sešlápnutí. Dolní **lost-motion dead-zona** (`cc64 <= kDamperBiteCC`) drží damping na 0 — pokryje oba typy pedálu (kontinuální i on/off) a zajistí spolehlivé ztlumení rezonance po uvolnění i když kontinuální pedál nedojede přesně na 0. Tok eventů je proto: **MIDI vlákno → MidiQueue → audio vlákno → PedalState + VoicePool + ResonanceEngine**. Note-on/off nesou MIDI kanál a procházejí přes `NoteHoldTracker` (cross-channel hold): tlumítko/voice se uvolní teprve když pustí poslední kanál držící danou výšku — viz `note_hold.h` níže.

---

## Implementováno v souborech

| Soubor | Odpovědnost | Klíčové typy |
|---|---|---|
| `engine/midi/midi_input.h` | Deklarace `MidiInput` — RtMidi wrapper, channel filter | `MidiInput` |
| `engine/midi/midi_input.cpp` | Implementace otevření portů + RtMidi callbacku | `MidiInput` |
| `engine/midi/midi_queue.h` | Lock-free MPSC fronta MIDI událostí (Vyukov bounded) | `MidiQueue`, `MidiEvent` |
| `engine/midi/note_hold.h` | Per-pitch maska kanálů držících notu (cross-channel hold) | `NoteHoldTracker` |
| `engine/pedal/pedal_state.h` | Deklarace `PedalState` — damping mapa, half-pedal API | `PedalState` |
| `engine/pedal/pedal_state.cpp` | Implementace recompute a per-strunu damping výpočtu | `PedalState` |

---

## `engine/midi/midi_input.h` + `engine/midi/midi_input.cpp`

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|---|---|---|---|---|---|---|
| `static std::vector<std::string> listPorts()` | off-RT (GUI / CLI) | — → seznam názvů portů | `app/cli/main.cpp`, `app/gui/app_context.cpp`, `app/gui/panel_topbar.cpp` | `RtMidiIn::getPortName()` — enumerace portů | — | Vytvoří dočasný `RtMidiIn`, projde dostupné porty a vrátí jejich jména. Výjimka z RtMidi je zachycena; v takovém případě se vrátí prázdný nebo částečný seznam. Slouží jen pro zobrazení v UI a CLI (`--midi-list`). |
| `static bool channelAccepts(int channel, uint8_t status)` | libovolné | `channel` (-1 nebo 0–15), `status` byte → bool | `callback()` | — | `channel`: -1 = OMNI, 0–15 = konkrétní MIDI kanál (0-based). `status`: raw MIDI status byte. | Čistá funkce bez vedlejšího efektu. Pokud `channel < 0`, vrátí vždy `true` (OMNI). Jinak porovná `channel` s dolní nibblou `status` (`status & 0x0F`). Testovatelná samostatně bez instance. |
| `void setChannel(int ch)` | off-RT (GUI / CLI) | `ch` → nastaví `channel_` | `app/gui/app_context.cpp` (před open), `app/gui/` (combo CH za běhu), `app/cli/main.cpp` | `channel_.store(relaxed)` | `ch`: -1 (OMNI) nebo 0–15; hodnoty mimo rozsah se normalizují na -1. | `channel_` je `std::atomic<int>` — GUI smí kanál měnit i za otevřeného portu, RtMidi callback ho čte jedním atomic loadem per event (plain int byl formální data race). `app_context.cpp` navíc kanál nastavuje **před** `open()`, aby callback hned od otevření filtroval podle správného kanálu. |
| `bool open(Engine& engine, int port_index)` | off-RT | `port_index` → otevřený port nebo `false` při chybě | `app/cli/main.cpp`, `app/gui/app_context.cpp` | `close()`, `RtMidiIn::openPort()`, `RtMidiIn::ignoreTypes()`, `RtMidiIn::setCallback()` | `port_index`: index portu (< 0 nebo mimo rozsah → 0). | Nejprve zavolá `close()` (idempotentní). Pokud nejsou k dispozici žádné porty, zaloguje varování a vrátí `false`. Nastaví `ignoreTypes(false, true, true)` — přijímá SysEx, ignoruje MIDI timing a active sensing. Zaregistruje `MidiInput::callback` s `this` jako `user_data`. |
| `bool openVirtual(Engine& engine, const std::string& name)` | off-RT | `name` → virtuální port nebo `false` | `app/cli/main.cpp` | `close()`, `RtMidiIn::openVirtualPort()`, `RtMidiIn::ignoreTypes()`, `RtMidiIn::setCallback()` | `name`: název virtuálního portu (výchozí `"ithaca-cli"`). | Pouze macOS / Linux — umožňuje DAW posílat MIDI dovnitř bez fyzického zařízení. Jinak totožné nastavení jako `open()`. |
| `void close()` | off-RT | — → uvolní RtMidiIn | destruktor, `open()`, `openVirtual()` | `RtMidiIn::closePort()`, `delete midi_` | — | Idempotentní. Pokud je port otevřen, zavře ho; poté uvolní `midi_` a vynuluje `engine_` a `port_name_`. Destruktor třídy volá `close()` automaticky. |
| `static void callback(double ts, std::vector<unsigned char>* msg, void* user_data)` | MIDI vlákno (RtMidi) | surová MIDI zpráva → volání `Engine` API | RtMidi runtime | `channelAccepts()`, `engine_->noteOn()`, `engine_->noteOff()`, `engine_->sustainPedal()`, `engine_->allNotesOff()` | `ts`: časové razítko (nevyužito). `msg`: vektor bytů. `user_data`: ukazatel na `MidiInput`. | **Jádro MIDI vstupu.** Bežní na RtMidi vláknu, nikoli na audio threadu. Ochranné podmínky: `msg` nesmí být null, musí mít alespoň 2 bajty, `self` a `self->engine_` nesmí být null. **Channel filter:** jeden atomic load `channel_` per event, pak `channelAccepts(chan, status)` — pokud neodpovídá, zpráva se tiše zahodí (note-on/off se před filtrem ještě DIAG-loguje na úrovni Debug, vč. `[FILTERED-OUT]` značky). Dekódování typu: horní nibble `status & 0xF0` určuje typ zprávy. Kanál `ch = status & 0x0F` se předává do Engine API pro cross-channel hold. `0x90` NoteOn: vel > 0 → `engine_->noteOn(data1, data2, ch)`; vel == 0 → `engine_->noteOff(data1, ch)` (MIDI konvence). `0x80` NoteOff → `engine_->noteOff(data1, ch)`. `0xB0` CC: CC64 (Sustain) → `engine_->sustainPedal(data2)` se spojitou hodnotou; CC120 nebo CC123 → `engine_->allNotesOff()`. Ostatní CC a ostatní typy zpráv jsou ignorovány. `Engine` API okamžitě vloží event do `MidiQueue` (`push` je thread-safe), callback tedy nikdy neblokuje. |

---

## `engine/midi/midi_queue.h`

Soubor je pouze hlavičkový — veškerá logika je inlinovaná.

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|---|---|---|---|---|---|---|
| `bool push(const MidiEvent& e)` | MIDI / GUI / CLI vlákno (**multi-producer safe**) | `e` → `true` (vloženo) / `false` (fronta plná, drop) | `Engine::noteOn()`, `Engine::noteOff()`, `Engine::allNotesOff()`, `Engine::sustainPedal()` | — | `e`: událost k vložení. | **Lock-free MPSC producent (Vyukov bounded queue).** Slot se claimuje **CAS na `w_`**: producent načte `w_`, přečte per-slot `seq` (acquire) a porovná s pozicí — `seq == pos` znamená volný slot, `compare_exchange_weak(pos, pos+1)` ho atomicky zabere (souběžní producenti se nikdy neperou o tentýž slot; dřívější SPSC push = ztracený event + torn write při kolizi). `seq < pos` = fronta plná → `false`, drop bez blokování. Po claimu zapíše data a **publikuje per-slot** `seq.store(pos+1, release)` — konzument vidí kompletní event. Bez zámků a alokací. |
| `bool pop(MidiEvent& out)` | Audio vlákno (konzument — JEDINÝ) | — → `true` + vyplněné `out` / `false` (prázdno) | `Engine::processBlock()` (drain smyčka) | — | `out`: výstupní reference na event. | **Lock-free konzument.** Načte `r_` relaxed (konzument ho jako jediný modifikuje), přečte `seq` slotu (acquire). Pokud `seq < r+1`, fronta je prázdná **nebo slot ještě není publikován** (probíhající push → event dorazí příštím pop) → `false`. Jinak zkopíruje event, uvolní slot `seq.store(r + MIDI_Q_SIZE, release)` a posune `r_`. Nikdy neblokuje; audio thread je vždy RT-safe. |

### Poznámka k memory ordering

Vyukov queue synchronizuje přes **per-slot sequence číslo** (`Cell::seq`), ne přes globální pár indexů:
- Producent: `seq.load(acquire)` ověří volnost slotu; CAS na `w_` (relaxed) slot exkluzivně claimne; `seq.store(pos+1, release)` publikuje zapsaná data konzumentovi.
- Konzument: `seq.load(acquire)` páruje s producentovým release — vidí-li publikované seq, vidí i kompletní data eventu; `seq.store(pos + MIDI_Q_SIZE, release)` vrací slot do oběhu pro další kolo.
- `r_` vlastní výhradně konzument (relaxed); `w_` sdílejí producenti přes CAS.

### `MidiEvent`

```cpp
struct MidiEvent {
    enum Type : uint8_t { NoteOn, NoteOff, Sustain, AllNotesOff };
    Type    type    = NoteOn;
    uint8_t data1   = 0;   // MIDI nota (NoteOn/Off) nebo CC hodnota (Sustain)
    uint8_t data2   = 0;   // velocity (NoteOn)
    uint8_t channel = 0;   // MIDI kanál 0..15 (NoteOn/Off; cross-channel hold)
};
```

Pole `channel` nese 0-based MIDI kanál události a slouží **cross-channel hold** logice (`NoteHoldTracker`, viz níže): při OMNI příjmu (více kanálů, např. Synthesia levá ruka = ch0, pravá = ch1) musí engine vědět, který kanál notu drží, aby note-off jednoho kanálu nezhasl stejnou výšku drženou druhým.

Kapacita fronty: `MIDI_Q_SIZE = 1024` eventů. Překročení → drop; `Engine::noteOn/noteOff/allNotesOff/sustainPedal` zahození logují jako Warning (viz Nálezy revize).

---

## `engine/midi/note_hold.h`

`NoteHoldTracker` řeší **cross-channel hold**. Skutečný klavír má na každou výšku jedno tlumítko — spadne až když je puštěna *poslední* klávesa té výšky. Vícekanálové MIDI (Synthesia: levá ruka ch0, pravá ch1) může stejnou výšku držet více kanály současně nebo si ji předávat; channel-blind engine nechával note-off jednoho kanálu zhasnout notu druhého → **výpadek noty** (typicky C uprostřed klaviatury). Tracker drží pro každou ze 128 not 16-bitovou masku kanálů, které ji právě drží. Stav je vlastněn a měněn **výhradně audio threadem** při drainu MIDI fronty → bez atomik/zámků.

| Metoda | Vstup → výstup | Volá ji | Vysvětlení |
|---|---|---|---|
| `bool noteOn(int note, int ch)` | nota + kanál → `true` pokud **první** držitel | `Engine::processBlock()` drain, case `NoteOn` | Nastaví bit `ch` v masce `note`. `true` = předtím notu nedržel žádný kanál (key-down transition → `PedalState::noteOn`). Voice se ale (re)striká vždy (re-artikulace). Neplatná nota/kanál → `false`, no-op. |
| `bool noteOff(int note, int ch)` | nota + kanál → `true` pokud **poslední** držitel | drain, case `NoteOff` a `NoteOn` s vel=0 | Smaže bit `ch`. `true` = už ji nedrží žádný jiný kanál (→ teprve teď `PedalState::noteOff` + `VoicePool::noteOffWithPedal`). Off od kanálu který notu nedržel → `false`, no-op (žádný falešný release). |
| `bool held(int note) const` | nota → bool | — | Drží notu aspoň jeden kanál? |
| `void allNotesOff()` | — | drain, case `AllNotesOff` | Všechny masky na 0 (panika / reload). |

**Proč bitmaska (a ne čítač/refcount):** `held_[note] |= (1u<<ch)` / `&= ~(1u<<ch)`. 16 bitů přesně pokryje 16 MIDI kanálů; je **idempotentní v rámci kanálu** (opakovaný on z téhož kanálu jen znovu nastaví bit — refcount by přičetl a rozhodil párování); **off od kanálu, který notu nedržel, je čistý no-op** (`bit & maska == 0`) — refcount by podtekl do záporu. `first` = maska byla 0 (→ `pedal_.noteOn`), `last` = maska je 0 (→ release). Velikost 128×2 B = 256 B, vše na audio threadu bez zámků/atomik.

**Hraniční případ:** maska se mění **per kanál**, takže když note-off dorazí na *jiném* kanálu, než byl note-on (nesoulad), nebo se off ztratí, bit zůstane nastaven → `last` nikdy nenastane → `pedal_.noteOff`/`VoicePool::noteOffWithPedal` se nezavolá → **nota může uvíznout**. Za normálního MIDI (off matchuje kanál on) nenastává; **CC123 All-Notes-Off** (panika) masky vyčistí. Vědomý trade-off oproti staré channel-blind verzi, která uvolňovala na každý off (a měla proto cross-hand cancellation bug).

Verifikace: `tests/test_note_hold.cpp` (cross-channel hand-off, idempotence opakovaného on téhož kanálu, no-op off, hranice).

---

## `engine/pedal/pedal_state.h` + `engine/pedal/pedal_state.cpp`

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|---|---|---|---|---|---|---|
| `void setSustainCC(uint8_t cc)` | audio | `cc` (0–127) → aktualizuje `cc64_` + `damping_[]` | `Engine::processBlock()` (drain, case `Sustain`) | `recompute()` | `cc`: hodnota CC64 z MIDI. | Uloží `cc` do `cc64_` a ihned zavolá `recompute()`, která přepočítá celý `damping_[128]`. Jde o O(128) přepočet — viz `recompute()`. |
| `void noteOn(int midi)` | audio | `midi` → nastaví bit v `held_` + `damping_[midi]` na 1,0 | `Engine::processBlock()` (drain, case `NoteOn`) | `recompute()` | `midi`: 0–127; mimo rozsah → ignoruje. | Nastaví `held_[midi]` a zavolá `recompute()`. Po recompute bude `damping_[midi] == 1.0f` bez ohledu na CC64. **Volá se jen na PRVNÍ držitel výšky** (`NoteHoldTracker::noteOn` vrátil `true`) — opakovaný on z dalšího kanálu už `held_[midi]` nemění. |
| `void noteOff(int midi)` | audio | `midi` → resetuje bit v `held_` + přepočítá `damping_[midi]` | `Engine::processBlock()` (drain, case `NoteOff` a `NoteOn` s vel=0) | `recompute()` | `midi`: 0–127; mimo rozsah → ignoruje. | Resetuje `held_[midi]` a zavolá `recompute()`. Po recompute bude `damping_[midi] == cc64_ / 127.f`. **Volá se jen na POSLEDNÍ release výšky** (`NoteHoldTracker::noteOff` vrátil `true`), **před** `VoicePool::noteOffWithPedal()`, aby VoicePool viděl aktuální stav pedálu. |
| `void allNotesOff()` | audio | — → reset celého `held_` + recompute | `Engine::processBlock()` (drain, case `AllNotesOff`) | `recompute()` | — | `held_.reset()` smaže všechny bity najednou (`std::bitset`), pak `recompute()` nastaví `damping_[n] = cc64_ / 127.f` pro všechna `n`. |
| `void recompute()` | audio | — → přepočítá `damping_[0..127]` | `setSustainCC()`, `noteOn()`, `noteOff()`, `allNotesOff()`, konstruktor | — | — | **Jádro damping modelu.** `lift = (cc64_ <= kDamperBiteCC) ? 0 : cc64_/127`. Pro každé `n`: `held_[n] → 1.f`, jinak `lift`. Dolní **lost-motion dead-zona** (`cc64_ <= kDamperBiteCC`, default 8) → `lift = 0`: dokud pedál nepřekročí práh, dusítka leží na strunách. To zaručuje, že po uvolnění pedálu se nedržené struny spolehlivě ztlumí i u **kontinuálního pedálu, který nedojede přesně na 0** (a u on/off pedálu, který posílá 0/127). Nad prahem klasické spojité `cc64/127` (half-pedal feel zachován: CC=64 → ~0,504). Dražená klávesa vždy 1,0. Složitost O(128). |
| `float dampingFor(int midi) const` | audio (resonance, voice pool) | `midi` → damping koeficient [0,0 – 1,0] | `ResonanceEngine::processBlock()`, `ResonanceEngine::onPlayedNoteOn()`, `ResonanceEngine::onSelfNoteOn()`, `VoicePool::noteOn()` | — | `midi`: 0–127; mimo rozsah → vrátí `0.f` (plně dampováno). | Prostý přístup do pole `damping_[]` po hraničním testu. Hodnota je platná okamžitě po posledním `recompute()`. ResonanceEngine ji používá jako multiplikátor excitačního gainu: `excite = (vel/127) × harm × strength × dampingFor(N)`. |
| `bool isUndamped(int midi) const` | audio | `midi` → bool | `Engine::processBlock()` (drain, case `NoteOff`), `VoicePool::noteOffWithPedal()`, `VoicePool::releasePendingNotes()`, `ResonanceEngine` (eligibility check) | `dampingFor()` | `midi`: 0–127. | Inline wrapper: `dampingFor(midi) > kDampingEpsilon` (epsilon = 0,001). Hodnota `false` znamená, že struna je prakticky ztlumena a neměla by rezonovat ani být udržována voice poolem v pending-release stavu. |
| `bool isHeld(int midi) const` | audio / GUI | `midi` → bool | interně (engine, GUI diagnostika) | — | `midi`: 0–127; mimo rozsah → `false`. | Přímý přístup do `held_[midi]`. Nezahrnuje vliv pedálu — pouze zda klávesa fyzicky stisknuta. |
| `bool isPedalDown() const` | audio | — → bool | `Engine::processBlock()` (Sustain case — detekce přechodu DOWN→UP) | — | — | `cc64_ >= kPedalDownThreshold` (64). Prahová hodnota odpovídá MIDI konvenci. Slouží jen pro detekci hrany (byl pedál dole, nyní je nahoře?) → spustit `VoicePool::releasePendingNotes()`. Damping samotný je vždy spojitý. |
| `uint8_t sustainCC() const` | audio / GUI | — → `cc64_` | `Engine::pedalCC()`, `Engine::scaledReleaseMs()`, GUI diagnostika | — | — | Přímé čtení `cc64_`. Používáno pro logging, GUI zobrazení a škálování release time dle spec 5.4. |

---

## Křížové odkazy

- **A — Jádro enginu (`engine/engine.cpp`, `processBlock`):** Drain smyčky `while (midi_q_.pop(e))` je definován v sekci A. Tato sekce B dokumentuje pouze producenty (push do `MidiQueue`) a stav `PedalState`; konzumaci (switch/case s volbami `VoicePool`, `PedalState`, `ResonanceEngine`) viz A-core.md.
- **Resonance — damping v `resonance_engine.cpp`:** `ResonanceEngine::processBlock()` i `onPlayedNoteOn()` čtou `pedal.dampingFor(N)` a `pedal.isUndamped(N)` jako multiplikátor excitačního gainu a eligibility filtr rezonančních hlasů. Kaskáda: CC64 event → `PedalState::setSustainCC()` → `recompute()` → `damping_[]` → `ResonanceEngine` čte per blok.
- **Polyfonie — noteOff s pedálem (`voice_pool.cpp`):** `VoicePool::noteOffWithPedal()` volá `pedal.isUndamped(midi)` — pokud pedál drží strunu (damping > epsilon), hlas přejde do stavu `pending_release` místo normálního release rampu. `releasePendingNotes()` pak uvolní všechny pending hlasy při přechodu pedálu DOWN→UP.

---

## Nálezy revize

### RT-safety fronty — MPSC ✅ VYŘEŠENO (fix/revize-2026-06-10)

Dřívější nález: fronta byla implementována jako SPSC, ale reálně do ní tlačilo více producentů (RtMidi callback + GUI AllNotesOff + CLI) → race na `w_` (ztracený event / torn write). `MidiQueue` je nyní **Vyukov bounded MPSC**: producenti claimují sloty CAS na `w_` a publikují per-slot `seq` (release) — souběžní producenti jsou legální. Konzument zůstává jediný (audio thread). Audio thread nikdy neblokuje.

### Drop-on-full ✅ VYŘEŠENO (fix/revize-2026-06-10)

Při plné frontě (1024 eventů) `push()` vrátí `false` a event je zahozen — to je správné RT chování (žádné blokování). Dřívější nález: `Engine` návratovou hodnotu ignoroval (tichý drop). Nyní `Engine::noteOn/noteOff/allNotesOff/sustainPedal` při `push() == false` **logují Warning** („MIDI fronta plná — … ZAHOZEN") z non-RT producenta. Za normálního provozu (audio block každých ~5 ms, fronta drénovaná každý blok) je 1024 eventů dostatečná rezerva; přeplnění hrozí hlavně během `bank_loading_` pozastavení drainu při agresivním MIDI vstupu — nyní s indikací v logu.

### Half-pedal model

`recompute()` se volá O(1)×-krát po každé změně `cc64_` nebo `held_` — tedy při každém note-on, note-off a CC64 eventu zvlášť. Protože `recompute()` vždy prochází všech 128 strun, každá MIDI událost ve frontě způsobí 128 zápisů do `damping_[]`. Při hustém MIDI vstupu (akord + pedál) to jsou nízko-latentní, cache-friendly operace nad malým polem — v praxi zanedbatelné, ale je to O(n_events × 128) zápisů na drain. Bez nálezů z hlediska korektnosti.

### Channel filter race ✅ VYŘEŠENO (fix/revize-2026-06-10)

`channel_` je nyní `std::atomic<int>` (store/load relaxed) — GUI smí kanál měnit i za otevřeného portu bez data race. `app/gui/app_context.cpp` navíc kanál nastavuje **před** `open()`, takže callback od první události filtruje podle správného kanálu.
