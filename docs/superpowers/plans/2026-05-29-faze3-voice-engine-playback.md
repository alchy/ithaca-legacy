# Faze 3 — voice pool + patch_manager + engine fasada + prehravani — implementacni plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (doporuceno) nebo superpowers:executing-plans. Kroky pouzivaji checkbox (`- [ ]`) syntaxi.
>
> Jazyk: komentare/docs/commit messages cesky bez diakritiky; identifikatory anglicky;
> komentovat spise vice (explicit > implicit).

**Goal:** Z "banka v RAM" udelat HRAJICI nastroj (headless): 128-hlasy voice pool, patch_manager
(MIDI → nota → velocity slot → round-robin → pitch-shift chybejicich not ve dvou osach), retrigger
pres fast-decay damp hlas, lock-free MIDI fronta, engine fasada (init/start/processBlock/stop),
audio vystup pres miniaudio, a offline batch render do WAV (testovani bez audio HW).

**Architecture:** Vse pribyva do `libithaca_core` jako oddelene moduly + dva nove TU pro audio.
Signalni cesta (faze 3, zjednodusena): voice pool → master gain → out. Mic mixer, DSP chain,
rezonance a streaming jsou DALSI faze — sem nepatri. Voice hraje jeden SampleAsset pres jeho
prvni mic perspektivu (legacy = 1 stereo mic). All-in-RAM (zadny streaming). Testovatelne jadro
(patch_manager, voice, voice_pool, engine, batch render) ma unit/integ testy; ziva audio device se
overuje manualne (nelze unit-testovat bez HW), proto je posledni a `make smoke` = batch render.

**Tech Stack:** C++20, navazuje na faze 1-2 (logger, wav_reader, sample_store, CMake, Makefile,
doctest). Audio: miniaudio (uz vendored, zatim nepouzity). Build/verifikace macOS.

---

## Kontext a zdrojove vzory

- Voice/resample/envelope predloha (OVERENA): icr `engine/cores/sampler/sampler_core.cpp` — voice
  `process()` loop (linearni interpolace pro SR/pitch konverzi pres `pos_inc`, onset ramp, release
  ramp, pan, damping crossfade buffer pro click-free retrigger), `VoiceManager::findFreeSlot`
  (kradez nejtissiho releasing/overall hlasu), `allocVoice`.
- Lock-free SPSC vzor: nas vlastni logger `engine/util/log.cpp` (vlogRT/flushRTBuffer — write index
  publish release/acquire, drop-on-full). MIDI fronta pouzije stejny princip.
- miniaudio integrace: icr2 `player/miniaudio_impl.cpp` (`#define MINIAUDIO_IMPLEMENTATION` v jedinem
  TU), `player/audio_device.{h,cpp}` (forward-declare `ma_device` v hlavicce, trampolina v .cpp).
- Datovy model: `engine/sample/sample_types.h` (Bank → NoteSlots[128] → VelocitySlot → SampleAsset
  → MicLayer). Bank loader: `engine/sample/sample_store.h` (`loadLegacyBank`).
- Spec: `docs/superpowers/specs/2026-05-29-ithaca-legacy-design.md` (sekce 5 voice/retrigger, 2.4
  pitch-shift dve osy, 2.2 velocity sloty).
- Realna dev banka: `/Users/j/SoundBanks/Ithaca/as-blackgrand` (88 not × 8 vel; CELA ~5.4 GB RAM!).
  Proto Task 1b prida MIDI-range filtr do loaderu, aby testy/render nacitaly jen par not.

### Vedome ODLOZENO mimo fazi 3 (nepatri sem)

- Streaming z disku (faze 4). Faze 3 je all-in-RAM.
- Mic mixer / multi-mic (faze 6) — voice hraje jen `mics[0]`.
- DSP chain, rezonance, half-pedaling (faze 5-6).
- True inter-slot velocity crossfade (dva assety na hlas). Faze 3 vybere JEDEN slot dle velocity
  (nearest na dynamicke krivce) + skaluje vel_gain. Sample-crossfade mezi vrstvami je pozdejsi
  polish — explicitne odlozeno.

### Cilova struktura po fazi 3 (nove soubory)

```
engine/
  io/
    wav_writer.h / .cpp      zapis stereo float → WAV (16-bit PCM) pro batch render + testy
  voice/
    voice.h / .cpp           jeden hlas: pozice, pitch ratio, envelope, pan, damping
    voice_pool.h / .cpp      128 slotu, alokace/kradez, retrigger, processBlock
    patch_manager.h / .cpp   MIDI → VoiceSpec (slot vyber, round-robin, pitch-shift 2 osy)
  midi/
    midi_queue.h             lock-free SPSC fronta MIDI udalosti (header-only)
  engine.h / .cpp            fasada: init/loadBank/start/stop, MIDI fronta, processBlock
  io/
    audio_device.h / .cpp    miniaudio wrapper (forward-declare ma_device)
  miniaudio_impl.cpp         jediny TU s MINIAUDIO_IMPLEMENTATION
  render/
    batch_renderer.h / .cpp  offline render not → WAV (bez audio device)
app/cli/main.cpp             + prikazy --render <dir> --out <wav> a --play <dir>
tests/
  test_patch_manager.cpp
  test_voice_pool.cpp
  test_midi_queue.cpp
  test_engine.cpp
  test_batch_renderer.cpp
```

### Konvence commitu (jako faze 1-2)

`typ(faze3): popis` cesky bez diakritiky; `git add` konkretni soubory; trailer
`Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`; commit pres
`git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit`.

Pred zacatkem: vytvor a prepni se na vetev `faze3-voice-engine` (`git checkout -b faze3-voice-engine`
z `main`).

---

## Task 1: WAV writer (TDD)

**Files:**
- Create: `engine/io/wav_writer.h`, `engine/io/wav_writer.cpp`
- Test: `tests/test_wav_writer.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Zapise interleaved stereo float buffer do 16-bit PCM stereo WAV. Pouzije ho batch renderer (Task 6)
a integracni testy. Round-trip s existujicim `readWav` je idealni test.

- [ ] **Step 1: Napis selhavajici test `tests/test_wav_writer.cpp`**

```cpp
// tests/test_wav_writer.cpp
// Round-trip: zapis buffer pres writeWav, precti pres readWav, over shodu.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "io/wav_writer.h"
#include "io/wav_reader.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace ithaca;

TEST_CASE("writeWav + readWav round-trip zachova hodnoty a SR") {
    std::vector<float> in = {0.5f, -0.5f, 0.25f, -0.25f, 0.f, 1.f, -1.f, 0.75f};  // 4 stereo frames
    std::string p = "/tmp/ithaca_wavwriter_rt.wav";
    REQUIRE(writeWavStereo16(p, in, 48000));
    WavData w = readWav(p);
    std::remove(p.c_str());
    REQUIRE(w.valid);
    CHECK(w.sample_rate == 48000);
    CHECK(w.frames == 4);
    REQUIRE(w.samples.size() == 8u);
    for (size_t i = 0; i < in.size(); ++i)
        CHECK(w.samples[i] == doctest::Approx(in[i]).epsilon(0.001));
}

