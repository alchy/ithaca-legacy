# Faze 5 — pedal + sympaticka rezonance — implementacni plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (doporuceno) nebo superpowers:executing-plans. Kroky pouzivaji checkbox (`- [ ]`).
>
> Jazyk: komentare/docs/commit messages cesky bez diakritiky; identifikatory anglicky;
> komentovat spise vice (explicit > implicit).

**Goal:** Pridat dva uzce provazane moduly: (1) `pedal` modul, ktery prijima CC64 (0–127) a
udrzuje "set neztlumenych strun" + obstara note-off-with-sustain logiku; (2) `resonance_engine`,
ktery z toho setu spousti tiche rezonancni hlasy pri kazdem note-on harmonicky pribuznych not.
Hraje se z preload_resonance regionu, ktery jsme jiz pripravili strukturalne ve fazi 4 (zatim
prazdny — fazi 5 ho **doplnime** v loaderu pro Streamed samply).

**Architecture:**
- `pedal_state.h` — citelny snapshot: `cc64`, `pedal_down`, `undamped_strings[128]` (bitmap).
  Pristup z audio threadu cteni jen, modifikace pres MidiQueue evnety + note-on/note-off bookkeeping.
- `resonance_engine.{h,cpp}` — novy modul nad `voice_pool`. Drzi `resonance_voices[128]` (jeden
  slot per nota N) + `resonance_strength_` atomic. API:
  - `onNoteOn(triggering_midi, velocity, bank, pool, pedal)` — pri kazdem note-on hrane noty M
    spocita harmonicky pribuzne struny + aktualizuje rezonancni hlasy.
  - `onNoteOn_self(N)` / `onNoteOff(N)` / `onPedalUp()` — udalosti, ktere ovlivni co je eligible.
  - **Eligibility filter (5.5.1 (1)):** `N ∈ undamped_strings ∧ N ∉ active_main_voices`.
  - **Uniqueness (5.5.1 (2)):** jeden slot per N; multi-source jen aktualizuje amplitudu.
  - **Pravidlo B:** note-on(N) s aktivni rezonanci → fast fade (5 ms) + free slot.
- `voice/resonance_voice.{h,cpp}` — specializovany hlas pro rezonanci. Sdili strukturu se
  Streamed `Voice` (cte z preload_resonance + (faze 5) ringu z `resonance_start_frame`), ale
  ma jiny start (skip-attack, fast ramp up na low gain) a jiny envelope (gain `f(velocity, harm,
  strength)` updatovany pri kazdem novem buzeni, fade out kdyz nota opusti `undamped_strings`).
- `sample_store.cpp` — DOPLN nacteni preload_resonance regionu: pro Streamed mic vezmi
  `resonance_start_frame = attack_end_frame` (jiz mereno ve fazi 4) a nacti vyrez
  `[resonance_start_frame, +resonance_window_ms]` pres readWavRange. Pro FullyLoaded sampl
  preload_resonance zustava prazdny (cely sampl je uz v preload_head).
- `engine.cpp` — drainuje `Sustain` event z MidiQueue → predava do `pedal`. NoteOn/NoteOff
  events teď navic informuji pedal + resonance_engine. Engine pridava `resonance_engine` jako
  member; processBlock vola jeji aktualizace v presnem poradi (viz 5.5.1 hranicni MIDI poradi).

**Tech Stack:** C++20, navazuje na fazi 1–4. Zadne nove vendored deps. Build/verifikace macOS.

---

## Kontext a zdrojove vzory

- Spec sekce 5.4 (pedal), 5.5 (rezonance — riadena zivym stavem), 5.5.1 (invariant: NIKDY dvoji
  hrani teze noty + matrice + pravidlo B + multi-source uniqueness): pravidla, ktera plan
  realizuje. PRI POCHYBNOSTECH si vzdy nactete spec.
- Voice damping crossfade z faze 3 (`engine/voice/voice.cpp`, `prepareDamp`/`damp_buf_`): vzor pro
  fast fade na rezonancnim hlase.
- Voice streaming z faze 4 (`engine/voice/voice.cpp`): vzor pro cteni z preload + ring. Rezonancni
  hlas zacina od `resonance_start_frame` v souboru — analogicke, jen jiny offset.
- StreamEngine + RingHandle (`engine/stream/stream_engine.{h,cpp}`): rezonancni hlas se
  Streamed mic taky pouziva ring (alokuje pri start, requestRead od `resonance_start_frame +
  preload_resonance.size()`).
- MidiQueue evnety (`engine/midi/midi_queue.h`): `Sustain` typ uz existuje, jen ho dosud
  nikdo nekonzumoval — Engine::processBlock ho zacne predavat do pedal modulu.

### Vedome ODLOZENO mimo fazi 5

- **Half-pedal dedikovane samply** — jen continuous release-time scaling jako fallback (legacy
  nikdy nemel half-pedal samply). Dedikovane samply prijdou s extended bankou (faze 7).
- **DSP chain + mic mixer** — faze 6.
- **Extended format multi-mic rezonance** — rezonance ve fazi 5 cita JEN main mic (mics[0]) i
  pro extended banky. Mixovani vsech micpos pro rezonanci je faze 6/7.
- **Pitch detection / harmonic detection z FFT** — pouzijeme jednoduchy harmonicky model
  (oktavy, kvinty, kvarty + nizke vahy pro tercie a sekundy). Plne FFT-based je `(open)`.

### Cilova struktura po fazi 5 (nove + modifikovane soubory)

```
engine/
  pedal/
    pedal_state.h               PedalState struct + UndampedSet bitmap + helpers
    pedal_state.cpp             update z CC64, note-on/off bookkeeping
  resonance/
    resonance_engine.h          ResonanceEngine, ResonanceVoice* table[128]
    resonance_engine.cpp        eligibility, alokace/update/fade, harmonic model
    harmonic_proximity.h        harmonicProximity(N, M) → vaha [0..1]
    harmonic_proximity.cpp
  voice/
    resonance_voice.h           specializovany hlas (skip-attack, low gain, update vstupu)
    resonance_voice.cpp
  sample/
    sample_store.cpp            DOPLN nacteni preload_resonance pro Streamed mic
  engine.h / .cpp               drainuje Sustain, predava do pedal + resonance_engine;
                                processBlock vola resonance_engine.process()
tests/
  test_pedal_state.cpp          undamped set: CC64 prah, note-on/off, allNotesOff
  test_harmonic_proximity.cpp   oktava=1.0, kvinta>0.5, tritonus<0.1, ...
  test_resonance_engine.cpp     eligibility filter, uniqueness, pravidlo B, multi-source
  test_long_sample_resonance.cpp  integ: fixture banka, pedal dolu, note-on M -> N rezonuje
```