TEST_CASE("writeWav clampuje mimo rozsah a vrati false pro nezapisovatelnou cestu") {
    std::vector<float> in = {2.0f, -2.0f};   // bude clampnuto na +1/-1
    std::string p = "/tmp/ithaca_wavwriter_clamp.wav";
    REQUIRE(writeWavStereo16(p, in, 44100));
    WavData w = readWav(p);
    std::remove(p.c_str());
    REQUIRE(w.valid);
    CHECK(w.samples[0] == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(w.samples[1] == doctest::Approx(-1.0f).epsilon(0.001));
    // Nezapisovatelna cesta (neexistujici adresar) → false.
    CHECK_FALSE(writeWavStereo16("/tmp/ithaca_nope_dir_xyz/out.wav", in, 48000));
}
```

- [ ] **Step 2: Vytvor `engine/io/wav_writer.h`**

```cpp
#pragma once
// engine/io/wav_writer.h
// ----------------------
// Zapis interleaved stereo float bufferu do 16-bit PCM stereo WAV. Pouziva
// batch renderer (offline render bez audio device) a testy. Float [-1,1] se
// clampuje a kvantuje na int16.

#include <string>
#include <vector>

namespace ithaca {

// Zapise `samples` (interleaved stereo [L,R,...]) jako 16-bit PCM stereo WAV.
// Vrati false kdyz nelze otevrit soubor pro zapis.
bool writeWavStereo16(const std::string& path, const std::vector<float>& samples,
                      int sample_rate);

} // namespace ithaca
```

- [ ] **Step 3: Vytvor `engine/io/wav_writer.cpp`**

```cpp
// engine/io/wav_writer.cpp — viz wav_writer.h.
#include "io/wav_writer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace ithaca {

namespace {
void wU32(std::FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void wU16(std::FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }
} // namespace

bool writeWavStereo16(const std::string& path, const std::vector<float>& samples,
                      int sample_rate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    const uint32_t n_samp    = (uint32_t)samples.size();      // celkem L+R hodnot
    const uint32_t data_size = n_samp * 2u;                   // 2 bajty / hodnota (16-bit)
    const uint16_t channels  = 2;
    const uint32_t byte_rate = (uint32_t)sample_rate * channels * 2u;

    std::fwrite("RIFF", 1, 4, f); wU32(f, 36u + data_size);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); wU32(f, 16u);
    wU16(f, 1);                                               // PCM
    wU16(f, channels);
    wU32(f, (uint32_t)sample_rate);
    wU32(f, byte_rate);
    wU16(f, (uint16_t)(channels * 2));                        // block align
    wU16(f, 16);                                              // bits per sample
    std::fwrite("data", 1, 4, f); wU32(f, data_size);

    for (float s : samples) {
        if (s > 1.f) s = 1.f;
        if (s < -1.f) s = -1.f;
        int16_t v = (int16_t)std::lround(s * 32767.f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

} // namespace ithaca
```

- [ ] **Step 4: CMake** — pridej `engine/io/wav_writer.cpp` do `ithaca_core`; v `tests/CMakeLists.txt`:

```cmake
add_executable(test_wav_writer test_wav_writer.cpp)
target_link_libraries(test_wav_writer PRIVATE ithaca_core doctest)
add_test(NAME test_wav_writer COMMAND test_wav_writer)
```

- [ ] **Step 5: Build + test**

Run: `cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_wav_writer`
Expected: `test_wav_writer` projde (2 cases).

- [ ] **Step 6: Commit**

```bash
git add engine/io/wav_writer.h engine/io/wav_writer.cpp tests/test_wav_writer.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze3): WAV writer (stereo 16-bit PCM) pro batch render + testy

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 1b: MIDI-range filtr v loadLegacyBank

**Files:**
- Modify: `engine/sample/sample_store.h`, `engine/sample/sample_store.cpp`
- Test: `tests/test_sample_store.cpp` (pridat 1 case)

Cela `as-blackgrand` zabere ~5.4 GB RAM — nepouzitelne pro rychle testy a render. Pridame
volitelny MIDI rozsah; mimo nej se soubory preskoci (nenacitaji se). Default = cely rozsah (zpetne
kompatibilni s fazi 2).

- [ ] **Step 1: Rozsir `loadLegacyBank` signaturu v `sample_store.h`**

Zmen deklaraci na:

```cpp
// midi_from / midi_to: nacti jen noty v tomto inkluzivnim rozsahu (default 0..127 = vse).
// Slouzi k rychlemu testovani/renderu bez nacteni cele (vicegb) banky.
Bank loadLegacyBank(const std::string& dir, log::Logger& logger,
                    int cache_budget_mb = 0,
                    int midi_from = 0, int midi_to = 127);
```

- [ ] **Step 2: Uprav `sample_store.cpp`** — hned po `if (p.midi < 0 || p.midi > 127) continue;`
pridej rozsahovy filtr:

```cpp
        if (p.midi < midi_from || p.midi > midi_to) continue;   // mimo pozadovany rozsah
```

(Parametry `midi_from`/`midi_to` jsou uz v signature; jen je pouzij.)

- [ ] **Step 3: Pridej test do `tests/test_sample_store.cpp`**

```cpp
TEST_CASE("loadLegacyBank respektuje MIDI rozsah") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_fixture_range";
    fs::remove_all(dir);
    fs::create_directories(dir);
    writeConstWav(dir + "/m060-vel3-f48.wav", 0.5f);
    writeConstWav(dir + "/m072-vel3-f48.wav", 0.5f);
    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);
    // Nacti jen notu 60 (rozsah 60..60).
    Bank bank = loadLegacyBank(dir, L, /*cache_budget_mb=*/0, /*midi_from=*/60, /*midi_to=*/60);
    fs::remove_all(dir);
    CHECK(bank.loaded_samples == 1);
    CHECK(bank.notes[60].recorded);
    CHECK_FALSE(bank.notes[72].recorded);
}
```

- [ ] **Step 4: Build + test**

Run: `cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_sample_store`
Expected: vsechny sample_store testy projdou (puvodni 2 + novy + faze-2 range nic nerozbije).

- [ ] **Step 5: Commit**

```bash
git add engine/sample/sample_store.h engine/sample/sample_store.cpp tests/test_sample_store.cpp
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze3): MIDI-range filtr v loadLegacyBank pro rychle testy/render

Cela as-blackgrand je ~5.4 GB RAM. Volitelny midi_from/midi_to (default
0..127) umozni nacist jen par not. Streaming (faze 4) bude prave reseni.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: patch_manager (TDD)

**Files:**
- Create: `engine/voice/patch_manager.h`, `engine/voice/patch_manager.cpp`
- Test: `tests/test_patch_manager.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Ciste rozhodovaci funkce nad `Bank` (zadne I/O, zadny audio stav): pro (midi, velocity) vrati
`VoiceSpec` = ktery SampleAsset hrat, s jakym pitch ratiem (transpozice u chybejicich not) a
vel_gain. Round-robin stav je drzeny zvlast (per nota+slot), aby funkce sla testovat deterministicky.

Algoritmus (spec 2.4 dve osy + 2.2 velocity):
1. Najdi zdrojovou notu: kdyz `notes[midi].recorded`, je to `midi` (pitch_ratio=1). Jinak najdi
   NEJBLIZSI nahranou notu (min |delta|) a `pitch_ratio = 2^(delta/12)` (delta = midi - source).
2. Ve zdrojove note vyber velocity slot: namapuj velocity 0-127 na index slotu po dynamicke krivce
   (sloty jsou serazene vzestupne dle RMS). `idx = round(velocity/127 * (nslots-1))`. (Nearest na
   krivce; sample-crossfade mezi sloty je odlozeny — viz "ODLOZENO".)
3. Round-robin: ze slotu vyber variantu nahodne, ale neopakuj naposledy hranou (per nota+slot).
4. vel_gain: `(velocity/127)^2` (percepcni, jako icr).

- [ ] **Step 1: Napis selhavajici test `tests/test_patch_manager.cpp`**

```cpp
// tests/test_patch_manager.cpp
// Testuje vyber hlasu: velocity→slot, pitch-shift chybejicich not (2 osy),
// round-robin bez opakovani. Banka se staví v pameti (zadny disk).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "voice/patch_manager.h"
#include "sample/sample_types.h"

#include <cmath>

using namespace ithaca;

namespace {
// Pomocna: pridej notu s `nslots` sloty (kazdy 1 variant, rostouci RMS),
// volitelne `rr` round-robin variant v prvnim slotu.
void addNote(Bank& b, int midi, int nslots, int rr_in_slot0 = 1) {
    for (int s = 0; s < nslots; ++s) {
        VelocitySlot slot;
        slot.rms_db = -40.f + (float)s * 5.f;          // vzestupne
        int variants = (s == 0) ? rr_in_slot0 : 1;
        for (int v = 0; v < variants; ++v) {
            SampleAsset a;
            a.peak_rms_db = slot.rms_db;
            MicLayer m; m.mic_name = "stereo"; m.frames = 100; m.sample_rate = 48000;
            m.data.assign(100 * 2, 0.1f);
            a.mics.push_back(std::move(m));
            slot.variants.push_back(std::move(a));
        }
        b.notes[midi].slots.push_back(std::move(slot));
        b.notes[midi].recorded = true;
    }
}
} // namespace

TEST_CASE("selectVoice: presna nota → pitch_ratio 1.0") {
    Bank b; addNote(b, 60, 8);
    RoundRobinState rr;
    VoiceSpec vs = selectVoice(b, 60, 100, rr);
    REQUIRE(vs.asset != nullptr);
    CHECK(vs.pitch_ratio == doctest::Approx(1.0).epsilon(0.0001));
    CHECK(vs.vel_gain > 0.f);
}

TEST_CASE("selectVoice: chybejici nota → transpozice z nejblizsi") {
    Bank b; addNote(b, 60, 4);                          // jen nota 60
    RoundRobinState rr;
    VoiceSpec up = selectVoice(b, 62, 100, rr);         // o 2 pultony vys
    REQUIRE(up.asset != nullptr);
    CHECK(up.pitch_ratio == doctest::Approx(std::pow(2.0, 2.0/12.0)).epsilon(0.001));
    VoiceSpec dn = selectVoice(b, 57, 100, rr);         // o 3 pultony niz
    REQUIRE(dn.asset != nullptr);
    CHECK(dn.pitch_ratio == doctest::Approx(std::pow(2.0, -3.0/12.0)).epsilon(0.001));
}

TEST_CASE("selectVoice: vyssi velocity → vyssi slot (hlasitejsi)") {
    Bank b; addNote(b, 60, 8);
    RoundRobinState rr;
    VoiceSpec soft = selectVoice(b, 60, 1, rr);
    VoiceSpec loud = selectVoice(b, 60, 127, rr);
    REQUIRE(soft.asset != nullptr); REQUIRE(loud.asset != nullptr);
    // loud asset ma vyssi peak_rms_db nez soft (vybral hlasitejsi slot).
    CHECK(loud.asset->peak_rms_db > soft.asset->peak_rms_db);
}

TEST_CASE("selectVoice: round-robin neopakuje naposledy hranou variantu") {
    Bank b; addNote(b, 60, 1, /*rr_in_slot0=*/3);      // 1 slot, 3 RR varianty
    RoundRobinState rr;
    const SampleAsset* prev = nullptr;
    // Pri 3 variantach se zadne dve po sobe nesmi shodovat.
    for (int i = 0; i < 20; ++i) {
        VoiceSpec vs = selectVoice(b, 60, 64, rr);
        REQUIRE(vs.asset != nullptr);
        CHECK(vs.asset != prev);                        // nikdy stejny jako minuly
        prev = vs.asset;
    }
}

TEST_CASE("selectVoice: prazdna banka → asset nullptr") {
    Bank b;
    RoundRobinState rr;
    VoiceSpec vs = selectVoice(b, 60, 100, rr);
    CHECK(vs.asset == nullptr);
}
```

- [ ] **Step 2: Vytvor `engine/voice/patch_manager.h`**

```cpp
#pragma once
// engine/voice/patch_manager.h
// ----------------------------
// Rozhodovaci logika prehravani: pro (midi, velocity) vybere konkretni
// SampleAsset z banky + pitch ratio (transpozice u chybejicich not) + vel_gain.
// Ciste funkce nad Bank — bez I/O a bez audio stavu (snadno testovatelne).
// Round-robin stav je drzeny zvlast (RoundRobinState).

#include "sample/sample_types.h"

#include <cstdint>

namespace ithaca {

// Vysledek vyberu: co a jak hrat. asset == nullptr → neni co hrat.
struct VoiceSpec {
    const SampleAsset* asset       = nullptr;
    double             pitch_ratio = 1.0;   // nasobic rychlosti prehravani (transpozice)
    float              vel_gain    = 1.0f;  // gain z velocity
};

// Round-robin pamet: pro kazdou (notu, slot) posledni hranou variantu.
// Jednoduchy deterministicky RNG (LCG) — bez globalniho stavu, testovatelne.
struct RoundRobinState {
    // last[midi][slot] = index naposledy hrane varianty (-1 = jeste nehrano).
    // Pole je rezervovano lazy az pri pouziti, aby struct zustal levny.
    int  last[128][16];
    uint32_t rng = 0x12345678u;            // seed; meni se pri kazdem vyberu
    RoundRobinState() { for (auto& n : last) for (int& s : n) s = -1; }
};

// Vybere hlas pro (midi, velocity). Aktualizuje rr (round-robin + rng).
VoiceSpec selectVoice(const Bank& bank, int midi, int velocity, RoundRobinState& rr);

} // namespace ithaca
```

POZN.: `last[128][16]` predpoklada max 16 slotu na notu (legacy 8, rezerva). Kdyby mela nota vic
slotu, round-robin pamet se pro slot >=16 nepouzije (vybere se varianta 0) — bezpecne, jen mene
variabilni; extended format to doresi ve fazi 7.

- [ ] **Step 3: Vytvor `engine/voice/patch_manager.cpp`**

```cpp
// engine/voice/patch_manager.cpp — viz patch_manager.h.
#include "voice/patch_manager.h"

#include <cmath>

namespace ithaca {

namespace {

// Najde nejblizsi nahranou notu k `midi`. Vrati -1 kdyz banka nema zadnou.
int nearestRecordedNote(const Bank& bank, int midi) {
    if (bank.notes[midi].recorded) return midi;
    for (int d = 1; d < 128; ++d) {
        int lo = midi - d, hi = midi + d;
        if (lo >= 0 && bank.notes[lo].recorded)  return lo;
        if (hi < 128 && bank.notes[hi].recorded) return hi;
    }
    return -1;
}

// Namapuj velocity 0-127 na index slotu (sloty serazene vzestupne dle RMS).
int slotIndexForVelocity(int velocity, int nslots) {
    if (nslots <= 1) return 0;
    float t = (float)velocity / 127.f;                 // 0..1
    int idx = (int)std::lround(t * (float)(nslots - 1));
    if (idx < 0) idx = 0;
    if (idx >= nslots) idx = nslots - 1;
    return idx;
}

// LCG krok — levny deterministicky RNG (Numerical Recipes konstanty).
uint32_t lcgNext(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

} // namespace

VoiceSpec selectVoice(const Bank& bank, int midi, int velocity, RoundRobinState& rr) {
    VoiceSpec out;
    if (midi < 0 || midi > 127) return out;

    int src = nearestRecordedNote(bank, midi);
    if (src < 0) return out;                            // prazdna banka

    const NoteSlots& note = bank.notes[src];
    if (note.slots.empty()) return out;

    int nslots = (int)note.slots.size();
    int sidx = slotIndexForVelocity(velocity, nslots);
    const VelocitySlot& slot = note.slots[sidx];
    if (slot.variants.empty()) return out;

    // Round-robin: vyber variantu != naposledy hrana (kdyz je vic nez jedna).
    int nvar = (int)slot.variants.size();
    int chosen = 0;
    if (nvar == 1) {
        chosen = 0;
    } else {
        int last = (sidx < 16) ? rr.last[src][sidx] : -1;
        // Nahodny vyber, opakuj dokud != last (pri nvar>=2 vzdy skonci rychle).
        do {
            chosen = (int)(lcgNext(rr.rng) % (uint32_t)nvar);
        } while (chosen == last);
        if (sidx < 16) rr.last[src][sidx] = chosen;
    }

    out.asset       = &slot.variants[chosen];
    out.pitch_ratio = std::pow(2.0, (double)(midi - src) / 12.0);
    float vn = (float)velocity / 127.f;
    out.vel_gain    = vn * vn;                          // percepcni (kvadraticky)
    return out;
}

} // namespace ithaca
```

- [ ] **Step 4: CMake** — pridej `engine/voice/patch_manager.cpp` do `ithaca_core`; v tests:

```cmake
add_executable(test_patch_manager test_patch_manager.cpp)
target_link_libraries(test_patch_manager PRIVATE ithaca_core doctest)
add_test(NAME test_patch_manager COMMAND test_patch_manager)
```

- [ ] **Step 5: Build + test**

Run: `cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_patch_manager`
Expected: 5 cases projdou.

- [ ] **Step 6: Commit**

```bash
git add engine/voice/patch_manager.h engine/voice/patch_manager.cpp tests/test_patch_manager.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze3): patch_manager — vyber slotu dle velocity, pitch-shift 2 osy, round-robin

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: voice + voice_pool (TDD)

**Files:**
- Create: `engine/voice/voice.h`, `engine/voice/voice.cpp`,
  `engine/voice/voice_pool.h`, `engine/voice/voice_pool.cpp`
- Test: `tests/test_voice_pool.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

`Voice` hraje jeden SampleAsset pres `mics[0]` s linearni interpolaci (pitch/SR), onset+release
ramp, pan. `VoicePool` ma N slotu (default 128), alokuje volny / krade nejtissi, retrigger pres
damping crossfade. Adaptovano z icr `sampler_core.cpp` (overeny voice loop), ciste rozdelene.

- [ ] **Step 1: Napis selhavajici test `tests/test_voice_pool.cpp`**

```cpp
// tests/test_voice_pool.cpp
// Testuje render hlasu a chovani poolu na syntetickych SampleAssetech.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "voice/voice_pool.h"
#include "voice/patch_manager.h"
#include "sample/sample_types.h"

#include <cmath>
#include <vector>

using namespace ithaca;

namespace {
// SampleAsset s konstantni amplitudou, dany pocet frames, SR 48k.
SampleAsset makeAsset(float amp, int frames) {
    SampleAsset a;
    a.peak_rms_db = 0.f;
    MicLayer m; m.mic_name = "stereo"; m.frames = frames; m.sample_rate = 48000;
    m.data.assign((size_t)frames * 2, amp);
    a.mics.push_back(std::move(m));
    return a;
}
// Soucet absolutnich hodnot bufferu (hruba "energie").
double energy(const std::vector<float>& b) {
    double s = 0; for (float v : b) s += std::fabs((double)v); return s;
}
} // namespace

TEST_CASE("Voice prehraje nenulovy zvuk a pak utichne (konec samplu)") {
    SampleAsset a = makeAsset(0.5f, 256);
    VoicePool pool(8);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 1.0; vs.vel_gain = 1.0f;
    pool.noteOn(60, vs, /*engine_sr=*/48000.f);

    std::vector<float> L(512, 0.f), R(512, 0.f);
    bool any = pool.processBlock(L.data(), R.data(), 512, 48000.f);
    CHECK(any);
    CHECK(energy(L) > 0.0);                  // neco zaznelo
    // Po dohrani 256-frame samplu uz dalsi blok nic neprida.
    std::vector<float> L2(512, 0.f), R2(512, 0.f);
    pool.processBlock(L2.data(), R2.data(), 512, 48000.f);
    CHECK(energy(L2) == doctest::Approx(0.0));
}

TEST_CASE("noteOff spusti release a hlas dozni") {
    SampleAsset a = makeAsset(0.5f, 48000);  // 1 s, dost dlouhy
    VoicePool pool(8);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 1.0; vs.vel_gain = 1.0f;
    pool.noteOn(60, vs, 48000.f);
    std::vector<float> L(256, 0.f), R(256, 0.f);
    pool.processBlock(L.data(), R.data(), 256, 48000.f);
    CHECK(pool.activeCount() == 1);
    pool.noteOff(60, /*release_ms=*/10.f, 48000.f);
    // Po dostatku bloku (>10 ms = 480 frames) hlas zmizi.
    for (int i = 0; i < 10; ++i) {
        std::vector<float> b(256, 0.f), b2(256, 0.f);
        pool.processBlock(b.data(), b2.data(), 256, 48000.f);
    }
    CHECK(pool.activeCount() == 0);
}

TEST_CASE("pitch_ratio 2.0 prehraje sampl 2x rychleji (drive utichne)") {
    SampleAsset a = makeAsset(0.5f, 1000);
    VoicePool pool(8);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 2.0; vs.vel_gain = 1.0f;
    pool.noteOn(60, vs, 48000.f);
    // Pri 2x rychlosti je 1000-frame sampl prehrany za ~500 frames.
    std::vector<float> L(600, 0.f), R(600, 0.f);
    pool.processBlock(L.data(), R.data(), 600, 48000.f);
    std::vector<float> L2(100, 0.f), R2(100, 0.f);
    bool any2 = pool.processBlock(L2.data(), R2.data(), 100, 48000.f);
    CHECK_FALSE(any2);                        // uz dohrano
}

TEST_CASE("retrigger tehoz tonu neztrati hlas (porad 1 aktivni)") {
    SampleAsset a = makeAsset(0.5f, 48000);
    VoicePool pool(8);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 1.0; vs.vel_gain = 1.0f;
    pool.noteOn(60, vs, 48000.f);
    std::vector<float> b(256, 0.f), b2(256, 0.f);
    pool.processBlock(b.data(), b2.data(), 256, 48000.f);
    pool.noteOn(60, vs, 48000.f);            // retrigger
    pool.processBlock(b.data(), b2.data(), 256, 48000.f);
    // Po retriggeru hraje (nove) tlo + pripadny damping; aktivni aspon 1.
    CHECK(pool.activeCount() >= 1);
}
```

- [ ] **Step 2: Vytvor `engine/voice/voice.h`**

```cpp
#pragma once
// engine/voice/voice.h
// --------------------
// Jeden prehravany hlas. Hraje jeden SampleAsset pres jeho prvni mic
// perspektivu (mics[0]); multi-mic mix je faze 6. Pozici drzi v double kvuli
// pitch/SR konverzi (linearni interpolace). Envelope: onset ramp (anti-click)
// + release ramp. Pan z MIDI noty (keyboard spread). Damping buffer umoznuje
// click-free retrigger/kradez. Adaptovano z icr sampler_core.

#include "sample/sample_types.h"

#include <cstdint>

namespace ithaca {

// Konstanty hlasu.
constexpr float  kOnsetMs      = 3.f;     // nabeh proti lupnuti pri note-on
constexpr float  kDampingMs    = 21.f;    // crossfade pri retriggeru/kradezi
constexpr int    kDampMaxFrames = 2048;   // strop damping bufferu (frames)

class Voice {
public:
    // Spusti hlas. asset musi mit aspon 1 mic. pan_l/pan_r jiz spocteny.
    void start(const SampleAsset* asset, double pitch_ratio, float vel_gain,
               float pan_l, float pan_r, float engine_sr);

    // Pripravi damping crossfade ze SOUCASNEHO stavu (pred prepsanim novym
    // tonem) — zabrani lupnuti pri retriggeru/kradezi.
    void prepareDamp(float engine_sr);

    // Spusti release ramp (note-off).
    void release(float release_ms, float engine_sr);

    // Renderuj n_samples additivne do out_l/out_r. Vrati true kdyz stale aktivni.
    bool process(float* out_l, float* out_r, int n_samples) noexcept;

    bool  active()    const { return active_; }
    bool  releasing() const { return releasing_; }
    int   midi()      const { return midi_; }
    void  setMidi(int m)    { midi_ = m; }
    // Aktualni hlasitost (pro kradez nejtissiho hlasu).
    float currentLevel() const noexcept;

private:
    const SampleAsset* asset_ = nullptr;
    const MicLayer*    mic_   = nullptr;     // = &asset_->mics[0]

    bool   active_    = false;
    bool   releasing_ = false;
    bool   in_onset_  = false;
    int    midi_      = -1;

    double position_  = 0.0;                 // frame pozice (fractional)
    double pos_inc_   = 1.0;                 // pitch_ratio * (sample_sr/engine_sr)

    float  vel_gain_  = 1.f;
    float  onset_gain_ = 0.f, onset_step_ = 0.f;
    float  rel_gain_   = 1.f, rel_step_   = 0.f;
    float  pan_l_ = 0.707f, pan_r_ = 0.707f;

    // Damping crossfade buffer (interleaved stereo).
    float  damp_buf_[2 * kDampMaxFrames] = {};
    int    damp_len_ = 0, damp_pos_ = 0;
    bool   damping_  = false;
};

} // namespace ithaca
```

- [ ] **Step 3: Vytvor `engine/voice/voice.cpp`**

```cpp
// engine/voice/voice.cpp — viz voice.h. Adaptovano z icr sampler_core voice loop.
#include "voice/voice.h"

#include <algorithm>
#include <cmath>

namespace ithaca {

void Voice::prepareDamp(float engine_sr) {
    // Vytvor kratky fade-out ze soucasne pozice → damp_buf_, aby novy ton
    // (retrigger) nelupnul. Funguje jen kdyz hlas hraje a ma data.
    if (!active_ || !mic_) { damping_ = false; return; }
    int pos = (int)position_;
    if (pos >= mic_->frames) { damping_ = false; return; }
    int damp_frames = (std::min)((int)(kDampingMs * 0.001f * engine_sr), kDampMaxFrames);
    int avail = (std::min)(damp_frames, mic_->frames - pos);
    if (avail <= 0) { damping_ = false; return; }
    const float* src = mic_->data.data() + (size_t)pos * 2;
    float env = vel_gain_;
    if (releasing_) env *= rel_gain_;
    float step = 1.f / (float)avail;
    for (int i = 0; i < avail; ++i) {
        float fade = 1.f - (float)i * step;
        damp_buf_[i * 2]     = src[i * 2]     * env * fade * pan_l_;
        damp_buf_[i * 2 + 1] = src[i * 2 + 1] * env * fade * pan_r_;
    }
    damp_len_ = avail;
    damp_pos_ = 0;
    damping_  = true;
}

void Voice::start(const SampleAsset* asset, double pitch_ratio, float vel_gain,
                  float pan_l, float pan_r, float engine_sr) {
    asset_ = asset;
    mic_   = (asset && !asset->mics.empty()) ? &asset->mics[0] : nullptr;
    active_    = (mic_ != nullptr && mic_->frames > 0);
    releasing_ = false;
    in_onset_  = true;
    position_  = 0.0;
    double sample_sr = mic_ ? (double)mic_->sample_rate : (double)engine_sr;
    pos_inc_ = pitch_ratio * (sample_sr / (double)engine_sr);
    vel_gain_  = vel_gain;
    onset_gain_ = 0.f;
    onset_step_ = 1.f / (kOnsetMs * 0.001f * engine_sr);
    rel_gain_   = 1.f;
    rel_step_   = 0.f;
    pan_l_ = pan_l;
    pan_r_ = pan_r;
}

void Voice::release(float release_ms, float engine_sr) {
    if (!active_ || releasing_) return;
    releasing_ = true;
    rel_gain_  = in_onset_ ? onset_gain_ : 1.f;
    rel_step_  = -rel_gain_ / (release_ms * 0.001f * engine_sr);
}

float Voice::currentLevel() const noexcept {
    if (!active_) return 0.f;
    float env = vel_gain_;
    if (in_onset_)  env *= onset_gain_;
    if (releasing_) env *= rel_gain_;
    return env;
}

bool Voice::process(float* out_l, float* out_r, int n_samples) noexcept {
    if (!mic_ || mic_->frames <= 0) { active_ = false; return false; }
    const float* data = mic_->data.data();
    const int max_frames = mic_->frames;

    for (int i = 0; i < n_samples; ++i) {
        // Damping crossfade (zbytek predchoziho tonu pri retriggeru).
        if (damping_ && damp_pos_ < damp_len_) {
            out_l[i] += damp_buf_[damp_pos_ * 2];
            out_r[i] += damp_buf_[damp_pos_ * 2 + 1];
            damp_pos_++;
            if (damp_pos_ >= damp_len_) damping_ = false;
        }

        int p0 = (int)position_;
        if (p0 >= max_frames - 1) { active_ = false; break; }
        float frac = (float)(position_ - (double)p0);
        int p1 = p0 + 1;

        float sL = data[p0 * 2]     * (1.f - frac) + data[p1 * 2]     * frac;
        float sR = data[p0 * 2 + 1] * (1.f - frac) + data[p1 * 2 + 1] * frac;
        position_ += pos_inc_;

        // Onset ramp.
        float env = 1.f;
        if (in_onset_) {
            onset_gain_ += onset_step_;
            if (onset_gain_ >= 1.f) { onset_gain_ = 1.f; in_onset_ = false; }
            env = onset_gain_;
        }
        // Release ramp.
        if (releasing_) {
            env *= rel_gain_;
            rel_gain_ += rel_step_;
            if (rel_gain_ <= 0.f) { rel_gain_ = 0.f; active_ = false; }
        }

        float g = vel_gain_ * env;
        out_l[i] += sL * g * pan_l_;
        out_r[i] += sR * g * pan_r_;

        if (!active_) break;
    }
    // Damping muze dobehnout i kdyz uz tlo neni aktivni — hlas je "aktivni"
    // dokud bud hraje tlo, nebo dobiha damping.
    return active_ || damping_;
}

} // namespace ithaca
```

- [ ] **Step 4: Vytvor `engine/voice/voice_pool.h`**

```cpp
#pragma once
// engine/voice/voice_pool.h
// -------------------------
// Pool N hlasu (default 128). noteOn alokuje volny slot, nebo ukrade nejtissi
// (spec: nejtissi sampl z celeho poolu). Retrigger tehoz tonu nejdriv dampne
// stary hlas (click-free), pak spusti novy. noteOff spusti release vsech hlasu
// dane noty. processBlock renderuje vsechny aktivni hlasy additivne.

#include "voice/voice.h"
#include "voice/patch_manager.h"

#include <vector>

namespace ithaca {

constexpr int kDefaultPoolSize = 128;
constexpr int kMaxPoolSize     = 256;

class VoicePool {
public:
    explicit VoicePool(int pool_size = kDefaultPoolSize);

    // Spusti (nebo retriggeruje) ton. keyboard_spread ovlivnuje pan dle noty.
    void noteOn(int midi, const VoiceSpec& spec, float engine_sr,
                float keyboard_spread = 0.6f);
    // Release vsech hlasu dane noty.
    void noteOff(int midi, float release_ms, float engine_sr);
    // Release vsech hlasu (panic).
    void allNotesOff(float release_ms, float engine_sr);

    // Renderuj vsechny aktivni hlasy additivne. Vrati true kdyz neco znelo.
    bool processBlock(float* out_l, float* out_r, int n_samples,
                      float engine_sr) noexcept;

    int activeCount() const noexcept;
    int poolSize() const { return (int)voices_.size(); }

private:
    int findSlot();                          // volny, nebo nejtissi (kradez)

    std::vector<Voice> voices_;
};

} // namespace ithaca
```

- [ ] **Step 5: Vytvor `engine/voice/voice_pool.cpp`**

```cpp
// engine/voice/voice_pool.cpp — viz voice_pool.h.
#include "voice/voice_pool.h"

#include <algorithm>
#include <cmath>

namespace ithaca {

namespace {
// Pan z MIDI noty: stred + rozprostreni dle vzdalenosti od C4 (midi 60).
void panForNote(int midi, float spread, float& pan_l, float& pan_r) {
    constexpr float kPi = 3.14159265f;
    float angle = (kPi / 4.f) + ((float)midi - 64.5f) / 87.f * spread * 0.5f;
    pan_l = std::cos(angle);
    pan_r = std::sin(angle);
}
} // namespace

VoicePool::VoicePool(int pool_size) {
    int n = (std::max)(1, (std::min)(pool_size, kMaxPoolSize));
    voices_.resize((size_t)n);
}

int VoicePool::findSlot() {
    // 1. Volny slot.
    for (int i = 0; i < (int)voices_.size(); ++i)
        if (!voices_[i].active()) return i;
    // 2. Kradez: nejtissi hlas z celeho poolu (spec rozhodnuti).
    int best = 0;
    float best_level = voices_[0].currentLevel();
    for (int i = 1; i < (int)voices_.size(); ++i) {
        float lvl = voices_[i].currentLevel();
        if (lvl < best_level) { best_level = lvl; best = i; }
    }
    return best;
}

void VoicePool::noteOn(int midi, const VoiceSpec& spec, float engine_sr,
                       float keyboard_spread) {
    if (!spec.asset) return;

    // Retrigger: pokud uz nektery hlas hraje tuto notu, damp ho (click-free).
    for (auto& v : voices_)
        if (v.active() && v.midi() == midi && !v.releasing())
            v.prepareDamp(engine_sr);

    int slot = findSlot();
    Voice& v = voices_[slot];
    // Kdyz krademe aktivni hlas, damp i jeho (jiny ton) → bez lupnuti.
    if (v.active()) v.prepareDamp(engine_sr);

    float pl, pr;
    panForNote(midi, keyboard_spread, pl, pr);
    v.start(spec.asset, spec.pitch_ratio, spec.vel_gain, pl, pr, engine_sr);
    v.setMidi(midi);
}

void VoicePool::noteOff(int midi, float release_ms, float engine_sr) {
    for (auto& v : voices_)
        if (v.active() && v.midi() == midi && !v.releasing())
            v.release(release_ms, engine_sr);
}

void VoicePool::allNotesOff(float release_ms, float engine_sr) {
    for (auto& v : voices_)
        if (v.active() && !v.releasing())
            v.release(release_ms, engine_sr);
}

bool VoicePool::processBlock(float* out_l, float* out_r, int n_samples,
                             float engine_sr) noexcept {
    (void)engine_sr;
    bool any = false;
    for (auto& v : voices_) {
        if (!v.active()) continue;
        if (v.process(out_l, out_r, n_samples)) any = true;
    }
    return any;
}

int VoicePool::activeCount() const noexcept {
    int n = 0;
    for (const auto& v : voices_) if (v.active()) n++;
    return n;
}

} // namespace ithaca
```

POZN.: damping (retrigger crossfade) muze dobihat i u hlasu, ktery uz `process()` oznacil
`active_=false` (tlo dohralo) — protoze `process()` vraci `active_ || damping_`. activeCount() pocita
jen `active_` (tlo), takze test "retrigger → activeCount>=1" plati diky novemu hlasu. To je v poradku.

- [ ] **Step 6: CMake** — pridej `engine/voice/voice.cpp` a `engine/voice/voice_pool.cpp` do
`ithaca_core`; v tests:

```cmake
add_executable(test_voice_pool test_voice_pool.cpp)
target_link_libraries(test_voice_pool PRIVATE ithaca_core doctest)
add_test(NAME test_voice_pool COMMAND test_voice_pool)
```

- [ ] **Step 7: Build + test**

Run: `cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_voice_pool`
Expected: 4 cases projdou.

- [ ] **Step 8: Commit**

```bash
git add engine/voice/voice.h engine/voice/voice.cpp engine/voice/voice_pool.h engine/voice/voice_pool.cpp tests/test_voice_pool.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze3): voice + voice_pool — render hlasu, kradez nejtissiho, retrigger damp

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: lock-free MIDI fronta + engine fasada (TDD)

**Files:**
- Create: `engine/midi/midi_queue.h` (header-only), `engine/engine.h`, `engine/engine.cpp`
- Test: `tests/test_midi_queue.cpp`, `tests/test_engine.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

`midi_queue` = lock-free SPSC ring (producent = MIDI/GUI thread, konzument = audio thread), stejny
princip jako logger. `Engine` je fasada: drzi Bank + VoicePool + RoundRobinState + frontu; noteOn/
noteOff jen vlozi do fronty (thread-safe), `processBlock` frontu vyprazdni (na audio threadu) a
renderuje. Master gain post-mix.

- [ ] **Step 1: Napis selhavajici test `tests/test_midi_queue.cpp`**

```cpp
// tests/test_midi_queue.cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "midi/midi_queue.h"

using namespace ithaca;

TEST_CASE("MidiQueue push/pop zachova poradi (FIFO)") {
    MidiQueue q;
    CHECK(q.push({MidiEvent::NoteOn, 60, 100}));
    CHECK(q.push({MidiEvent::NoteOff, 60, 0}));
    MidiEvent e;
    REQUIRE(q.pop(e)); CHECK(e.type == MidiEvent::NoteOn);  CHECK(e.data1 == 60); CHECK(e.data2 == 100);
    REQUIRE(q.pop(e)); CHECK(e.type == MidiEvent::NoteOff); CHECK(e.data1 == 60);
    CHECK_FALSE(q.pop(e));                                  // prazdna
}

TEST_CASE("MidiQueue drop-on-full nepretece") {
    MidiQueue q;
    int pushed = 0;
    for (int i = 0; i < 10000; ++i) if (q.push({MidiEvent::NoteOn, 60, 100})) pushed++;
    // Kapacita je omezena (MIDI_Q_SIZE); push nad ni vrati false, nespadne.
    CHECK(pushed > 0);
    CHECK(pushed < 10000);
}
```

- [ ] **Step 2: Vytvor `engine/midi/midi_queue.h`**

```cpp
#pragma once
// engine/midi/midi_queue.h
// ------------------------
// Lock-free SPSC fronta MIDI udalosti: producent = MIDI/GUI thread, konzument
// = audio thread. Stejny princip jako RT logger (publish write indexu
// release/acquire, drop-on-full). Audio thread NIKDY neblokuje.

#include <atomic>
#include <cstdint>

namespace ithaca {

struct MidiEvent {
    enum Type : uint8_t { NoteOn, NoteOff, Sustain, AllNotesOff };
    Type    type  = NoteOn;
    uint8_t data1 = 0;     // midi nota (NoteOn/Off) nebo hodnota (Sustain)
    uint8_t data2 = 0;     // velocity (NoteOn)
};

class MidiQueue {
public:
    static constexpr int MIDI_Q_SIZE = 1024;

    // Producent (MIDI/GUI thread). Vrati false kdyz je fronta plna (drop).
    bool push(const MidiEvent& e) {
        const size_t w = w_.load(std::memory_order_relaxed);
        const size_t r = r_.load(std::memory_order_acquire);
        if (w - r >= MIDI_Q_SIZE) return false;          // plna → drop
        buf_[w % MIDI_Q_SIZE] = e;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Konzument (audio thread). Vrati false kdyz je fronta prazdna.
    bool pop(MidiEvent& out) {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t w = w_.load(std::memory_order_acquire);
        if (r >= w) return false;
        out = buf_[r % MIDI_Q_SIZE];
        r_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    MidiEvent           buf_[MIDI_Q_SIZE];
    std::atomic<size_t> w_{0};
    std::atomic<size_t> r_{0};
};

} // namespace ithaca
```

- [ ] **Step 3: Napis selhavajici test `tests/test_engine.cpp`**

```cpp
// tests/test_engine.cpp
// Engine fasada: nacti malou banku (fixture), noteOn pres frontu, processBlock
// vyrobi zvuk. Bez audio device — voláme processBlock primo.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace ithaca;

namespace {
void wU32(std::FILE* f, uint32_t v){ std::fwrite(&v,4,1,f);} 
void wU16(std::FILE* f, uint16_t v){ std::fwrite(&v,2,1,f);} 
void writeConstWav(const std::string& p, float amp, int sr=48000) {
    std::FILE* f=std::fopen(p.c_str(),"wb"); REQUIRE(f);
    int frames=sr/2; uint32_t ds=(uint32_t)frames*4u;
    std::fwrite("RIFF",1,4,f); wU32(f,36u+ds); std::fwrite("WAVE",1,4,f);
    std::fwrite("fmt ",1,4,f); wU32(f,16u); wU16(f,1); wU16(f,2);
    wU32(f,(uint32_t)sr); wU32(f,(uint32_t)sr*4u); wU16(f,4); wU16(f,16);
    std::fwrite("data",1,4,f); wU32(f,ds);
    int16_t v=(int16_t)std::lround(amp*32767.f);
    for(int i=0;i<frames;i++){std::fwrite(&v,2,1,f);std::fwrite(&v,2,1,f);} std::fclose(f);
}
double energy(const std::vector<float>& b){double s=0;for(float v:b)s+=std::fabs((double)v);return s;}
} // namespace

TEST_CASE("Engine: load bank, noteOn pres frontu, processBlock vyrobi zvuk") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_engine_fixture";
    fs::remove_all(dir); fs::create_directories(dir);
    writeConstWav(dir + "/m060-vel4-f48.wav", 0.5f);

    Engine eng;
    EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 256; cfg.max_voices = 32;
    REQUIRE(eng.init(cfg));
    REQUIRE(eng.loadBank(dir));
    fs::remove_all(dir);

    eng.noteOn(60, 100);                      // vlozi do fronty
    std::vector<float> L(256, 0.f), R(256, 0.f);
    eng.processBlock(L.data(), R.data(), 256); // drainuje frontu + renderuje
    CHECK(energy(L) > 0.0);
}

TEST_CASE("Engine: prazdna banka → processBlock je ticho, ne crash") {
    Engine eng;
    EngineConfig cfg;
    REQUIRE(eng.init(cfg));
    eng.noteOn(60, 100);
    std::vector<float> L(256, 0.f), R(256, 0.f);
    eng.processBlock(L.data(), R.data(), 256);
    CHECK(energy(L) == doctest::Approx(0.0));
}
```

- [ ] **Step 4: Vytvor `engine/engine.h`**

```cpp
#pragma once
// engine/engine.h
// ---------------
// Fasada celeho prehravaciho enginu (headless). Drzi banku, voice pool a
// lock-free MIDI frontu. noteOn/noteOff jsou thread-safe (jen vlozi do fronty);
// processBlock bezi na audio threadu — vyprazdni frontu a renderuje. Master
// gain post-mix. Streaming/DSP/rezonance jsou dalsi faze.

#include "sample/sample_types.h"
#include "voice/voice_pool.h"
#include "voice/patch_manager.h"
#include "midi/midi_queue.h"

#include <atomic>
#include <memory>
#include <string>

namespace ithaca {

struct EngineConfig {
    int   sample_rate    = 48000;
    int   block_size     = 256;
    int   max_voices     = 128;
    float master_gain    = 1.0f;     // linearni
    float release_ms     = 200.f;
    float keyboard_spread = 0.6f;
    int   midi_from      = 0;        // rozsah nacitane banky (rychle testy)
    int   midi_to        = 127;
};

class Engine {
public:
    Engine();
    ~Engine();

    bool init(const EngineConfig& cfg);
    // Nacti legacy banku do RAM (respektuje cfg.midi_from/to). Vrati false kdyz nic.
    bool loadBank(const std::string& dir);

    // -- Thread-safe MIDI vstup (volat z MIDI/GUI threadu) --
    void noteOn(int midi, int velocity);
    void noteOff(int midi);
    void allNotesOff();

    // -- Audio thread -- renderuj n_samples do interleaved-by-caller L/R bufferu.
    // Caller buffery nuluje. Drainuje MIDI frontu pred renderem.
    void processBlock(float* out_l, float* out_r, int n_samples) noexcept;

    int  sampleRate() const { return cfg_.sample_rate; }
    int  blockSize()  const { return cfg_.block_size; }
    int  activeVoices() const { return pool_ ? pool_->activeCount() : 0; }
    void setMasterGain(float g) { master_gain_.store(g, std::memory_order_relaxed); }

private:
    EngineConfig                cfg_;
    Bank                        bank_;
    std::unique_ptr<VoicePool>  pool_;
    RoundRobinState             rr_;
    MidiQueue                   midi_q_;
    std::atomic<float>          master_gain_{1.0f};
    bool                        initialized_ = false;
};

} // namespace ithaca
```

- [ ] **Step 5: Vytvor `engine/engine.cpp`**

```cpp
// engine/engine.cpp — viz engine.h.
#include "engine.h"

#include "sample/sample_store.h"
#include "util/log.h"

#include <cmath>

namespace ithaca {

Engine::Engine() {}
Engine::~Engine() {}

bool Engine::init(const EngineConfig& cfg) {
    cfg_ = cfg;
    pool_ = std::make_unique<VoicePool>(cfg.max_voices);
    master_gain_.store(cfg.master_gain, std::memory_order_relaxed);
    initialized_ = true;
    return true;
}

bool Engine::loadBank(const std::string& dir) {
    auto& L = log::Logger::default_();
    bank_ = loadLegacyBank(dir, L, /*cache_budget_mb=*/0, cfg_.midi_from, cfg_.midi_to);
    return bank_.loaded_samples > 0;
}

void Engine::noteOn(int midi, int velocity) {
    if (velocity <= 0) { noteOff(midi); return; }
    midi_q_.push({MidiEvent::NoteOn, (uint8_t)midi, (uint8_t)velocity});
}
void Engine::noteOff(int midi) {
    midi_q_.push({MidiEvent::NoteOff, (uint8_t)midi, 0});
}
void Engine::allNotesOff() {
    midi_q_.push({MidiEvent::AllNotesOff, 0, 0});
}

void Engine::processBlock(float* out_l, float* out_r, int n_samples) noexcept {
    if (!initialized_ || !pool_) return;
    const float sr = (float)cfg_.sample_rate;

    // 1. Vyprazdni MIDI frontu (audio thread) → akce do voice poolu.
    MidiEvent e;
    while (midi_q_.pop(e)) {
        switch (e.type) {
            case MidiEvent::NoteOn: {
                VoiceSpec vs = selectVoice(bank_, e.data1, e.data2, rr_);
                if (vs.asset)
                    pool_->noteOn(e.data1, vs, sr, cfg_.keyboard_spread);
                break;
            }
            case MidiEvent::NoteOff:
                pool_->noteOff(e.data1, cfg_.release_ms, sr);
                break;
            case MidiEvent::AllNotesOff:
                pool_->allNotesOff(cfg_.release_ms, sr);
                break;
            case MidiEvent::Sustain:
                break;   // pedal je faze 5
        }
    }

    // 2. Render hlasu (caller buffery vynuloval).
    pool_->processBlock(out_l, out_r, n_samples, sr);

    // 3. Master gain post-mix.
    float g = master_gain_.load(std::memory_order_relaxed);
    if (std::fabs(g - 1.f) > 0.001f)
        for (int i = 0; i < n_samples; ++i) { out_l[i] *= g; out_r[i] *= g; }
}

} // namespace ithaca
```

- [ ] **Step 6: CMake** — pridej `engine/engine.cpp` do `ithaca_core` (midi_queue.h je header-only,
netreba registrovat). V tests pridej `test_midi_queue` a `test_engine`:

```cmake
add_executable(test_midi_queue test_midi_queue.cpp)
target_link_libraries(test_midi_queue PRIVATE ithaca_core doctest)
add_test(NAME test_midi_queue COMMAND test_midi_queue)

add_executable(test_engine test_engine.cpp)
target_link_libraries(test_engine PRIVATE ithaca_core doctest)
add_test(NAME test_engine COMMAND test_engine)
```

- [ ] **Step 7: Build + test**

Run: `cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R "test_midi_queue|test_engine"`
Expected: oba projdou.

- [ ] **Step 8: Commit**

```bash
git add engine/midi/midi_queue.h engine/engine.h engine/engine.cpp tests/test_midi_queue.cpp tests/test_engine.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze3): lock-free MIDI fronta + engine fasada (init/loadBank/processBlock)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: batch renderer + CLI --render (integracni test)

**Files:**
- Create: `engine/render/batch_renderer.h`, `engine/render/batch_renderer.cpp`
- Test: `tests/test_batch_renderer.cpp`
- Modify: `app/cli/main.cpp` (prikaz `--render`), `CMakeLists.txt`, `tests/CMakeLists.txt`

Offline render: vezme engine + seznam not, kazdou zahraje (note-on, drz N sekund, note-off, dozvuk)
do jednoho stereo WAV. Bez audio device → testovatelne i v CI a bez HW. To je novy `make smoke`.

- [ ] **Step 1: Napis selhavajici test `tests/test_batch_renderer.cpp`**

```cpp
// tests/test_batch_renderer.cpp
// Render par not z fixture banky do WAV, over ze WAV je nenulovy.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "render/batch_renderer.h"
#include "engine.h"
#include "io/wav_reader.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace ithaca;

namespace {
void wU32(std::FILE* f, uint32_t v){ std::fwrite(&v,4,1,f);} 
void wU16(std::FILE* f, uint16_t v){ std::fwrite(&v,2,1,f);} 
void writeConstWav(const std::string& p, float amp, int sr=48000) {
    std::FILE* f=std::fopen(p.c_str(),"wb"); REQUIRE(f);
    int frames=sr; uint32_t ds=(uint32_t)frames*4u;       // 1 s
    std::fwrite("RIFF",1,4,f); wU32(f,36u+ds); std::fwrite("WAVE",1,4,f);
    std::fwrite("fmt ",1,4,f); wU32(f,16u); wU16(f,1); wU16(f,2);
    wU32(f,(uint32_t)sr); wU32(f,(uint32_t)sr*4u); wU16(f,4); wU16(f,16);
    std::fwrite("data",1,4,f); wU32(f,ds);
    int16_t v=(int16_t)std::lround(amp*32767.f);
    for(int i=0;i<frames;i++){std::fwrite(&v,2,1,f);std::fwrite(&v,2,1,f);} std::fclose(f);
}
} // namespace

TEST_CASE("renderNotes vyrobi nenulovy WAV z fixture banky") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_render_fixture";
    fs::remove_all(dir); fs::create_directories(dir);
    writeConstWav(dir + "/m060-vel4-f48.wav", 0.5f);

    Engine eng;
    EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 256;
    REQUIRE(eng.init(cfg));
    REQUIRE(eng.loadBank(dir));

    std::string out = "/tmp/ithaca_render_out.wav";
    std::vector<BatchNote> notes = { {60, 100, 0.2f} };   // 1 nota, 0.2 s
    int rendered = renderNotes(eng, notes, out, /*tail_s=*/0.1f);
    fs::remove_all(dir);

    CHECK(rendered == 1);
    WavData w = readWav(out);
    std::remove(out.c_str());
    REQUIRE(w.valid);
    CHECK(w.frames > 0);
    double e = 0; for (float s : w.samples) e += std::fabs((double)s);
    CHECK(e > 0.0);                                        // neco zaznelo
}
```

- [ ] **Step 2: Vytvor `engine/render/batch_renderer.h`**

```cpp
#pragma once
// engine/render/batch_renderer.h
// ------------------------------
// Offline render not do jednoho stereo WAV bez audio device. Pro kazdou notu:
// note-on, drz duration_s, note-off, nech dozvuk tail_s. Pouziva se pro
// testovani prehravani bez HW a jako `make smoke`.

#include <string>
#include <vector>

namespace ithaca {

class Engine;

struct BatchNote {
    int   midi;
    int   velocity;
    float duration_s;
};

// Renderuje noty sekvencne do jednoho WAV (sum vsech, jedna za druhou).
// Vrati pocet uspesne odehranych not (s nenulovym vystupem se nepocita zvlast
// — vraci pocet zpracovanych not). tail_s = dozvuk po note-off.
int renderNotes(Engine& engine, const std::vector<BatchNote>& notes,
                const std::string& out_path, float tail_s = 0.5f);

} // namespace ithaca
```

- [ ] **Step 3: Vytvor `engine/render/batch_renderer.cpp`**

```cpp
// engine/render/batch_renderer.cpp — viz batch_renderer.h.
#include "render/batch_renderer.h"

#include "engine.h"
#include "io/wav_writer.h"
#include "util/log.h"

#include <algorithm>
#include <vector>

namespace ithaca {

int renderNotes(Engine& engine, const std::vector<BatchNote>& notes,
                const std::string& out_path, float tail_s) {
    const int   sr    = engine.sampleRate();
    const int   block = 512;
    std::vector<float> out;                              // interleaved stereo akumulator
    std::vector<float> bl(block), br(block);

    int done = 0;
    for (const auto& n : notes) {
        int sustain_frames = (int)(n.duration_s * (float)sr);
        int tail_frames    = (int)(tail_s * (float)sr);

        engine.allNotesOff();
        engine.noteOn(n.midi, n.velocity);

        // Sustain faze.
        for (int s = 0; s < sustain_frames; ) {
            int k = (std::min)(block, sustain_frames - s);
            std::fill(bl.begin(), bl.begin() + k, 0.f);
            std::fill(br.begin(), br.begin() + k, 0.f);
            engine.processBlock(bl.data(), br.data(), k);
            for (int i = 0; i < k; ++i) { out.push_back(bl[i]); out.push_back(br[i]); }
            s += k;
        }
        engine.noteOff(n.midi);
        // Tail (dozvuk).
        for (int s = 0; s < tail_frames; ) {
            int k = (std::min)(block, tail_frames - s);
            std::fill(bl.begin(), bl.begin() + k, 0.f);
            std::fill(br.begin(), br.begin() + k, 0.f);
            engine.processBlock(bl.data(), br.data(), k);
            for (int i = 0; i < k; ++i) { out.push_back(bl[i]); out.push_back(br[i]); }
            s += k;
        }
        done++;
    }

    auto& L = log::Logger::default_();
    if (!writeWavStereo16(out_path, out, sr)) {
        L.log("render", log::Severity::Error, "Nelze zapsat WAV: %s", out_path.c_str());
        return 0;
    }
    L.log("render", log::Severity::Info, "Render: %d not → %s (%zu frames)",
          done, out_path.c_str(), out.size() / 2);
    return done;
}

} // namespace ithaca
```

- [ ] **Step 4: Rozsir `app/cli/main.cpp` o `--render`**

Pridej includes `#include "engine.h"` a `#include "render/batch_renderer.h"`. Pridej lokalni
promenne `std::string render_dir, render_out;`. Pridej parsovani:

```cpp
        } else if (a == "--render" && i + 1 < argc) {
            render_dir = argv[++i];
        } else if (a == "--out" && i + 1 < argc) {
            render_out = argv[++i];
```

Do usage textu pridej:
```cpp
        "  %s --render <dir> --out <wav> [--log-level <lvl>]\n"
```
(a odpovidajici radek ve volbach: `"  --render <dir>       nacti banku a renderuj test noty do --out WAV\n"`,
`"  --out <wav>          vystupni WAV pro --render\n"`) a pridej argv0 do printf seznamu.

Pridej blok (po nastaveni loggeru, vedle --inspect):

```cpp
    if (!render_dir.empty()) {
        if (render_out.empty()) {
            LOG_ERROR("render", "--render vyzaduje --out <wav>");
            return 1;
        }
        Engine eng;
        EngineConfig cfg;
        // Pro rychly render nacti jen par not kolem stredu klaviatury.
        cfg.midi_from = 57; cfg.midi_to = 72;
        if (!eng.init(cfg) || !eng.loadBank(render_dir)) {
            LOG_ERROR("render", "Nelze nacist banku: %s", render_dir.c_str());
            return 1;
        }
        std::vector<BatchNote> notes = {
            {60, 100, 1.0f}, {64, 100, 1.0f}, {67, 100, 1.0f},   // C-E-G akord po sobe
        };
        int n = renderNotes(eng, notes, render_out, /*tail_s=*/0.5f);
        LOG_INFO("render", "Hotovo: %d not → %s", n, render_out.c_str());
        return n > 0 ? 0 : 1;
    }
```

Pridej `#include <vector>` k includes main.cpp.

- [ ] **Step 5: CMake** — pridej `engine/render/batch_renderer.cpp` do `ithaca_core`; v tests:

```cmake
add_executable(test_batch_renderer test_batch_renderer.cpp)
target_link_libraries(test_batch_renderer PRIVATE ithaca_core doctest)
add_test(NAME test_batch_renderer COMMAND test_batch_renderer)
```

- [ ] **Step 6: Build + test + render na realne bance**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_batch_renderer
./build/ithaca-cli --render /Users/j/SoundBanks/Ithaca/as-blackgrand --out /tmp/ithaca_render_real.wav
```
Expected: test projde; render vyrobi `/tmp/ithaca_render_real.wav` (exit 0, log "Hotovo: 3 not").
Volitelne si ho prehraj (`afplay /tmp/ithaca_render_real.wav` na macOS) — mel by znit akord C-E-G.

- [ ] **Step 7: Commit**

```bash
git add engine/render/batch_renderer.h engine/render/batch_renderer.cpp tests/test_batch_renderer.cpp app/cli/main.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze3): batch renderer + CLI --render (offline WAV bez audio HW)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: audio_device (miniaudio) + CLI --play (ziva hra, manualni overeni)