### Konvence commitu (jako predchozi faze)

`typ(faze5): popis` cesky bez diakritiky; `git add` konkretni soubory; trailer
`Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`; commit pres
`git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit`.

Pred zacatkem prace: vytvor a prepni se na vetev `faze5-pedal-rezonance` z `main`.

---

## Task 1: `PedalState` — spojity damping vektor (TDD)

**Files:**
- Create: `engine/pedal/pedal_state.h`, `engine/pedal/pedal_state.cpp`
- Test: `tests/test_pedal_state.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Drzi aktualni `cc64` (0–127) a **`damping_[128]` — per-string damping koeficient v [0, 1]**
(NE binarni bitmap, viz spec 5.5). Sustain pedal je SPOJITY parametr, ne on/off prah:
- `damping_[N] = 1.0` → struna zni volne (drzena klavesa nebo plne zveduty pedal)
- `damping_[N] = 0.0` → struna ztlumena
- mezi tim → half-pedal (tlumitko se jen dotyka struny, rezonance prirozene tlumena)

`damping_[N]` slouzi rezonancnimu enginu jako multiplikator excitacniho gainu — pri half-pedal je
rezonance prirozene tisi. Pri zmene CC64 (rec. pomalym pohybem pedalu) vsechny existujici
rezonancni hlasy aktualizuji svuj `target_gain` (resi ResonanceVoice/Engine, ne PedalState).

API je jednovlaknove (vse na audio threadu pri MidiQueue drain).

- [ ] **Step 1: Napis selhavajici test `tests/test_pedal_state.cpp`**

```cpp
// tests/test_pedal_state.cpp
// PedalState je SPOJITY: damping_[N] in [0, 1] dle CC64 a stavu klaves.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "pedal/pedal_state.h"

using namespace ithaca;

TEST_CASE("PedalState: pedal UP (CC=0) — undamped jen drzene klavesy") {
    PedalState p;
    p.noteOn(60); p.noteOn(64); p.noteOn(67);
    p.setSustainCC(0);
    CHECK(p.dampingFor(60) == doctest::Approx(1.0f));   // drzena = vzdy 1.0
    CHECK(p.dampingFor(64) == doctest::Approx(1.0f));
    CHECK(p.dampingFor(67) == doctest::Approx(1.0f));
    CHECK(p.dampingFor(72) == doctest::Approx(0.0f));   // ne drzena + pedal up = 0
    CHECK(p.dampingFor(50) == doctest::Approx(0.0f));
    p.noteOff(64);  // klavesa pustena, pedal up = ztlumi se
    CHECK(p.dampingFor(64) == doctest::Approx(0.0f));
}

TEST_CASE("PedalState: pedal DOWN (CC=127) — vsechny struny undamped (1.0)") {
    PedalState p;
    p.noteOn(60);
    p.setSustainCC(127);
    for (int n = 0; n < 128; ++n)
        CHECK(p.dampingFor(n) == doctest::Approx(1.0f));
    // note-off s pedalem dole nemeni damping (pedal drzi sustain).
    p.noteOff(60);
    CHECK(p.dampingFor(60) == doctest::Approx(1.0f));
}

TEST_CASE("PedalState: half-pedal (CC=64) — ne-drzene maji 0.5, drzene 1.0") {
    PedalState p;
    p.noteOn(60);
    p.setSustainCC(64);
    CHECK(p.dampingFor(60) == doctest::Approx(1.0f));        // drzena = vzdy 1.0
    CHECK(p.dampingFor(72) == doctest::Approx(64.f/127.f).epsilon(0.001));
    // CC=32 = ~25% pedalu
    p.setSustainCC(32);
    CHECK(p.dampingFor(72) == doctest::Approx(32.f/127.f).epsilon(0.001));
}

TEST_CASE("PedalState: spojita zmena CC se promita lineárne") {
    PedalState p;
    // bez drzeni: damping ne-drzene noty = cc/127
    for (int cc : {0, 16, 32, 48, 64, 80, 96, 112, 127}) {
        p.setSustainCC((uint8_t)cc);
        float expected = (float)cc / 127.f;
        CHECK(p.dampingFor(50) == doctest::Approx(expected).epsilon(0.001));
    }
}

TEST_CASE("PedalState: isUndamped pouziva epsilon prah") {
    PedalState p;
    p.setSustainCC(0);
    CHECK_FALSE(p.isUndamped(50));     // damping = 0
    p.setSustainCC(1);                  // ~0.008 — slabe lize
    CHECK(p.isUndamped(50));           // > 0.001 epsilon
}

TEST_CASE("PedalState: helper isPedalDown / sustainCC pro half-pedal release scaling") {
    PedalState p;
    // 0..63 = up, 64..127 = down (klasicky MIDI prah pro on/off).
    p.setSustainCC(63);  CHECK_FALSE(p.isPedalDown());
    p.setSustainCC(64);  CHECK(p.isPedalDown());
    // Continuous hodnota pro release scaling (faze 5: scaledReleaseMs).
    p.setSustainCC(96);  CHECK(p.sustainCC() == 96);
}

TEST_CASE("PedalState: allNotesOff — drzene klavesy se uvolni") {
    PedalState p;
    p.noteOn(60); p.noteOn(64);
    p.setSustainCC(0);
    p.allNotesOff();
    CHECK(p.dampingFor(60) == doctest::Approx(0.0f));
    CHECK(p.dampingFor(64) == doctest::Approx(0.0f));
}
```

- [ ] **Step 2: Vytvor `engine/pedal/pedal_state.h`**

```cpp
#pragma once
// engine/pedal/pedal_state.h
// --------------------------
// PedalState drzi aktualni stav sustain pedalu (CC64, 0-127) a **spojite**
// damping_[128] (per-string damping koeficient v [0, 1]). Sustain pedal NENI
// on/off prah — je to spojity parametr, ktery se promita do hlasitosti
// doznivani i rezonance (half-pedal).
//
// Vzorec per strunu N:
//   damping_[N] = 1.0           pokud N je drzena (held)
//   damping_[N] = cc64_ / 127.0 pokud N neni drzena
//
// Resonance engine pouzije damping_[N] jako multiplikator excitacniho gainu:
//   excite = (vel/127) × harm × strength × damping_[N]
//
// API je jednovlaknove — vsechna volani z audio threadu pri drainu MidiQueue.
// GUI/MIDI thread posila zmeny VYHRADNE pres MidiQueue (jako noteOn/noteOff).

#include <bitset>
#include <cstdint>

namespace ithaca {

// Prah CC64 podle MIDI konvence (>=64 = pedal dolu); slouzi jen pro helper
// `isPedalDown()` a release-time scaling (5.4). Damping je VZDY spojite.
constexpr uint8_t kPedalDownThreshold = 64;

// Epsilon prah pro `isUndamped()` eligibility check — pod nim je rezonance
// tak slaba, ze ji povazujeme za prakticky ztlumenou.
constexpr float kDampingEpsilon = 0.001f;

class PedalState {
public:
    PedalState() { recompute(); }

    // Update z MidiEvent::Sustain (audio thread, drain MidiQueue).
    void setSustainCC(uint8_t cc);
    // Bookkeeping z note-on/off (audio thread, drain MidiQueue).
    void noteOn(int midi);
    void noteOff(int midi);
    void allNotesOff();

    // -- Read API (audio thread, volane resonance_engine + Engine release scaling) --

    // Per-string damping koeficient [0, 1]. 1.0 = struna zni volne, 0.0 = ztlumena.
    float dampingFor(int midi) const;

    // Rychly bool: damping > epsilon (= prakticky undamped).
    bool isUndamped(int midi) const { return dampingFor(midi) > kDampingEpsilon; }

    // Drzena klavesa?
    bool isHeld(int midi) const { return midi >= 0 && midi < 128 && held_[(size_t)midi]; }

    // Helpery pro continuous release-time scaling a UI (5.4 spec).
    bool   isPedalDown() const { return cc64_ >= kPedalDownThreshold; }
    uint8_t sustainCC()  const { return cc64_; }

private:
    // Recompute damping_ podle (cc64_, held_).
    void recompute();

    uint8_t          cc64_ = 0;
    std::bitset<128> held_;        // klavesa drzena (note-on bez note-off)
    float            damping_[128] = {};  // per-string damping [0..1]
};

} // namespace ithaca
```

- [ ] **Step 3: Vytvor `engine/pedal/pedal_state.cpp`**

```cpp
// engine/pedal/pedal_state.cpp — viz pedal_state.h.
#include "pedal/pedal_state.h"

namespace ithaca {

void PedalState::setSustainCC(uint8_t cc) {
    cc64_ = cc;
    recompute();
}

void PedalState::noteOn(int midi) {
    if (midi < 0 || midi > 127) return;
    held_.set((size_t)midi);
    recompute();
}

void PedalState::noteOff(int midi) {
    if (midi < 0 || midi > 127) return;
    held_.reset((size_t)midi);
    recompute();
}

void PedalState::allNotesOff() {
    held_.reset();
    recompute();
}

void PedalState::recompute() {
    // Spojita damping mapa: drzena klavesa vzdy 1.0, ne-drzena = cc64/127.
    const float lift = (float)cc64_ / 127.f;
    for (int n = 0; n < 128; ++n) {
        damping_[n] = held_[(size_t)n] ? 1.f : lift;
    }
}

float PedalState::dampingFor(int midi) const {
    if (midi < 0 || midi > 127) return 0.f;
    return damping_[midi];
}

} // namespace ithaca
```

- [ ] **Step 4: Registruj v CMake** — pridej `engine/pedal/pedal_state.cpp` do `ithaca_core`;
v `tests/CMakeLists.txt`:

```cmake
add_executable(test_pedal_state test_pedal_state.cpp)
target_link_libraries(test_pedal_state PRIVATE ithaca_core doctest)
add_test(NAME test_pedal_state COMMAND test_pedal_state)
```

- [ ] **Step 5: Build + test**

Run: `cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_pedal_state`
Expected: 5 cases projdou.

- [ ] **Step 6: Commit**

```bash
git add engine/pedal/pedal_state.h engine/pedal/pedal_state.cpp tests/test_pedal_state.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze5): PedalState — CC64 + undamped strings bitmap

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: harmonicky model `harmonicProximity` (TDD)

**Files:**
- Create: `engine/resonance/harmonic_proximity.h`, `engine/resonance/harmonic_proximity.cpp`
- Test: `tests/test_harmonic_proximity.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Cista funkce `float harmonicProximity(int target_midi, int source_midi)` vrati vahu [0, 1]
podle toho, jak silne nota `source_midi` budi rezonanci na strune `target_midi`. Pro fazi 5
jednoduchy harmonic-overlap model:
- nota sama na sebe: 0.0 (resi se jinde, je to play-on-self)
- oktava (12, 24, 36 pultonu) → 0.8 — silna
- kvinta (7) → 0.6
- kvarta (5) → 0.3
- velka tercia (4) → 0.2
- mala tercia (3) → 0.1
- ostatne → 0.0

Funkce je symetricka: `harmonicProximity(N, M) == harmonicProximity(M, N)`. Model bere |M-N|
modulo 12 + bere v uvahu oktavove vzdalenosti (cim dal oktava, tim mensi vaha — viz nize).

- [ ] **Step 1: Napis selhavajici test `tests/test_harmonic_proximity.cpp`**

```cpp
// tests/test_harmonic_proximity.cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "resonance/harmonic_proximity.h"

using namespace ithaca;

TEST_CASE("nota sama na sebe = 0 (resi se jinde, play-on-self)") {
    CHECK(harmonicProximity(60, 60) == doctest::Approx(0.f));
}

TEST_CASE("oktavy klesaji s vzdalenosti") {
    float p1 = harmonicProximity(60, 72);   // +1 oktava
    float p2 = harmonicProximity(60, 84);   // +2 oktavy
    float p3 = harmonicProximity(60, 96);   // +3 oktavy
    CHECK(p1 > p2);
    CHECK(p2 > p3);
    CHECK(p1 > 0.5f);
    CHECK(p3 > 0.f);
}

TEST_CASE("kvinta > kvarta > tercia") {
    float q5 = harmonicProximity(60, 67);   // kvinta
    float q4 = harmonicProximity(60, 65);   // kvarta
    float t3 = harmonicProximity(60, 64);   // velka tercia
    CHECK(q5 > q4);
    CHECK(q4 > t3);
}

TEST_CASE("tritonus / nahodne intervaly = ~0") {
    CHECK(harmonicProximity(60, 66) < 0.05f);   // tritonus
    CHECK(harmonicProximity(60, 61) < 0.05f);   // sekunda
}