**Files:**
- Create: `engine/miniaudio_impl.cpp`, `engine/io/audio_device.h`, `engine/io/audio_device.cpp`
- Modify: `app/cli/main.cpp` (prikaz `--play`), `CMakeLists.txt`

Ziva audio device pres miniaudio. NELZE unit-testovat (potrebuje HW + realny cas), proto se overuje
MANUALNE: `--play` otevre device a hraje testovaci sekvenci (nebo drzi tichy a ceka). `make smoke`
zustava batch render z Tasku 5 (deterministicke).

- [ ] **Step 1: Vytvor `engine/miniaudio_impl.cpp`**

```cpp
// engine/miniaudio_impl.cpp
// -------------------------
// Jediny translation unit, ktery kompiluje implementaci miniaudio. Vsude jinde
// se miniaudio.h includuje jen jako hlavicka.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
```

- [ ] **Step 2: Vytvor `engine/io/audio_device.h`**

```cpp
#pragma once
// engine/io/audio_device.h
// ------------------------
// miniaudio playback wrapper. Forward-declaruje ma_device (zadna miniaudio
// dependency v hlavicce). Caller dodava callback, ktery plni interleaved
// stereo float32 buffer. Adaptovano z icr2 player/audio_device.

#include <atomic>
#include <cstdint>
#include <string>

struct ma_device;

namespace ithaca {

// Callback: naplni `output` (interleaved stereo float32) `frames` framy.
using AudioCallback = void(*)(void* userdata, float* output, uint32_t frames);

class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Init + start. Vrati false on failure.
    bool start(AudioCallback cb, void* userdata, int sample_rate, int block_size);
    void stop();

    bool isRunning() const { return running_.load(); }
    const std::string& deviceName() const { return device_name_; }

    // Interni: vola ulozeny callback. Verejne jen kvuli C-trampoline v .cpp
    // (miniaudio callback je free funkce a potrebuje se dostat k callback_).
    void invokeCallback(float* output, uint32_t frames) {
        if (callback_) callback_(userdata_, output, frames);
    }

private:
    ma_device*        device_   = nullptr;
    std::atomic<bool> running_  {false};
    AudioCallback     callback_ = nullptr;
    void*             userdata_ = nullptr;
    std::string       device_name_;
};

} // namespace ithaca
```