TEST_CASE("symetrie: f(a,b) == f(b,a)") {
    for (int a : {21, 36, 60, 84, 108}) {
        for (int b : {24, 48, 67, 72, 96}) {
            CHECK(harmonicProximity(a, b) == doctest::Approx(harmonicProximity(b, a)));
        }
    }
}
```

- [ ] **Step 2: Vytvor `engine/resonance/harmonic_proximity.h`**

```cpp
#pragma once
// engine/resonance/harmonic_proximity.h
// -------------------------------------
// Vraci harmonickou "blizkost" mezi dvema MIDI strunami [0, 1] — jak silne
// nota `source` budi rezonanci na strune `target`. Cista funkce, bez stavu.
//
// Model (faze 5, jednoduchy):
//   1. Vaha intervalu modulo 12: oktava 1.0, kvinta 0.6, kvarta 0.3, ...
//   2. Klesa s oktavovou vzdalenosti — pricitam (12-step distance)*octave_decay.
//   3. f(N, N) = 0 (play-on-self resi voice_pool, ne rezonance).
//
// FUTURE (faze 5+): mozna nahradit FFT-based modelem podle skutecne spektralni
// energie samplu — to az kdyz budou nove banky.

namespace ithaca {

float harmonicProximity(int target_midi, int source_midi);

} // namespace ithaca
```

- [ ] **Step 3: Vytvor `engine/resonance/harmonic_proximity.cpp`**

```cpp
// engine/resonance/harmonic_proximity.cpp — viz .h
#include "resonance/harmonic_proximity.h"

#include <cmath>
#include <cstdlib>

namespace ithaca {

namespace {

// Vahy intervalu modulo 12 (semiton 0..11). Hodnoty vychazi z harmonicke
// rady: kazdy obsazeny term v overtone serii dostane vahu cca podle sve
// energetiky.
constexpr float kIntervalWeight[12] = {
    1.00f,  // 0:  unison (vraci se 0 pres self-check, ale vaha pro oktavu)
    0.05f,  // 1:  m2
    0.05f,  // 2:  M2
    0.10f,  // 3:  m3
    0.20f,  // 4:  M3
    0.30f,  // 5:  P4
    0.03f,  // 6:  tritonus
    0.60f,  // 7:  P5
    0.10f,  // 8:  m6
    0.15f,  // 9:  M6
    0.10f,  // 10: m7
    0.20f,  // 11: M7
};

// Oktavovy pokles: kazda oktava vzdalenost x0.7
constexpr float kOctaveDecay = 0.7f;

} // namespace

float harmonicProximity(int target_midi, int source_midi) {
    if (target_midi == source_midi) return 0.f;
    int diff = std::abs(target_midi - source_midi);
    int octaves = diff / 12;
    int semis   = diff % 12;
    float w = kIntervalWeight[semis];
    // Pokles s oktavovou vzdalenosti.
    for (int i = 0; i < octaves; ++i) w *= kOctaveDecay;
    return w;
}

} // namespace ithaca
```

- [ ] **Step 4: CMake** — pridej `engine/resonance/harmonic_proximity.cpp` do `ithaca_core`;
v `tests/CMakeLists.txt`:

```cmake
add_executable(test_harmonic_proximity test_harmonic_proximity.cpp)
target_link_libraries(test_harmonic_proximity PRIVATE ithaca_core doctest)
add_test(NAME test_harmonic_proximity COMMAND test_harmonic_proximity)
```

- [ ] **Step 5: Build + test**

Run: `cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_harmonic_proximity`
Expected: 5 cases projdou.

- [ ] **Step 6: Commit**

```bash
git add engine/resonance/harmonic_proximity.h engine/resonance/harmonic_proximity.cpp tests/test_harmonic_proximity.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze5): harmonicProximity — jednoduchy harmonic-overlap model pro rezonanci

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: doplnit nacteni `preload_resonance` v loaderu

**Files:**
- Modify: `engine/sample/sample_store.cpp`
- Modify: `engine/sample/sample_store.h` (pridat `int resonance_window_ms = 500` parametr)
- Modify: `engine/engine.{h,cpp}` (pridat `int resonance_window_ms = 500` do EngineConfig)
- Test: `tests/test_sample_store.cpp` (1 novy case: dlouhy sampl ma neprazdny preload_resonance)

Po fazi 4 mame v `MicLayer` `preload_resonance + resonance_start_frame + resonance_frames`, ale
loader je nechava prazdne. Faze 5 to dopni:

- Pro `Streamed` mic: `resonance_start_frame = asset.attack_end_frame` (jiz mereno), nacti vyrez
  `[resonance_start_frame, +resonance_window_frames]` pres `readWavRange`.
- Pro `FullyLoaded` mic: preload_resonance zustava prazdny (cely sampl je v preload_head, vc.
  oblasti od peak_RMS — rezonancni hlas ho cte primo z preload_head s offsetem).
- Pro mic kde `resonance_window_frames <= 0` (kratky window): nechame prazdne, rezonance se
  prizpusobi (viz Task 5 — fallback na preload_head).

POZN: attack_end_frame se ve fazi 4 meri z preload_head (krátké), tj. je vždy <= head_frames.
Pro Streamed sampl tedy `resonance_start_frame` je v head regionu nebo tesne za nim — to neva,
rezonancni hlas si vezme prislusna data podle toho, jestli je v head_frames nebo dal (Task 5).

- [ ] **Step 1: Uprav `engine/sample/sample_store.h`**

Pridej parametr `int resonance_window_ms = 500` na konec signatury `loadLegacyBank`.

- [ ] **Step 2: Uprav `engine/sample/sample_store.cpp`** — najdi blok `// preload_resonance ve
fazi 4 zatim prazdny (rezonance je faze 5).` a NAHRAD ho:

```cpp
        // Nacti preload_resonance pro Streamed mic (faze 5: zdroj rezonancnich
        // hlasu — preskoceny attack, drzeny sustain). Pro FullyLoaded je cely
        // sampl v preload_head, takze separatni resonance buffer netreba.
        if (mic.mode == MicLayerMode::Streamed && resonance_window_ms > 0) {
            mic.resonance_start_frame = ae;          // = attack_end_frame
            int rwin = (int)((int64_t)resonance_window_ms * info.sample_rate / 1000);
            // Orizni window na to, co soubor jeste obsahuje.
            int avail = info.frames - mic.resonance_start_frame;
            if (avail < 0) avail = 0;
            if (rwin > avail) rwin = avail;
            if (rwin > 0) {
                WavData rd = readWavRange(entry.full_path,
                                          mic.resonance_start_frame, rwin);
                if (rd.valid && rd.frames > 0) {
                    mic.resonance_frames  = rd.frames;
                    mic.preload_resonance = std::move(rd.samples);
                } else {
                    logger.log("bank", log::Severity::Warning,
                               "Nelze nacist preload_resonance: %s",
                               p.filename.c_str());
                }
            }
        }
```

Predej `resonance_window_ms` z `EngineConfig.resonance_window_ms` (uz mame v config.json od
faze 1; engine.cpp ho jen propoji).