- [ ] **Step 3: Vytvor `engine/io/audio_device.cpp`**

```cpp
// engine/io/audio_device.cpp — viz audio_device.h.
#include "io/audio_device.h"

#include "miniaudio.h"            // jen hlavicka; impl je v miniaudio_impl.cpp
#include "util/log.h"

namespace ithaca {

// Trampolina: miniaudio ma_device callback → nas AudioCallback. miniaudio drzi
// pUserData = AudioDevice*, takze se pres nej dostaneme k ulozenemu callbacku.
// output je interleaved stereo float32 (device je nize nakonfigurovan na
// format f32, channels 2), takze pretypovani na float* je bezpecne.
static void ma_data_callback(ma_device* dev, void* output,
                             const void* /*input*/, ma_uint32 frame_count) {
    auto* self = static_cast<AudioDevice*>(dev->pUserData);
    if (self) self->invokeCallback(static_cast<float*>(output),
                                   (uint32_t)frame_count);
}

AudioDevice::AudioDevice() {}
AudioDevice::~AudioDevice() { stop(); }

bool AudioDevice::start(AudioCallback cb, void* userdata,
                        int sample_rate, int block_size) {
    callback_ = cb;
    userdata_ = userdata;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate        = (ma_uint32)sample_rate;
    config.periodSizeInFrames = (ma_uint32)block_size;
    config.dataCallback      = ma_data_callback;
    config.pUserData         = this;

    device_ = new ma_device();
    if (ma_device_init(nullptr, &config, device_) != MA_SUCCESS) {
        delete device_; device_ = nullptr;
        log::Logger::default_().log("audio", log::Severity::Error,
                                    "ma_device_init selhalo");
        return false;
    }
    device_name_ = device_->playback.name;
    if (ma_device_start(device_) != MA_SUCCESS) {
        ma_device_uninit(device_); delete device_; device_ = nullptr;
        log::Logger::default_().log("audio", log::Severity::Error,
                                    "ma_device_start selhalo");
        return false;
    }
    running_.store(true);
    log::Logger::default_().log("audio", log::Severity::Info,
                                "Audio start: %s SR=%d block=%d",
                                device_name_.c_str(), sample_rate, block_size);
    return true;
}

void AudioDevice::stop() {
    if (device_) {
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
    }
    running_.store(false);
}

} // namespace ithaca
```

- [ ] **Step 4: Rozsir `app/cli/main.cpp` o `--play`**

Pridej `#include "io/audio_device.h"`. Pridej promennou `std::string play_dir;` a parsovani
`--play <dir>`. Audio callback potrebuje pristup k Engine; uloz ho do male struktury:

```cpp
// (nad main, file-scope)
namespace {
struct PlayCtx { ithaca::Engine* eng; };
void playAudioCb(void* userdata, float* output, uint32_t frames) {
    auto* ctx = static_cast<PlayCtx*>(userdata);
    // Rozdel interleaved výstup na docasne L/R? Jednodussi: engine renderuje
    // do docasnych L/R bufferu a pak interleave. Pro jednoduchost pouzij
    // staticky scratch (audio thread, jeden konzument).
    static std::vector<float> L, R;
    if ((int)L.size() < (int)frames) { L.resize(frames); R.resize(frames); }
    std::fill(L.begin(), L.begin() + frames, 0.f);
    std::fill(R.begin(), R.begin() + frames, 0.f);
    ctx->eng->processBlock(L.data(), R.data(), (int)frames);
    for (uint32_t i = 0; i < frames; ++i) { output[i*2] = L[i]; output[i*2+1] = R[i]; }
}
} // namespace
```

Blok pro `--play` (po nastaveni loggeru):