- [ ] **Step 3: Uprav `engine/engine.h`** — pridej do `EngineConfig`:

```cpp
    int   resonance_window_ms = 500;  // delka preload_resonance regionu (Streamed mic)
```

- [ ] **Step 4: Uprav `engine/engine.cpp`** — v `loadBank` predej dale:

```cpp
    bank_ = loadLegacyBank(dir, L, /*cache_budget_mb=*/0,
                           cfg_.midi_from, cfg_.midi_to,
                           cfg_.preload_ms, cfg_.resonance_window_ms);
```

- [ ] **Step 5: Pridej test do `tests/test_sample_store.cpp`**

```cpp
TEST_CASE("loadLegacyBank: Streamed sampl nacita i preload_resonance") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_fixture_resonance";
    fs::remove_all(dir); fs::create_directories(dir);
    // 60 000 frames @48k = 1.25 s -> Streamed (>2*preload_ms=14400 pro 150 ms).
    writeConstWav(dir + "/m060-vel4-f48.wav", 0.5f, 60000);

    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);
    Bank bank = loadLegacyBank(dir, L, /*cache_budget_mb=*/0,
                               /*midi_from=*/0, /*midi_to=*/127,
                               /*preload_ms=*/150,
                               /*resonance_window_ms=*/200);
    fs::remove_all(dir);

    REQUIRE(bank.notes[60].recorded);
    const MicLayer& mic = bank.notes[60].slots[0].variants[0].mics[0];
    CHECK(mic.mode == MicLayerMode::Streamed);
    CHECK(mic.head_frames == 7200);                   // 150 ms @ 48k
    // preload_resonance je nenulovy (max 200ms = 9600 frames, ori§nuto na zbytek).
    CHECK(mic.resonance_frames > 0);
    CHECK((int)mic.preload_resonance.size() == mic.resonance_frames * 2);
    CHECK(mic.resonance_start_frame == mic.slots_attack_end_or_0());
    // attack_end_frame na konstantnim signalu by mel byt brzy — over ze se vejde do preloadu.
    CHECK(mic.resonance_start_frame >= 0);
    CHECK(mic.resonance_start_frame < mic.file.frames);
}
```

POZN: `mic.slots_attack_end_or_0()` neexistuje — vymez si v testu helper nebo cti
`bank.notes[60].slots[0].variants[0].attack_end_frame` (asset, ne mic). Plus zkontroluj, ze
`resonance_start_frame == asset.attack_end_frame`.

Korigovany test:
```cpp
    const SampleAsset& a = bank.notes[60].slots[0].variants[0];
    const MicLayer& mic  = a.mics[0];
    CHECK(mic.mode == MicLayerMode::Streamed);
    CHECK(mic.head_frames == 7200);
    CHECK(mic.resonance_frames > 0);
    CHECK((int)mic.preload_resonance.size() == mic.resonance_frames * 2);
    CHECK(mic.resonance_start_frame == a.attack_end_frame);
```

- [ ] **Step 6: Build + ALL ctest** (existujici testy musi prejit i s novym parametrem):

```bash
cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure
```
Expected: vsechny testy zelene (14 z faze 4 + 1 novy resonance test + Task 1/2 testy = 17).

- [ ] **Step 7: Manualni overeni RAM dopadu na realne bance**

```bash
./build/ithaca-cli --inspect /Users/j/SoundBanks/Ithaca/as-blackgrand
```
Expected: RAM stoupne z ~38 MB o nejake MB (preload_resonance pro Streamed samply = 200 ms windows).
Zaloguj novou hodnotu (orientacne: 704 souboru × 200 ms × 48k × stereo × 4 B = ~54 MB navic, takze
celkem ~92 MB). Pokud rozdil neni — neco se nepripocita.

- [ ] **Step 8: Commit**

```bash
git add engine/sample/sample_store.h engine/sample/sample_store.cpp engine/engine.h engine/engine.cpp tests/test_sample_store.cpp
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze5): loader doplnuje preload_resonance pro Streamed mic

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `ResonanceVoice` (TDD)

**Files:**
- Create: `engine/voice/resonance_voice.h`, `engine/voice/resonance_voice.cpp`
- Test: pokryje az `tests/test_resonance_engine.cpp` v Tasku 5 (rezonancni hlas se vzdy testuje
  v kontextu engine; samostatne unit testy by jen duplikovaly Voice testy).
- Modify: `CMakeLists.txt`

Specializovany hlas pro rezonanci. Klicove odlisnosti od `Voice`:

- **Start**: nezahajuje na pozici 0, ale na `mic_->resonance_start_frame` (a cte z
  `preload_resonance`). Onset ramp nahradime kratkym fade-in na low gain (~10 ms).
- **Vstup**: amplituda se nepocita z velocity hrane noty, ale z funkce
  `gain = velocity_excite × harm_proximity × resonance_strength` (kde `velocity_excite` je vel
  hrane noty, ne rezonujicí). Funkce `addExcitation(velocity_M, harm_proximity)` pricte do
  `target_gain_` (multi-source) — voice si pak fade na novy target.
- **Konec**: voice se nikdy nemusi sam ukoncit volanim release. Misto toho ho extern (resonance_
  engine) ukonci pri `pedal_up` / `note_on_self` / `note_off + neni undamped` voláním `fade()`
  (fast 5 ms ramp).
- **Streaming**: stejne jako Voice — kdyz cursor opusti `preload_resonance`, prepne na ring;
  pri underrunu fade do ticha. PRO FAZI 5: `FullyLoaded` mic cte primo z `preload_head` s offsetem
  `resonance_start_frame` (preload_resonance je prazdny). Streamed mic cte z preload_resonance,
  pak z ringu (worker dostane `frame_off = resonance_start_frame + resonance_frames`).

API:
```cpp
class ResonanceVoice {
public:
    void setStreamEngine(StreamEngine* se) { stream_ = se; }
    // Spusti rezonancni hlas pro strunu N samplem z mic. initial_gain = celkovy
    // start gain. engine_sr potreba kvuli pos_inc + ramp prepoctum.
    void start(int midi, const MicLayer* mic, float initial_gain,
               float pan_l, float pan_r, float engine_sr);
    // Pricte buzeni od dalsi hrane noty M (multi-source) — meni target_gain_.
    void addExcitation(float excitation_gain);
    // Spust fast fade do ticha (note-on_self, pedal_up_for_unheld, ...).
    void fadeOut(float engine_sr);
    bool process(float* out_l, float* out_r, int n_samples) noexcept;
    bool active() const { return active_; }
    int  midi()   const { return midi_; }
    float currentLevel() const noexcept { return gain_; }
private:
    // Stejna mechanika cteni jako Voice, viz voice.cpp.
    // ... (members podobne Voice, viz nize)
};
```

Implementacni klice (detaily v `.cpp`):
- `position_` se inicializuje na `mic_->resonance_start_frame` (tj. globalni offset v souboru,
  ne lokalni v bufferu).
- Cteni: kdyz `position_ < mic_->resonance_start_frame + (FullyLoaded ? 0 : mic_->resonance_frames)`,
  cte z lokalniho bufferu (preload_resonance nebo s offsetem v preload_head). Pak prepne na ring
  stejne jako Voice.
- `gain_` smooth fade na `target_gain_` per audio block (typicky ~10 ms ramp).
- `fadeOut()` nastavi `target_gain_ = 0` + rychlejsi ramp (~5 ms), po dosazeni 0 → `active_ = false`.

- [ ] **Step 1: Vytvor `engine/voice/resonance_voice.h`** s plnou deklaraci dle vzoru Voice.
- [ ] **Step 2: Vytvor `engine/voice/resonance_voice.cpp`** — implementuj start + process +
  addExcitation + fadeOut. Pouzij stejny lookup pattern jako Voice (preload region pak ring
  po prekroceni).
- [ ] **Step 3: CMake** — pridej `engine/voice/resonance_voice.cpp` do `ithaca_core`.
- [ ] **Step 4: Build** — `make build`. Expected: clean. (Testy az Task 5.)
- [ ] **Step 5: Commit**

```bash
git add engine/voice/resonance_voice.h engine/voice/resonance_voice.cpp CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze5): ResonanceVoice — hlas pro rezonanci (skip-attack, multi-source gain)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: `ResonanceEngine` (TDD)