```cpp
    if (!play_dir.empty()) {
        Engine eng;
        EngineConfig cfg;
        cfg.midi_from = 21; cfg.midi_to = 108;   // cela klaviatura (POZOR: velka RAM!)
        if (!eng.init(cfg) || !eng.loadBank(play_dir)) {
            LOG_ERROR("play", "Nelze nacist banku: %s", play_dir.c_str());
            return 1;
        }
        PlayCtx ctx{&eng};
        AudioDevice dev;
        if (!dev.start(playAudioCb, &ctx, cfg.sample_rate, cfg.block_size)) {
            LOG_ERROR("play", "Nelze otevrit audio device");
            return 1;
        }
        LOG_INFO("play", "Hraje. Spoustim testovaci akord C-E-G, pak Enter pro konec.");
        eng.noteOn(60, 100); eng.noteOn(64, 100); eng.noteOn(67, 100);
        std::getchar();                          // ceka na Enter
        dev.stop();
        return 0;
    }
```

Pridej `#include <cstdio>` (uz je) a `#include <vector>`, `#include <algorithm>` do main.cpp.
Do usage textu pridej radek pro `--play <dir>`. POZN.: `--play` na cele bance alokuje ~5.4 GB —
pro test si muzes zuzit rozsah v kodu, ale ponech cely jako default chovani pro skutecne hrani.

- [ ] **Step 5: CMake** — pridej do `ithaca_core` zdroje `engine/io/audio_device.cpp` a
`engine/miniaudio_impl.cpp`, a nalinkuj miniaudio include + (macOS) audio frameworky:

```cmake
# do add_library(ithaca_core STATIC ...) pridej:
#     engine/io/audio_device.cpp
#     engine/miniaudio_impl.cpp

# za definici ithaca_core:
target_link_libraries(ithaca_core PUBLIC miniaudio_inc)
if(APPLE)
    target_link_libraries(ithaca_core PUBLIC
        "-framework CoreAudio" "-framework AudioToolbox"
        "-framework CoreFoundation")
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(ithaca_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
```

(`miniaudio_inc` je INTERFACE target z `third-party/CMakeLists.txt` z faze 1.)

- [ ] **Step 6: Build + manualni overeni**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy && make build
```
Expected: cisty build vc. miniaudio_impl (muze chvili trvat — miniaudio je velky single-header).

Manualni test zvuku (vyzaduje audio HW, NENI v ctest):
```bash
./build/ithaca-cli --play /Users/j/SoundBanks/Ithaca/as-blackgrand
```
Expected: zazni akord C-E-G ze sampleru; Enter ukonci. (Pozn.: nacita celou banku ~5.4 GB —
chvili to potrva nez nabehne; streaming ve fazi 4 to vyresi.)

- [ ] **Step 7: Over ze ctest + smoke porad prochazi**

Run: `cd /Users/j/Projects/ithaca-legacy && make test && make smoke`
Expected: vsechny ctest targety projdou; smoke (batch render) OK.

POZN.: `make smoke` akt. spousti `ithaca-cli --selftest`. V tomto tasku NEMEN smoke (zmena smoke
na render je samostatny maly krok niz).

- [ ] **Step 8: Commit**

```bash
git add engine/miniaudio_impl.cpp engine/io/audio_device.h engine/io/audio_device.cpp app/cli/main.cpp CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze3): audio_device (miniaudio) + CLI --play (ziva hra, manualni overeni)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: prepni `make smoke` na batch render