**Files:**
- Create: `engine/resonance/resonance_engine.h`, `engine/resonance/resonance_engine.cpp`
- Test: `tests/test_resonance_engine.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Centralni modul, ktery vynucuje 5.5.1 invariant + pravidlo B.

API:
```cpp
class ResonanceEngine {
public:
    ResonanceEngine(int max_resonance_voices = 32);
    void setStreamEngine(StreamEngine* se);
    void setStrength(float s);  // 0..1
    float strength() const;

    // Volane Enginem pri kazdem note-on hraneho hlasu.
    // bank: pro nalezeni samplu pro rezonujici struny.
    // voice_pool: pro zjisteni `notes_with_active_main_voice` (eligibility 1).
    // pedal: pro `undamped_strings` (eligibility 1).
    void onPlayedNoteOn(int played_midi, int velocity,
                        const Bank& bank, const VoicePool& pool,
                        const PedalState& pedal, float engine_sr);

    // Volane Enginem pri note-on hraneho hlasu — pravidlo B: pokud N rezonuje,
    // fast fade + free slot. (Volat PRED voice_pool.noteOn(N), aby aktualizace
    // `active_main_voices` byla az po fade.)
    void onSelfNoteOn(int played_midi, float engine_sr);

    // Volane pri prechodu pedalu down -> up — fade vsech rezonanci, ktere uz
    // nejsou undamped.
    void onPedalChanged(const PedalState& pedal, float engine_sr);

    // Render vsech rezonancnich hlasu (volane z Engine.processBlock).
    bool processBlock(float* out_l, float* out_r, int n_samples) noexcept;

    int activeCount() const noexcept;

private:
    // Per nota 1 slot — invariant 5.5.1 (2).
    std::array<std::unique_ptr<ResonanceVoice>, 128> voices_;
    std::atomic<float> strength_{0.5f};
    StreamEngine* stream_ = nullptr;
    int max_voices_;
};
```

Implementacni logika:

```cpp
void onPlayedNoteOn(played_midi M, vel V, bank, pool, pedal, sr):
    for N in 0..127:
        if N == M: continue                      // play-on-self
        if !pedal.isUndamped(N): continue        // eligibility (1) cast 1
        if pool.hasActiveMainVoice(N): continue  // eligibility (1) cast 2
        float harm = harmonicProximity(N, M)
        if harm < 0.05f: continue                // zanedbatelne
        float excite = (V / 127.f) * harm * strength_.load()
        if excite < 0.001f: continue
        if voices_[N] && voices_[N]->active():
            voices_[N]->addExcitation(excite)    // uniqueness (2)
        else:
            // Najdi sampl pro N (legacy: jediny slot; extended: ten s nejvyssim RMS — to neva,
            // rezonance je tichá, presnost dynamiky neni klicova). Pouzij mic[0] (main).
            const SampleAsset* a = bank.notes[N].slots[0].variants[0]    // pripadne if !empty
            const MicLayer* m = &a->mics[0]
            float pl, pr;  panForNote(N, pl, pr)
            if voices_[N] == nullptr: voices_[N] = make_unique<ResonanceVoice>()
            voices_[N]->setStreamEngine(stream_)
            voices_[N]->start(N, m, excite, pl, pr, sr)
            // Pocet aktivnich rezonancnich hlasu nesmi prekrocit max_voices_.
            // Pri prekroceni krad nejtissi rezonancni hlas (NIKOLI hrany hlas — viz spec).
            enforceVoiceBudget()
```

```cpp
void onSelfNoteOn(played_midi N, sr):       // pravidlo B
    if voices_[N] && voices_[N]->active():
        voices_[N]->fadeOut(sr)              // ResonanceVoice si pak rampuje na 0 a deaktivuje
        // POZN: slot ne-nullujeme okamzite — voice si dofade ve sve process(). Eligibility filter
        // se diva na voices_[N]->active() v processBlock + onPlayedNoteOn -> kontrolovat
        // active() && !fading_out_, ne jen active(). Detail: ResonanceVoice ma flag fadingOut().
```

```cpp
void onPedalChanged(pedal, sr):
    int diff[128]
    int n = pedal.notesNoLongerUndamped(diff, 128)
    for i in 0..n-1:
        N = diff[i]
        if voices_[N] && voices_[N]->active(): voices_[N]->fadeOut(sr)
```

```cpp
void enforceVoiceBudget():
    // Spocti active rezonance. Kdyz > max, najdi nejtissi a fadeOut.
    // Tim nikdy nekrademe HRANY hlas (5.5 invariant).
```

VoicePool potrebuje novou metodu `bool hasActiveMainVoice(int midi) const` — iteruje voices_
hleda jakykoli active voice s tim midi (vc. releasing). To je trivialni rozsireni (Task 5
soucasti).

- [ ] **Step 1: Pridej `VoicePool::hasActiveMainVoice(int midi)` const**

V `engine/voice/voice_pool.h` pridej deklaraci:
```cpp
    bool hasActiveMainVoice(int midi) const noexcept;
```
V `.cpp` implementuj:
```cpp
bool VoicePool::hasActiveMainVoice(int midi) const noexcept {
    for (const auto& v : voices_)
        if (v.active() && v.midi() == midi) return true;
    return false;
}
```

- [ ] **Step 2: Napis selhavajici test `tests/test_resonance_engine.cpp`**

Test pouziva malou fixture banku (4 noty: 60, 64, 67, 72 — C major triad + oktava), vytvori
Engine, ResonanceEngine, posila noteOn/noteOff/sustain a kontroluje:

```cpp
// tests/test_resonance_engine.cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "resonance/resonance_engine.h"
#include "voice/voice_pool.h"
#include "pedal/pedal_state.h"
// ... + helpery na vytvoreni male fixture banky (viz wzor z test_sample_store)

using namespace ithaca;

// Fixture banka: 4 noty (60, 64, 67, 72) s krátkymi samply.
// (Sdileny helper s test_long_sample_stream — duplikuj nebo extrahuj do
// tests/common/.)

TEST_CASE("ResonanceEngine: eligibility (1) — N s aktivni main voice neni eligible") {
    // ... priprav bank, pool, pedal
    pedal.setSustainCC(127);                  // pedal dolu, vse undamped
    pool.noteOn(60, spec_for_60, sr);         // hraje 60
    res.onPlayedNoteOn(60, 100, bank, pool, pedal, sr);
    // Nesmi vzniknout rezonance na 60 (sebe-self) — vyplyva uz z play-on-self check.
    // Hraj jeste 64.
    res.onPlayedNoteOn(64, 100, bank, pool, pedal, sr);
    // Rezonance na 60 by mela vzniknout od buzeni 64? NE — protoze 60 ma aktivni main voice.
    CHECK_FALSE(res.isResonating(60));
    // Ale rezonance na 67 ano (67 nehraje).
    CHECK(res.isResonating(67));
}

TEST_CASE("ResonanceEngine: uniqueness (2) — multi-source jen aktualizuje amplitudu") {
    // ... pedal dolu
    res.onPlayedNoteOn(60, 80, bank, pool, pedal, sr);
    int after1 = res.activeCount();
    res.onPlayedNoteOn(64, 80, bank, pool, pedal, sr);
    int after2 = res.activeCount();
    // M=64 budi N=72 (1 oktava), takze pribyde +1 rezonance (72). Ale rezonance na 67 (kterou
    // jiz budila 60) NEPRIBYDE druhá; jen se aktualizuje amplitudu.
    CHECK((after2 - after1) <= 4);  // +rezonance jen na nezarezonujicich strunach
    // Pricna kontrola — pres "expected unique slots":
    CHECK(res.isResonating(67));
    CHECK(res.activeCountForNote(67) == 1);  // ne 2 (pravidlo uniqueness)
}

TEST_CASE("ResonanceEngine: pravidlo B — note-on na rezonujici notu zafade rezonanci") {
    pedal.setSustainCC(127);
    res.onPlayedNoteOn(60, 100, bank, pool, pedal, sr);  // 60 budi rezonance napr. na 67, 72
    REQUIRE(res.isResonating(67));
    // Hraj 67 (sebe).
    res.onSelfNoteOn(67, sr);
    pool.noteOn(67, spec_for_67, sr);
    // Po onSelfNoteOn rezonance 67 fade out. process few blocks, mela by zaniknout.
    std::vector<float> L(256, 0), R(256, 0);
    for (int i = 0; i < 50; ++i) res.processBlock(L.data(), R.data(), 256);  // 50*256 = 12800 frames @48k ~ 267 ms
    CHECK_FALSE(res.isResonating(67));
}

TEST_CASE("ResonanceEngine: pedal UP -> rezonance not, ktere nejsou drzene, fade") {
    pedal.setSustainCC(127);
    res.onPlayedNoteOn(60, 100, bank, pool, pedal, sr);
    REQUIRE(res.isResonating(67));
    pedal.setSustainCC(0);  // pedal nahoru
    res.onPedalChanged(pedal, sr);
    // 67 neni held → fade.
    std::vector<float> L(256, 0), R(256, 0);
    for (int i = 0; i < 50; ++i) res.processBlock(L.data(), R.data(), 256);
    CHECK_FALSE(res.isResonating(67));
}
```

POZN: `ResonanceEngine` pridej helper `bool isResonating(int midi) const` a `int
activeCountForNote(int midi) const` pro test (jen iteruje voices_[midi]->active()).

- [ ] **Step 3: Vytvor `engine/resonance/resonance_engine.h`** + `.cpp` dle algoritmu vyse.
- [ ] **Step 4: CMake + test registrace.**
- [ ] **Step 5: Build + test**

Run: `cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_resonance_engine`
Expected: 4 cases projdou.

- [ ] **Step 6: Commit**

```bash
git add engine/resonance/resonance_engine.h engine/resonance/resonance_engine.cpp engine/voice/voice_pool.h engine/voice/voice_pool.cpp tests/test_resonance_engine.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze5): ResonanceEngine — eligibility + uniqueness + pravidlo B

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: integrace do `Engine` + half-pedal release scaling + CLI parametr

**Files:**
- Modify: `engine/engine.h`, `engine/engine.cpp`
- Modify: `app/cli/main.cpp` (CLI parametr `--resonance-strength <0..1>`)

Klicove: zapojit PedalState + ResonanceEngine + drainovat Sustain evnety + volat metody ve
SPRAVNEM PORADI (viz 5.5.1 hranicni MIDI poradi):

```
processBlock(L, R, n):
    while pop MidiEvent e:
        switch e.type:
            NoteOn(M, V):
                pedal_.noteOn(M)
                resonance_.onSelfNoteOn(M, sr)     // pravidlo B PRED voice noteOn
                spec = selectVoice(bank_, M, V, rr_)
                if spec.asset: pool_.noteOn(M, spec, sr, keyboard_spread)
                resonance_.onPlayedNoteOn(M, V, bank_, pool_, pedal_, sr)  // PO voice noteOn
            NoteOff(M):
                pedal_.noteOff(M)
                pool_.noteOff(M, scaled_release_ms(pedal_), sr)
                // Rezonance se ne-startuje, jen flushne eligibility (pres next note-on).
            Sustain(cc):
                bool was_down = pedal_.isPedalDown()
                pedal_.setSustainCC(cc)
                if was_down && !pedal_.isPedalDown():
                    resonance_.onPedalChanged(pedal_, sr)
            AllNotesOff:
                pedal_.allNotesOff()
                pool_.allNotesOff(release_ms, sr)
                // Vsechny rezonance fade — onPedalChanged to udela kdyz prepoctene undamped je prazdne.
                resonance_.onPedalChanged(pedal_, sr)
    pool_.processBlock(L, R, n, sr)
    resonance_.processBlock(L, R, n)              // pricte rezonance do L, R
    apply master gain
```