**Files:**
- Modify: `Makefile` (cil `smoke`)

Smoke uz nebude jen selftest loggeru, ale realny render → WAV. Pouzije malou fixture banku
vytvorenou za behu (aby smoke nezavisel na /Users/j/SoundBanks), nebo realnou banku kdyz existuje.

- [ ] **Step 1: Uprav cil `smoke` v `Makefile`**

Nahrad telo `smoke` tak, aby vytvorilo docasnou fixture banku a renderovalo ji. Pouzij maly
shell helper primo v recipe (TAB odsazeni!):

```makefile
.PHONY: smoke
smoke: build
	@printf "Smoke test — batch render do test-samples/smoke.wav\n"
	@mkdir -p test-samples/_smoke_bank test-samples
	@# Vyrob 1 fixture WAV (1s stereo 48k konst. amplituda) pres maly python helper.
	@python3 -c "import wave,struct; \
f=wave.open('test-samples/_smoke_bank/m060-vel4-f48.wav','wb'); \
f.setnchannels(2); f.setsampwidth(2); f.setframerate(48000); \
f.writeframes(b''.join(struct.pack('<hh',8000,8000) for _ in range(48000))); f.close()"
	@if [ -x $(BUILD_DIR)/ithaca-cli ]; then \
	    $(BUILD_DIR)/ithaca-cli --render test-samples/_smoke_bank --out test-samples/smoke.wav; \
	else \
	    printf "ithaca-cli nenalezen\n" && exit 1; \
	fi
	@rm -rf test-samples/_smoke_bank
	@printf "Smoke OK — test-samples/smoke.wav\n"
```

POZN.: `--render` ma v main.cpp napevno rozsah midi 57..72 a hraje noty 60/64/67 — fixture nota 60
do toho spada, takze render bude nenulovy. (Pokud python3 neni k dispozici, smoke selze s jasnou
chybou — to je OK, python je bezny; alternativne by sel C++ generator, ale drzime smoke jednoduchy.)

- [ ] **Step 2: Over smoke**

Run: `cd /Users/j/Projects/ithaca-legacy && make smoke`
Expected: vypise "Smoke OK", vznikne `test-samples/smoke.wav`. (test-samples/ je gitignored.)

- [ ] **Step 3: Commit**

```bash
git add Makefile
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
build(faze3): make smoke = batch render misto selftest loggeru

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Hotovo — kriteria dokonceni faze 3

- `make build` cisty na macOS; `make test` — vsechny doctest targety projdou (faze 1-2 + nove:
  wav_writer, patch_manager, voice_pool, midi_queue, engine, batch_renderer).
- `ithaca-cli --render <bankdir> --out x.wav` vyrobi slysitelny WAV (akord C-E-G) z legacy banky.
- `ithaca-cli --play <bankdir>` zive prehraje akord pres audio device (manualni overeni na macOS).
- `make smoke` = deterministicky batch render fixture banky → WAV.
- Engine fasada: thread-safe noteOn/noteOff pres lock-free frontu; processBlock drainuje + renderuje;
  voice pool s kradezi nejtissiho hlasu a click-free retriggerem; pitch-shift chybejicich not.

Tim hraje nastroj z RAM. Faze 4 doplni streaming z disku (preload + ring buffery + stream thread +
underrun fade), cimz odpadne nutnost drzet celou (vicegb) banku v RAM.