Half-pedal release scaling (5.4 spec — continuous):
```cpp
float scaledReleaseMs(const PedalState& p) const {
    // CC 0 → release_ms × 1.0 (rychly fade)
    // CC 64 → release_ms × ~4.0 (pomaly fade s pedalem)
    // CC 127 → release_ms × ~20.0 (skoro hold)
    // Interpolace exponencialne: kf = exp((cc/127) × log(20))
    float t  = (float)p.sustainCC() / 127.f;
    float kf = std::exp(t * std::log(20.f));
    return cfg_.release_ms * kf;
}
```

CLI: pridej `--resonance-strength <0..1>` (default 0.5). Aplikuje `res_.setStrength(...)`.

- [ ] **Step 1: Uprav `engine.h`** — pridej `PedalState pedal_;` a
  `std::unique_ptr<ResonanceEngine> resonance_;` jako members. Pridej do `EngineConfig` polozku
  `float resonance_strength = 0.5f;` a `int max_resonance_voices = 32;`.
- [ ] **Step 2: Uprav `engine.cpp`** — v `init()` vytvor resonance, propoji StreamEngine.
  V `processBlock()` prepis drain logiku dle vyse uvedeneho pseudokodu. Pricti scaledReleaseMs
  pro NoteOff.
- [ ] **Step 3: Uprav `app/cli/main.cpp`** — pridej parsovani `--resonance-strength <f>`,
  predej do `cfg_.resonance_strength`. Pridej parametr do usage text.
- [ ] **Step 4: Manualni overeni live na realne bance**

```bash
./build/ithaca-cli --play /Users/j/SoundBanks/Ithaca/as-blackgrand --resonance-strength 0.8
# zahraj akord; pak druhy akord s pedalem → mela by byt slysitelna rezonance navic
# pak zkus stisk noty, ktera prave rezonuje → bez cvaknuti
```

Pri rendere:
```bash
./build/ithaca-cli --render /Users/j/SoundBanks/Ithaca/as-blackgrand --out /tmp/test5.wav
# render pouziva default config, takze rezonance ~0.5; over ze WAV je vetsi co do
# velikosti (rezonance prida dalsi hlasy → vetsi mix).
```

- [ ] **Step 5: All ctest + smoke**

```bash
make build && make test && make smoke
```
Expected: 17+ testu projde (faze 4 + Tasky 1, 2, 3, 5 testy).

- [ ] **Step 6: Commit**

```bash
git add engine/engine.h engine/engine.cpp app/cli/main.cpp
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze5): Engine integruje pedal + resonance, half-pedal release scaling, CLI --resonance-strength

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: live overeni + merge

- [ ] **Step 1: Live overeni `--play`** s rezonanci:

```bash
./build/ithaca-cli --play /Users/j/SoundBanks/Ithaca/as-blackgrand --resonance-strength 0.7
```

Tested scenarios:
1. Zahraj akord BEZ pedalu — slabe rezonance jen na drzenych nota.
2. Stiskni pedal (na MIDI keyb), pak zahraj akord — slysitelna sympaticka rezonance ostatnich strun.
3. Stiskni notu, ktera prave rezonuje — bez cvaknuti, novy ton zacne.
4. Pust pedal — rezonance ostatnich strun zatlumi (fade).
5. Held-key rezonance: pedal nahoru, drz nizkou notu, pak hraj vysoke noty — nizka by mela slabе
   rezonovat (held-key set).

Zaloguj subjektivni dojem + zda hraje LOG_RT_WARN ohledne resonance budgetu nebo underrunu.

- [ ] **Step 2: Pokud OK** → merge:

```bash
git checkout main
git merge --no-ff faze5-pedal-rezonance -m "$(cat <<'EOF'
Merge faze5-pedal-rezonance: sympaticka rezonance + pedal (sustain + half-pedal)

Faze 5 hotova: PedalState (CC64 + undamped strings bitmap), harmonicProximity
model (oktavy + kvinty + ... × octave decay), preload_resonance region v
loaderu pro Streamed mic, ResonanceVoice (skip-attack + multi-source gain
update), ResonanceEngine (eligibility filter + per-nota uniqueness +
pravidlo B + multi-source aggregation). Engine integruje pedal/rezonance v
spravnem poradi v processBlock. Half-pedal continuous release scaling.
CLI --resonance-strength. Held-key + pedal rezonance v jednom kodu.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
git branch -d faze5-pedal-rezonance
git push
```

---

## Hotovo — kriteria dokonceni faze 5

- `make build` cisty, `make test` 100% (faze 1-4 testy + nove test_pedal_state, test_harmonic_
  proximity, test_resonance_engine, plus rozsirene test_sample_store).
- `--inspect` na as-blackgrand: RAM stoupla z ~38 MB o nejake MB (preload_resonance windows).
- `--play` na as-blackgrand: slysitelna sympaticka rezonance pri stisknutem pedalu; bez cvaknuti
  pri stisku noty, ktera prave rezonuje; pedal UP zatlumi rezonance ostatnich strun.
- 5.5.1 invariant DRZI: nikde se nesoucasne hraje N jako main voice a rezonancni hlas.
- ResonanceEngine ma vlastni voice budget (default 32); pri prekroceni krad nejtissi rezonancni
  hlas, NIKDY nesmi ukrast hrany hlas (active_main_voice).

Tim mame zive prehravani sampleru se sympaticky reagujicim pianem. Faze 6 (DSP chain + mic_mixer)
nasleduje per puvodni plan.

---

## Otevrene body (pristi iterace, neblokuje fazi 5)

- FFT-based harmonic detection misto manualnich vah intervalu. Az kdyz budou nove banky a budeme
  mit cas porovnat s realnym pianem.
- Rezonance v extended bance: aktualne pouziva jen main mic (mics[0]). Mixovani vsech micpos pro
  rezonancni hlasy je faze 6 (mic_mixer).
- Half-pedal dedikovane samply: prijdou s extended bankou (faze 7). Faze 5 ma jen continuous
  release-time scaling jako fallback.
- ResonanceVoice currently nepouziva damping buffer pri retriggeru (rezonance se sama nesteaul,
  je per-nota unique). Pri zmene strategie (napr. multi-instance rezonance per oktava) toto by se
  muselo doresit.
