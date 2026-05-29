# Faze 2 — WAV reader + bank_index + sample_loader + sample_store — implementacni plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (doporuceno) nebo superpowers:executing-plans. Kroky pouzivaji checkbox (`- [ ]`) syntaxi.
>
> Jazyk: komentare/docs/commit messages cesky bez diakritiky; identifikatory anglicky;
> komentovat spise vice (explicit > implicit).

**Goal:** Nacist LEGACY banku z disku do RAM: projit adresar, rozparsovat nazvy, zmerit peak RMS
+ hranici attack/sustain kazdeho samplu, postavit datovy model NoteMap[128] →
VelocitySlot → SampleAsset → MicLayer a zpristupnit ho. Overitelne pres `ithaca-cli --inspect <bankdir>`.

**Architecture:** Ctyri nove moduly v `libithaca_core`: `io/wav_reader` (cte WAV → interleaved
stereo float), `sample/bank_index` (scan + parsing nazvu + detekce formatu), `sample/sample_loader`
(analyza: peak RMS, attack/sustain hranice), `sample/sample_store` (datovy model + nacteni cele
banky do RAM + sledovani RAM rozpoctu). Vse je all-in-RAM — streaming a preload/disk rozdeleni
prijde az ve fazi 4. Format extended se zatim jen DETEKUJE (build extended banky je faze 7);
faze 2 plne resi jen legacy.

**Tech Stack:** C++20, navazuje na fazi 1 (logger, CMake, Makefile, doctest). Zadne nove vendored
deps — WAV reader je vlastni (port z icr `engine/cores/sampler/wav_loader.h`). Build/verifikace macOS.

---

## Kontext a zdrojove vzory

- icr WAV loader (port predlohy): `/Users/j/Projects/icr/engine/cores/sampler/wav_loader.h`
  (header-only, chunk-walking, 16/24/32-bit PCM + IEEE float, mono/stereo → interleaved stereo float).
- icr sampler bank discovery (predloha pro bank_index): `/Users/j/Projects/icr/engine/cores/sampler/sampler_core.cpp`
  funkce `discoverBanks` / `loadBank` (regex parsing, vel layers).
- Spec: `/Users/j/Projects/ithaca-legacy/docs/superpowers/specs/2026-05-29-ithaca-legacy-design.md`
  (sekce 2.1 formaty, 2.2 RMS+sloty, 3.2 datovy model).
- Logger z faze 1: `engine/util/log.h` — pouzivej `LOG_INFO/LOG_WARN/LOG_ERROR("component", ...)`.

### Realna legacy banka (pro --inspect a integracni test, NEcommituje se)

`/Users/j/SoundBanks/Ithaca/as-blackgrand/` — 704 souboru = 88 not (MIDI 21-108, souvisle) × 8 vel
vrstev. Format `mNNN-velV-fSS.wav` (napr. `m021-vel0-f48.wav`). WAV: stereo, 16-bit PCM, 48 kHz.
Basy az ~30 s.

### Cilova struktura po fazi 2 (nove soubory)

```
engine/
  io/
    wav_reader.h        deklarace: WavData + readWav() + peekWavInfo()
    wav_reader.cpp      implementace (chunk-walking)
  sample/
    sample_types.h      datovy model: BankFormat, MicLayer, SampleAsset, VelocitySlot,
                        NoteSlots, Bank, ParsedName
    bank_index.h        scan adresare, parsing nazvu, detekce formatu
    bank_index.cpp
    sample_loader.h     analyza: measurePeakRmsDb(), findAttackEnd()
    sample_loader.cpp
    sample_store.h      loadLegacyBank() → Bank; RAM rozpocet
    sample_store.cpp
app/cli/main.cpp        + prikaz --inspect <bankdir>
tests/
  test_wav_reader.cpp   round-trip zapis/cti
  test_bank_index.cpp   parsing nazvu + detekce formatu
  test_sample_loader.cpp RMS + attack na syntetickych bufferech
  test_sample_store.cpp  stavba NoteMap z male fixture banky
```

### Konvence commitu (stejne jako faze 1)

`typ(faze2): popis` cesky bez diakritiky; `git add` konkretni soubory; trailer
`Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`; commit pres
`git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit`.

Pred zacatkem prace: vytvor a prepni se na vetev `faze2-loader` (`git checkout -b faze2-loader`
z `main`).

---

## Task 1: WAV reader (TDD)

**Files:**
- Create: `engine/io/wav_reader.h`, `engine/io/wav_reader.cpp`
- Test: `tests/test_wav_reader.cpp`
- Modify: `CMakeLists.txt` (pridat `engine/io/wav_reader.cpp` do `ithaca_core`),
  `tests/CMakeLists.txt` (pridat test_wav_reader)

Cte WAV → vzdy interleaved stereo float [-1,1]. Mono se zdvoji do L+R. Podporuje 16/24/32-bit PCM
a 32-bit IEEE float. `peekWavInfo()` precte jen hlavicku (frames/SR/channels) bez nacteni dat —
bude se hodit ve fazi 4 pro streaming, ale uz ted ho otestujeme.

- [ ] **Step 1: Napis selhavajici test `tests/test_wav_reader.cpp`**

```cpp
// tests/test_wav_reader.cpp
// Round-trip test WAV readeru: zapis znamy buffer jako WAV (test helper),
// precti ho zpet a over data, SR a frames. Testuje se i peekWavInfo.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "io/wav_reader.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ithaca;

// -- Test helper: zapis 16-bit PCM WAV (mono nebo stereo) do docasneho souboru.
// Nepatri do produkcniho kodu — jen pro testy (faze 2 nepotrebuje WAV writer).
namespace {

void writeU32(std::FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void writeU16(std::FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

// samples: interleaved podle channels; values v [-1,1]. Zapise 16-bit PCM.
std::string writeTempWav16(const std::vector<float>& samples, int channels,
                           int sample_rate, const char* tag) {
    std::string path = std::string("/tmp/ithaca_test_") + tag + ".wav";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    uint32_t n_samp   = (uint32_t)samples.size();
    uint32_t data_sz  = n_samp * 2u;                 // 2 bajty / sample (16-bit)
    uint32_t byte_rate = (uint32_t)sample_rate * (uint32_t)channels * 2u;
    std::fwrite("RIFF", 1, 4, f); writeU32(f, 36u + data_sz);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); writeU32(f, 16u);
    writeU16(f, 1);                                  // PCM
    writeU16(f, (uint16_t)channels);
    writeU32(f, (uint32_t)sample_rate);
    writeU32(f, byte_rate);
    writeU16(f, (uint16_t)(channels * 2));           // block align
    writeU16(f, 16);                                 // bits per sample
    std::fwrite("data", 1, 4, f); writeU32(f, data_sz);
    for (float s : samples) {
        if (s > 1.f) s = 1.f; if (s < -1.f) s = -1.f;
        int16_t v = (int16_t)std::lround(s * 32767.f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
    return path;
}

} // namespace

TEST_CASE("readWav nacte stereo 16-bit PCM a zachova hodnoty") {
    // 4 stereo frames: L = 0.5, R = -0.5
    std::vector<float> in = {0.5f, -0.5f, 0.5f, -0.5f, 0.5f, -0.5f, 0.5f, -0.5f};
    std::string p = writeTempWav16(in, /*channels=*/2, /*sr=*/48000, "stereo");
    WavData w = readWav(p);
    std::remove(p.c_str());
    REQUIRE(w.valid);
    CHECK(w.sample_rate == 48000);
    CHECK(w.frames == 4);
    REQUIRE(w.samples.size() == 8u);
    CHECK(w.samples[0] == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(w.samples[1] == doctest::Approx(-0.5f).epsilon(0.001));
}

TEST_CASE("readWav zdvoji mono do stereo (L==R)") {
    std::vector<float> in = {0.25f, 0.25f, 0.25f};  // 3 mono frames
    std::string p = writeTempWav16(in, /*channels=*/1, /*sr=*/44100, "mono");
    WavData w = readWav(p);
    std::remove(p.c_str());
    REQUIRE(w.valid);
    CHECK(w.sample_rate == 44100);
    CHECK(w.frames == 3);
    REQUIRE(w.samples.size() == 6u);                 // stereo interleaved
    CHECK(w.samples[0] == doctest::Approx(0.25f).epsilon(0.001));
    CHECK(w.samples[1] == doctest::Approx(0.25f).epsilon(0.001));  // L==R
}

TEST_CASE("readWav vrati invalid pro neexistujici soubor") {
    WavData w = readWav("/tmp/ithaca_neexistuje_xyz.wav");
    CHECK(w.valid == false);
}

TEST_CASE("peekWavInfo precte hlavicku bez nacteni dat") {
    std::vector<float> in(2 * 100, 0.1f);            // 100 stereo frames
    std::string p = writeTempWav16(in, /*channels=*/2, /*sr=*/96000, "peek");
    WavInfo info = peekWavInfo(p);
    std::remove(p.c_str());
    REQUIRE(info.valid);
    CHECK(info.sample_rate == 96000);
    CHECK(info.frames == 100);
    CHECK(info.channels == 2);
}
```

- [ ] **Step 2: Vytvor `engine/io/wav_reader.h`**

```cpp
#pragma once
// engine/io/wav_reader.h
// ----------------------
// Minimalni WAV reader. Vraci VZDY interleaved stereo float [-1,1] (mono se
// zdvoji do L+R). Podporuje 16/24/32-bit PCM a 32-bit IEEE float. Port
// predlohy z icr (cores/sampler/wav_loader.h), rozdeleny na .h/.cpp a
// doplneny o peekWavInfo() pro budouci streaming (faze 4).

#include <string>
#include <vector>

namespace ithaca {

// Plna data samplu v RAM.
struct WavData {
    std::vector<float> samples;   // interleaved stereo [L,R,L,R,...]
    int  frames      = 0;         // pocet stereo frames
    int  sample_rate = 0;
    bool valid       = false;
};

// Jen hlavickove informace (bez nacteni dat) — levne, pro rozhodovani o
// streamingu / rozpoctu pameti.
struct WavInfo {
    int  frames      = 0;
    int  sample_rate = 0;
    int  channels    = 0;         // puvodni pocet kanalu v souboru (1 nebo 2)
    bool valid       = false;
};

// Nacte cely WAV do RAM jako interleaved stereo float. Pri chybe vrati
// WavData s valid=false.
WavData readWav(const std::string& path);

// Precte jen fmt+data hlavicku. Pri chybe valid=false.
WavInfo peekWavInfo(const std::string& path);

} // namespace ithaca
```

- [ ] **Step 3: Vytvor `engine/io/wav_reader.cpp`**

```cpp
// engine/io/wav_reader.cpp — viz wav_reader.h.
#include "io/wav_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ithaca {

namespace {

// Spolecny vysledek parsovani fmt chunku.
struct FmtInfo {
    uint16_t audio_format = 0;   // 1 = PCM, 3 = IEEE float
    uint16_t channels     = 0;
    uint32_t sample_rate  = 0;
    uint16_t bits         = 0;
    bool     have         = false;
};

// Najde fmt chunk a (volitelne) pozici+velikost data chunku. Soubor je po
// navratu pozicovan na zacatek data chunku (kdyz found_data=true).
bool parseHeader(std::FILE* f, FmtInfo& fmt,
                 uint32_t& data_size, bool& found_data) {
    found_data = false;
    char riff[4];
    if (std::fread(riff, 1, 4, f) != 4 || std::strncmp(riff, "RIFF", 4) != 0)
        return false;
    std::fseek(f, 4, SEEK_CUR);  // preskoc chunk size
    char wave[4];
    if (std::fread(wave, 1, 4, f) != 4 || std::strncmp(wave, "WAVE", 4) != 0)
        return false;

    while (true) {
        char chunk_id[4];
        if (std::fread(chunk_id, 1, 4, f) != 4) break;
        uint32_t chunk_size = 0;
        if (std::fread(&chunk_size, 4, 1, f) != 1) break;

        if (std::strncmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t raw[8] = {0};
            uint32_t to_read = chunk_size < 16u ? chunk_size : 16u;
            std::fread(raw, 1, to_read, f);
            fmt.audio_format = raw[0];
            fmt.channels     = raw[1];
            std::memcpy(&fmt.sample_rate, &raw[2], 4);
            fmt.bits         = raw[7];
            fmt.have         = true;
            if (chunk_size > 16u)
                std::fseek(f, (long)(chunk_size - 16u), SEEK_CUR);
        } else if (std::strncmp(chunk_id, "data", 4) == 0) {
            data_size  = chunk_size;
            found_data = true;
            return fmt.have;   // soubor pozicovan na zacatku dat
        } else {
            // Preskoc neznamy chunk (word-aligned).
            std::fseek(f, (long)(chunk_size + (chunk_size & 1u)), SEEK_CUR);
        }
    }
    return false;
}

// Prevede jeden vzorek surovych bajtu na float [-1,1] podle formatu.
float sampleToFloat(const uint8_t* p, uint16_t bits, uint16_t audio_format) {
    if (audio_format == 3 && bits == 32) {          // IEEE float
        float v; std::memcpy(&v, p, 4); return v;
    }
    if (bits == 16) {
        int16_t v; std::memcpy(&v, p, 2);
        return (float)v / 32768.f;
    }
    if (bits == 24) {
        int32_t v = (p[0]) | (p[1] << 8) | (p[2] << 16);
        if (v & 0x800000) v |= ~0xFFFFFF;            // sign-extend
        return (float)v / 8388608.f;
    }
    if (bits == 32) {                                // 32-bit PCM
        int32_t v; std::memcpy(&v, p, 4);
        return (float)v / 2147483648.f;
    }
    return 0.f;
}

} // namespace

WavData readWav(const std::string& path) {
    WavData out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;

    FmtInfo fmt; uint32_t data_size = 0; bool found_data = false;
    if (!parseHeader(f, fmt, data_size, found_data) || !found_data) {
        std::fclose(f);
        return out;
    }

    const int bytes_per_sample = fmt.bits / 8;
    if (bytes_per_sample <= 0 || fmt.channels == 0) { std::fclose(f); return out; }

    std::vector<uint8_t> raw(data_size);
    size_t got = std::fread(raw.data(), 1, data_size, f);
    std::fclose(f);
    if (got < data_size) data_size = (uint32_t)got;   // tolerantni k oriznutemu souboru

    const int total_samples = (int)(data_size / (uint32_t)bytes_per_sample);
    const int frames = total_samples / fmt.channels;

    out.samples.resize((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        const uint8_t* base = raw.data() + (size_t)i * fmt.channels * bytes_per_sample;
        float L = sampleToFloat(base, fmt.bits, fmt.audio_format);
        float R = (fmt.channels >= 2)
                ? sampleToFloat(base + bytes_per_sample, fmt.bits, fmt.audio_format)
                : L;                                  // mono → zdvoj
        out.samples[(size_t)i * 2]     = L;
        out.samples[(size_t)i * 2 + 1] = R;
    }
    out.frames      = frames;
    out.sample_rate = (int)fmt.sample_rate;
    out.valid       = true;
    return out;
}

WavInfo peekWavInfo(const std::string& path) {
    WavInfo out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    FmtInfo fmt; uint32_t data_size = 0; bool found_data = false;
    bool ok = parseHeader(f, fmt, data_size, found_data);
    std::fclose(f);
    if (!ok || !found_data) return out;
    const int bytes_per_sample = fmt.bits / 8;
    if (bytes_per_sample <= 0 || fmt.channels == 0) return out;
    out.channels    = fmt.channels;
    out.sample_rate = (int)fmt.sample_rate;
    out.frames      = (int)(data_size / (uint32_t)(bytes_per_sample * fmt.channels));
    out.valid       = true;
    return out;
}

} // namespace ithaca
```

- [ ] **Step 4: Zaregistruj v CMake**

V root `CMakeLists.txt` pridej do `add_library(ithaca_core STATIC ...)` radek
`engine/io/wav_reader.cpp` (za `engine/util/log.cpp`). V `tests/CMakeLists.txt` pridej novy
test executable:

```cmake
add_executable(test_wav_reader test_wav_reader.cpp)
target_link_libraries(test_wav_reader PRIVATE ithaca_core doctest)
add_test(NAME test_wav_reader COMMAND test_wav_reader)
```

POZN.: faze 1 mela jediny test target `ithaca_tests` (test_log.cpp). Od faze 2 ma kazdy modul
vlastni test executable. Prejmenuj puvodni na konzistentni schema NENI nutne — nech `ithaca_tests`
(test_log) byt a pridavej `test_<modul>` vedle nej.

- [ ] **Step 5: Build + test (musi failovat, pak projit)**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_wav_reader
```
Expected: `test_wav_reader` projde (4 test cases). Pokud pred implementaci spustis a test target
neexistuje/nelinkuje, to je ocekavany "red" stav TDD.

- [ ] **Step 6: Commit**

```bash
git add engine/io/wav_reader.h engine/io/wav_reader.cpp tests/test_wav_reader.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze2): WAV reader (16/24/32-bit PCM + float, mono→stereo) + peekWavInfo

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Datovy model (sample_types.h)

**Files:**
- Create: `engine/sample/sample_types.h`

Cisty header s datovymi strukturami (zadny .cpp). Mapuje 1:1 na hierarchii ze spec sekce 3.2.
Faze 2: MicLayer drzi cely sampl v RAM (rozdeleni preload/disk je faze 4); legacy ma 1 mic
(stereo), 1 round-robin variantu na slot.

- [ ] **Step 1: Vytvor `engine/sample/sample_types.h`**

```cpp
#pragma once
// engine/sample/sample_types.h
// ----------------------------
// Datovy model banky. Hierarchie (spec 3.2):
//   Bank → NoteSlots[128] → VelocitySlot → SampleAsset → MicLayer
// Dve nezavisle osy: VelocitySlot drzi round-robin varianty, SampleAsset drzi
// mic perspektivy. Faze 2 je all-in-RAM (MicLayer.data = cely sampl); preload/
// disk rozdeleni a round-robin/multi-mic naplno prijdou v dalsich fazich.

#include <cstddef>
#include <string>
#include <vector>

namespace ithaca {

enum class BankFormat { Unknown, Legacy, Extended };

inline const char* bankFormatName(BankFormat f) {
    switch (f) {
        case BankFormat::Legacy:   return "legacy";
        case BankFormat::Extended: return "extended";
        case BankFormat::Unknown:  return "unknown";
    }
    return "unknown";
}

// Jedna mic perspektiva jednoho uhozu. Faze 2: cela data v RAM.
struct MicLayer {
    std::string        mic_name;     // legacy: "stereo"; extended: "front"/"soundboard"
    std::vector<float> data;         // interleaved stereo [L,R,...]
    int                frames = 0;
    int                sample_rate = 0;
};

// Jeden uhoz: 1..N mic perspektiv hranych synchronne. Legacy: 1 (stereo).
struct SampleAsset {
    std::vector<MicLayer> mics;
    float peak_rms_db     = 0.f;     // zmereno z referencni (prvni/front) perspektivy
    int   attack_end_frame = 0;      // hranice attack/sustain (pro resonance skip-attack)
};

// Jeden velocity slot: 1..N round-robin variant stejne dynamiky. Legacy: 1.
struct VelocitySlot {
    std::vector<SampleAsset> variants;
    float rms_db = 0.f;              // reprezentativni RMS slotu (pro velocity krivku)
};

// Vsechny sloty jedne MIDI noty, serazene VZESTUPNE podle rms_db (nejtissi → nejhlasitejsi).
struct NoteSlots {
    std::vector<VelocitySlot> slots;
    bool recorded = false;          // true = mame realny sampl pro tuto MIDI notu
};

// Cela banka v pameti.
struct Bank {
    std::string name;
    std::string path;
    BankFormat  format = BankFormat::Unknown;
    NoteSlots   notes[128];
    // diagnostika
    size_t total_frames = 0;        // soucet frames vsech nactenych mic layeru
    size_t total_bytes  = 0;        // odhad RAM (data.size()*sizeof(float))
    int    loaded_samples = 0;      // pocet uspesne nactenych SampleAssetu
};

} // namespace ithaca
```

- [ ] **Step 2: Commit** (cisty header, build se overi v dalsim tasku az ho nekdo includne)

```bash
git add engine/sample/sample_types.h
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze2): datovy model banky (Bank/NoteSlots/VelocitySlot/SampleAsset/MicLayer)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: bank_index — parsing nazvu + detekce formatu (TDD)

**Files:**
- Create: `engine/sample/bank_index.h`, `engine/sample/bank_index.cpp`
- Test: `tests/test_bank_index.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Parsing nazvu jsou ciste stringove funkce (snadno testovatelne bez I/O). `scanBank()` projde
adresar a vrati seznam parsovanych zaznamu + detekovany format. Faze 2 plne parsuje LEGACY;
extended nazev se rozpozna (detekce vrati Extended), ale jeho plne zpracovani je faze 7.

- [ ] **Step 1: Napis selhavajici test `tests/test_bank_index.cpp`**

```cpp
// tests/test_bank_index.cpp
// Testuje ciste parsovaci funkce nazvu a detekci formatu.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "sample/bank_index.h"

#include <string>

using namespace ithaca;

TEST_CASE("parseLegacyName rozparsuje mNNN-velV-fSS.wav") {
    ParsedName p = parseLegacyName("m060-vel3-f48.wav");
    REQUIRE(p.ok);
    CHECK(p.midi == 60);
    CHECK(p.vel == 3);
    CHECK(p.sr_tag == 48);
}

TEST_CASE("parseLegacyName akceptuje hranice (m021-vel0, m108-vel7)") {
    ParsedName a = parseLegacyName("m021-vel0-f48.wav");
    REQUIRE(a.ok); CHECK(a.midi == 21); CHECK(a.vel == 0);
    ParsedName b = parseLegacyName("m108-vel7-f96.wav");
    REQUIRE(b.ok); CHECK(b.midi == 108); CHECK(b.vel == 7); CHECK(b.sr_tag == 96);
}

TEST_CASE("parseLegacyName odmitne nelegacy nazvy") {
    CHECK_FALSE(parseLegacyName("m60-front-abc123.wav").ok);   // extended
    CHECK_FALSE(parseLegacyName("nahodny.wav").ok);
    CHECK_FALSE(parseLegacyName("m060-v003-f48.wav").ok);      // 16-vrstva varianta nepodporovana
}

TEST_CASE("detectFormatFromName rozpozna legacy vs extended") {
    CHECK(detectFormatFromName("m060-vel3-f48.wav") == BankFormat::Legacy);
    CHECK(detectFormatFromName("m60-front-abc123.wav") == BankFormat::Extended);
    CHECK(detectFormatFromName("m60-soundboard-abc123.wav") == BankFormat::Extended);
    CHECK(detectFormatFromName("readme.txt") == BankFormat::Unknown);
}
```

- [ ] **Step 2: Vytvor `engine/sample/bank_index.h`**

```cpp
#pragma once
// engine/sample/bank_index.h
// --------------------------
// Objevi obsah banky: projde adresar, rozparsuje nazvy souboru a detekuje
// format (legacy vs extended). Parsovaci funkce jsou ciste (bez I/O), aby
// sly snadno testovat. scanBank() dela adresarovy sken.

#include "sample/sample_types.h"

#include <string>
#include <vector>

namespace ithaca {

// Vysledek parsovani jednoho nazvu souboru.
struct ParsedName {
    bool        ok   = false;
    int         midi = -1;
    // legacy:
    int         vel    = -1;     // 0-7; -1 kdyz neni legacy
    int         sr_tag = 0;      // 48/44/96 z fSS
    // extended (faze 7 — zde jen naplnime, build neresi):
    std::string mic;             // "front"/"soundboard"
    std::string hash;            // parovaci klic (posledni token pred .wav)
    std::string filename;        // puvodni nazev (pro pozdejsi otevreni)
};

// Jeden nalezeny soubor: parsovana metadata + plna cesta.
struct BankFileEntry {
    ParsedName  parsed;
    std::string full_path;
};

// Vysledek skenu cele banky.
struct BankScan {
    BankFormat                 format = BankFormat::Unknown;
    std::vector<BankFileEntry> files;     // jen soubory odpovidajiciho formatu
    int                        skipped = 0;  // nerozpoznane soubory
};

// -- Ciste parsovaci funkce (testovatelne bez disku) --
ParsedName parseLegacyName(const std::string& filename);
ParsedName parseExtendedName(const std::string& filename);   // faze 7 — minimalni
BankFormat detectFormatFromName(const std::string& filename);

// -- Adresarovy sken --
// Projde `dir`, urci format (podle vetsiny rozpoznanych souboru), vrati
// parsovane zaznamy. Pri prazdnem/neexistujicim adresari je format Unknown.
BankScan scanBank(const std::string& dir);

} // namespace ithaca
```

- [ ] **Step 3: Vytvor `engine/sample/bank_index.cpp`**

```cpp
// engine/sample/bank_index.cpp — viz bank_index.h.
#include "sample/bank_index.h"

#include <filesystem>
#include <regex>

namespace ithaca {

namespace {
// Legacy: mNNN-velV-fSS.wav  (3 cislice noty, 1 cislice vel, N cislic SR).
const std::regex& legacyRe() {
    static const std::regex re(R"(m(\d{3})-vel(\d)-f(\d+)\.wav)",
                               std::regex::icase);
    return re;
}
// Extended: mNN-MIC-HASH.wav  (MIC = pismena, HASH = posledni token pred .wav).
const std::regex& extendedRe() {
    static const std::regex re(R"(m(\d+)-([a-z]+)-([A-Za-z0-9]+)\.wav)",
                               std::regex::icase);
    return re;
}
} // namespace

ParsedName parseLegacyName(const std::string& filename) {
    ParsedName p;
    std::smatch m;
    if (!std::regex_match(filename, m, legacyRe())) return p;
    p.ok       = true;
    p.midi     = std::stoi(m[1].str());
    p.vel      = std::stoi(m[2].str());
    p.sr_tag   = std::stoi(m[3].str());
    p.filename = filename;
    return p;
}

ParsedName parseExtendedName(const std::string& filename) {
    ParsedName p;
    std::smatch m;
    if (!std::regex_match(filename, m, extendedRe())) return p;
    // Pozn.: legacy je podmnozina tohoto vzoru jen kdyby MIC byl "vel" — to ale
    // detectFormatFromName resi prioritou legacy, takze sem se legacy nedostane.
    p.ok       = true;
    p.midi     = std::stoi(m[1].str());
    p.mic      = m[2].str();
    p.hash     = m[3].str();
    p.filename = filename;
    return p;
}

BankFormat detectFormatFromName(const std::string& filename) {
    if (parseLegacyName(filename).ok)   return BankFormat::Legacy;
    if (parseExtendedName(filename).ok) return BankFormat::Extended;
    return BankFormat::Unknown;
}

BankScan scanBank(const std::string& dir) {
    BankScan scan;
    namespace fs = std::filesystem;
    std::error_code ec;

    int legacy_count = 0, extended_count = 0;
    std::vector<std::pair<ParsedName, std::string>> legacy_files, extended_files;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();

        ParsedName lg = parseLegacyName(fname);
        if (lg.ok) {
            legacy_files.emplace_back(lg, entry.path().string());
            legacy_count++;
            continue;
        }
        ParsedName ex = parseExtendedName(fname);
        if (ex.ok) {
            extended_files.emplace_back(ex, entry.path().string());
            extended_count++;
            continue;
        }
        scan.skipped++;
    }

    // Format urci vetsina rozpoznanych souboru (banka je homogenni).
    if (legacy_count == 0 && extended_count == 0) {
        scan.format = BankFormat::Unknown;
        return scan;
    }
    if (legacy_count >= extended_count) {
        scan.format = BankFormat::Legacy;
        for (auto& f : legacy_files)
            scan.files.push_back({f.first, f.second});
    } else {
        scan.format = BankFormat::Extended;
        for (auto& f : extended_files)
            scan.files.push_back({f.first, f.second});
    }
    return scan;
}

} // namespace ithaca
```

- [ ] **Step 4: Zaregistruj v CMake** — pridej `engine/sample/bank_index.cpp` do `ithaca_core`;
v `tests/CMakeLists.txt` pridej:

```cmake
add_executable(test_bank_index test_bank_index.cpp)
target_link_libraries(test_bank_index PRIVATE ithaca_core doctest)
add_test(NAME test_bank_index COMMAND test_bank_index)
```

- [ ] **Step 5: Build + test**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_bank_index
```
Expected: `test_bank_index` projde.

- [ ] **Step 6: Commit**

```bash
git add engine/sample/bank_index.h engine/sample/bank_index.cpp tests/test_bank_index.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze2): bank_index — parsing nazvu + detekce formatu (legacy plne, extended detekce)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: sample_loader — peak RMS + attack/sustain hranice (TDD)

**Files:**
- Create: `engine/sample/sample_loader.h`, `engine/sample/sample_loader.cpp`
- Test: `tests/test_sample_loader.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Ciste analyticke funkce nad uz nactenym interleaved stereo bufferem (zadne I/O — snadno
testovatelne na syntetickych datech).

- **Peak RMS**: maximum z kratko-oknovych RMS (klouzave okno ~50 ms), prevedeno na dBFS.
  Pocita se z mono mixu (L+R)/2. dBFS: `20*log10(rms)`, ticho → vrati podlahu (-120 dB).
- **Attack end / sustain start**: index frame, kde okenni RMS poprve dosahne sveho maxima
  (priblizne konec attacku u piana). Pouzije se pro resonance skip-attack (faze 5).

- [ ] **Step 1: Napis selhavajici test `tests/test_sample_loader.cpp`**

```cpp
// tests/test_sample_loader.cpp
// Testuje peak RMS a detekci konce attacku na syntetickych bufferech.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "sample/sample_loader.h"

#include <cmath>
#include <vector>

using namespace ithaca;

// Pomocna: vytvor interleaved stereo buffer z mono amplitud (L==R).
static std::vector<float> stereoFromMono(const std::vector<float>& mono) {
    std::vector<float> s(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) { s[i*2] = mono[i]; s[i*2+1] = mono[i]; }
    return s;
}

TEST_CASE("measurePeakRmsDb: plny rozsah (0 dBFS) pro konstantni 1.0") {
    std::vector<float> mono(48000, 1.0f);            // 1 s, plna amplituda
    auto s = stereoFromMono(mono);
    float db = measurePeakRmsDb(s.data(), (int)mono.size(), 48000);
    CHECK(db == doctest::Approx(0.0f).epsilon(0.05));   // RMS(1.0) = 1.0 → 0 dBFS
}

TEST_CASE("measurePeakRmsDb: -6 dBFS pro amplitudu 0.5") {
    std::vector<float> mono(48000, 0.5f);
    auto s = stereoFromMono(mono);
    float db = measurePeakRmsDb(s.data(), (int)mono.size(), 48000);
    CHECK(db == doctest::Approx(-6.02f).epsilon(0.1));
}

TEST_CASE("measurePeakRmsDb: ticho vrati podlahu") {
    std::vector<float> mono(48000, 0.0f);
    auto s = stereoFromMono(mono);
    float db = measurePeakRmsDb(s.data(), (int)mono.size(), 48000);
    CHECK(db <= -100.0f);
}

TEST_CASE("measurePeakRmsDb: chyti PEAK, ne prumer") {
    // 1 s ticha + kratky hlasity zaver → peak RMS musi byt blizko 0 dBFS,
    // ne prumer (ten by byl mnohem nizsi).
    std::vector<float> mono(48000, 0.0f);
    for (int i = 40000; i < 48000; ++i) mono[i] = 1.0f;   // hlasity konec
    auto s = stereoFromMono(mono);
    float db = measurePeakRmsDb(s.data(), (int)mono.size(), 48000);
    CHECK(db == doctest::Approx(0.0f).epsilon(0.5));
}

TEST_CASE("findAttackEnd: ramp nahoru → konec attacku je u vrcholu") {
    // Linearni ramp 0→1 pres 0.5 s, pak drzi 1.0 dalsich 0.5 s.
    std::vector<float> mono(48000, 1.0f);
    for (int i = 0; i < 24000; ++i) mono[i] = (float)i / 24000.f;
    auto s = stereoFromMono(mono);
    int ae = findAttackEnd(s.data(), (int)mono.size(), 48000);
    // Vrchol okenni RMS nastane az kdyz je okno plne v hlasite casti (~24000+).
    CHECK(ae >= 20000);
    CHECK(ae <= 30000);
}
```

- [ ] **Step 2: Vytvor `engine/sample/sample_loader.h`**

```cpp
#pragma once
// engine/sample/sample_loader.h
// -----------------------------
// Analyza uz nacteneho samplu: peak RMS (dBFS) a hranice attack/sustain.
// Ciste funkce nad interleaved stereo bufferem — zadne I/O.

namespace ithaca {

// Podlaha dBFS pro ticho (vyhneme se -inf z log10(0)).
constexpr float kSilenceFloorDb = -120.f;

// Maximum kratko-oknovych RMS (klouzave okno ~50 ms) v dBFS. Pocita z mono
// mixu (L+R)/2. data = interleaved stereo, frames = pocet stereo frames.
float measurePeakRmsDb(const float* data, int frames, int sample_rate);

// Index frame, kde okenni RMS poprve dosahne maxima — priblizny konec attacku
// / zacatek sustainu. Pouziva se pro resonance skip-attack (faze 5). Pro velmi
// kratke samply vrati 0.
int findAttackEnd(const float* data, int frames, int sample_rate);

} // namespace ithaca
```

- [ ] **Step 3: Vytvor `engine/sample/sample_loader.cpp`**

```cpp
// engine/sample/sample_loader.cpp — viz sample_loader.h.
#include "sample/sample_loader.h"

#include <cmath>

namespace ithaca {

namespace {
// Velikost okna pro klouzavy RMS v ms. 50 ms je kompromis: dost dlouhe na
// stabilni RMS, dost kratke na zachyceni peaku v transientu.
constexpr float kWindowMs = 50.f;

// Spocita okenni RMS (mono mix) a vrati (max_rms, frame_kde_max).
// Krok okna = polovina okna (50% overlap) pro rozumnou hustotu vzorkovani.
struct PeakResult { float max_rms; int peak_frame; };

PeakResult slidingPeakRms(const float* data, int frames, int sample_rate) {
    PeakResult r{0.f, 0};
    if (frames <= 0) return r;
    int win = (int)(kWindowMs * 0.001f * (float)sample_rate);
    if (win < 1) win = 1;
    if (win > frames) win = frames;
    const int hop = win > 1 ? win / 2 : 1;

    for (int start = 0; start + win <= frames; start += hop) {
        double acc = 0.0;
        for (int i = 0; i < win; ++i) {
            int f = start + i;
            float mono = 0.5f * (data[(size_t)f * 2] + data[(size_t)f * 2 + 1]);
            acc += (double)mono * (double)mono;
        }
        float rms = (float)std::sqrt(acc / (double)win);
        if (rms > r.max_rms) { r.max_rms = rms; r.peak_frame = start + win / 2; }
    }
    return r;
}
} // namespace

float measurePeakRmsDb(const float* data, int frames, int sample_rate) {
    if (frames <= 0 || data == nullptr) return kSilenceFloorDb;
    PeakResult r = slidingPeakRms(data, frames, sample_rate);
    if (r.max_rms <= 1e-7f) return kSilenceFloorDb;   // ~-140 dB → ticho
    float db = 20.f * std::log10(r.max_rms);
    return db < kSilenceFloorDb ? kSilenceFloorDb : db;
}

int findAttackEnd(const float* data, int frames, int sample_rate) {
    if (frames <= 0 || data == nullptr) return 0;
    PeakResult r = slidingPeakRms(data, frames, sample_rate);
    return r.peak_frame;
}

} // namespace ithaca
```

- [ ] **Step 4: Zaregistruj v CMake** — pridej `engine/sample/sample_loader.cpp` do `ithaca_core`;
v `tests/CMakeLists.txt` pridej:

```cmake
add_executable(test_sample_loader test_sample_loader.cpp)
target_link_libraries(test_sample_loader PRIVATE ithaca_core doctest)
add_test(NAME test_sample_loader COMMAND test_sample_loader)
```

- [ ] **Step 5: Build + test**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_sample_loader
```
Expected: `test_sample_loader` projde (5 test cases).

- [ ] **Step 6: Commit**

```bash
git add engine/sample/sample_loader.h engine/sample/sample_loader.cpp tests/test_sample_loader.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze2): sample_loader — peak RMS (dBFS) + detekce konce attacku

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: sample_store — nacteni legacy banky do RAM (TDD)

**Files:**
- Create: `engine/sample/sample_store.h`, `engine/sample/sample_store.cpp`
- Test: `tests/test_sample_store.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Slozi predchozi moduly: scan → pro kazdy soubor nacti WAV, zmer RMS+attack, vloz do `Bank`.
Legacy: slot index = `vel` tag (0-7), kazdy slot 1 variant 1 mic ("stereo"). Sloty pak serad
vzestupne podle rms_db. Sleduj `total_bytes` vs `cache_budget`; pri prekroceni jen WARNING (faze 2
porad nacita vse — evikce/streaming je faze 4).

- [ ] **Step 1: Napis selhavajici test `tests/test_sample_store.cpp`**

Test postavi malou fixture banku (par WAV souboru pres test helper) v docasnem adresari, nacte ji
a overi strukturu.

```cpp
// tests/test_sample_store.cpp
// Postavi malou legacy fixture banku v /tmp, nacte ji a overi NoteMap.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "sample/sample_store.h"
#include "util/log.h"

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

// Zapis stereo 16-bit WAV s konstantni amplitudou `amp` (0.3 s).
void writeConstWav(const std::string& path, float amp, int sr=48000) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    int frames = sr * 3 / 10;                         // 0.3 s
    uint32_t data_sz = (uint32_t)frames * 2u * 2u;    // stereo, 16-bit
    std::fwrite("RIFF",1,4,f); wU32(f,36u+data_sz);
    std::fwrite("WAVE",1,4,f); std::fwrite("fmt ",1,4,f); wU32(f,16u);
    wU16(f,1); wU16(f,2); wU32(f,(uint32_t)sr); wU32(f,(uint32_t)sr*4u);
    wU16(f,4); wU16(f,16);
    std::fwrite("data",1,4,f); wU32(f,data_sz);
    int16_t v = (int16_t)std::lround(amp*32767.f);
    for (int i=0;i<frames;i++){ std::fwrite(&v,2,1,f); std::fwrite(&v,2,1,f);} 
    std::fclose(f);
}
} // namespace

TEST_CASE("loadLegacyBank postavi NoteMap z fixture banky") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_fixture_bank";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // Nota 60: 3 vel vrstvy s rostouci amplitudou. Nota 62: 1 vrstva.
    writeConstWav(dir + "/m060-vel0-f48.wav", 0.1f);
    writeConstWav(dir + "/m060-vel4-f48.wav", 0.4f);
    writeConstWav(dir + "/m060-vel7-f48.wav", 0.9f);
    writeConstWav(dir + "/m062-vel3-f48.wav", 0.5f);

    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);                   // tichy pro test
    Bank bank = loadLegacyBank(dir, L);
    fs::remove_all(dir);

    CHECK(bank.format == BankFormat::Legacy);
    CHECK(bank.loaded_samples == 4);
    // Nota 60 ma 3 sloty, serazene vzestupne dle RMS.
    REQUIRE(bank.notes[60].recorded);
    REQUIRE(bank.notes[60].slots.size() == 3u);
    CHECK(bank.notes[60].slots[0].rms_db < bank.notes[60].slots[1].rms_db);
    CHECK(bank.notes[60].slots[1].rms_db < bank.notes[60].slots[2].rms_db);
    // Kazdy slot 1 round-robin variant, 1 mic (stereo).
    REQUIRE(bank.notes[60].slots[0].variants.size() == 1u);
    REQUIRE(bank.notes[60].slots[0].variants[0].mics.size() == 1u);
    CHECK(bank.notes[60].slots[0].variants[0].mics[0].mic_name == "stereo");
    CHECK(bank.notes[60].slots[0].variants[0].mics[0].sample_rate == 48000);
    // Nota 62 ma 1 slot.
    REQUIRE(bank.notes[62].recorded);
    CHECK(bank.notes[62].slots.size() == 1u);
    // Nota 61 nema sampl.
    CHECK_FALSE(bank.notes[61].recorded);
}

TEST_CASE("loadLegacyBank vrati prazdnou banku pro neexistujici adresar") {
    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);
    Bank bank = loadLegacyBank("/tmp/ithaca_neexistuje_zzz", L);
    CHECK(bank.loaded_samples == 0);
    CHECK_FALSE(bank.notes[60].recorded);
}
```

- [ ] **Step 2: Vytvor `engine/sample/sample_store.h`**

```cpp
#pragma once
// engine/sample/sample_store.h
// ----------------------------
// Nacteni banky do RAM. Faze 2 resi LEGACY: scan → nacti WAV → zmer RMS+attack
// → postav Bank. All-in-RAM (zadny streaming). cache_budget_mb je jen pro
// diagnosticky WARNING; evikce/streaming prijde ve fazi 4.

#include "sample/sample_types.h"
#include "util/log.h"

#include <string>

namespace ithaca {

// Nacte legacy banku z adresare `dir` do RAM. Loguje prubeh pres `logger`.
// cache_budget_mb: kdyz nactena data presahnou rozpocet, jen WARNING (porad
// nacita vse). 0 = bez kontroly rozpoctu.
Bank loadLegacyBank(const std::string& dir, log::Logger& logger,
                    int cache_budget_mb = 0);

} // namespace ithaca
```

- [ ] **Step 3: Vytvor `engine/sample/sample_store.cpp`**

```cpp
// engine/sample/sample_store.cpp — viz sample_store.h.
#include "sample/sample_store.h"

#include "io/wav_reader.h"
#include "sample/bank_index.h"
#include "sample/sample_loader.h"

#include <algorithm>
#include <filesystem>

namespace ithaca {

Bank loadLegacyBank(const std::string& dir, log::Logger& logger,
                    int cache_budget_mb) {
    Bank bank;
    bank.path = dir;
    bank.name = std::filesystem::path(dir).filename().string();

    BankScan scan = scanBank(dir);
    bank.format = scan.format;

    if (scan.format == BankFormat::Unknown) {
        logger.log("bank", log::Severity::Warning,
                   "Banka '%s': zadne rozpoznane samply (%d preskoceno)",
                   bank.name.c_str(), scan.skipped);
        return bank;
    }
    if (scan.format == BankFormat::Extended) {
        logger.log("bank", log::Severity::Warning,
                   "Banka '%s': extended format zatim nepodporovan (faze 7)",
                   bank.name.c_str());
        return bank;
    }

    logger.log("bank", log::Severity::Info,
               "Banka '%s': legacy, %zu souboru",
               bank.name.c_str(), scan.files.size());

    // Nacti kazdy soubor, zmer, vloz jako jeden VelocitySlot do prislusne noty.
    for (const auto& entry : scan.files) {
        const ParsedName& p = entry.parsed;
        if (p.midi < 0 || p.midi > 127) continue;

        WavData w = readWav(entry.full_path);
        if (!w.valid) {
            logger.log("bank", log::Severity::Warning,
                       "Nelze nacist: %s", p.filename.c_str());
            continue;
        }

        float rms = measurePeakRmsDb(w.samples.data(), w.frames, w.sample_rate);
        int   ae  = findAttackEnd(w.samples.data(), w.frames, w.sample_rate);

        MicLayer mic;
        mic.mic_name    = "stereo";
        mic.frames      = w.frames;
        mic.sample_rate = w.sample_rate;
        mic.data        = std::move(w.samples);

        SampleAsset asset;
        asset.peak_rms_db      = rms;
        asset.attack_end_frame = ae;
        asset.mics.push_back(std::move(mic));

        VelocitySlot slot;
        slot.rms_db = rms;
        slot.variants.push_back(std::move(asset));

        bank.notes[p.midi].slots.push_back(std::move(slot));
        bank.notes[p.midi].recorded = true;

        const MicLayer& m = bank.notes[p.midi].slots.back().variants[0].mics[0];
        bank.total_frames += (size_t)m.frames;
        bank.total_bytes  += m.data.size() * sizeof(float);
        bank.loaded_samples++;
    }

    // Serad sloty kazde noty vzestupne podle RMS (nejtissi → nejhlasitejsi).
    // Legacy vel tag je obvykle uz serazeny, ale RMS je autoritativni.
    for (int n = 0; n < 128; ++n) {
        auto& slots = bank.notes[n].slots;
        std::sort(slots.begin(), slots.end(),
                  [](const VelocitySlot& a, const VelocitySlot& b) {
                      return a.rms_db < b.rms_db;
                  });
    }

    logger.log("bank", log::Severity::Info,
               "Nacteno %d samplu, %zu frames, ~%zu MB RAM",
               bank.loaded_samples, bank.total_frames,
               bank.total_bytes / (1024 * 1024));

    if (cache_budget_mb > 0) {
        size_t budget_bytes = (size_t)cache_budget_mb * 1024 * 1024;
        if (bank.total_bytes > budget_bytes)
            logger.log("bank", log::Severity::Warning,
                       "Banka presahuje cache rozpocet (%d MB) — faze 4 doplni streaming",
                       cache_budget_mb);
    }
    return bank;
}

} // namespace ithaca
```

- [ ] **Step 4: Zaregistruj v CMake** — pridej `engine/sample/sample_store.cpp` do `ithaca_core`;
v `tests/CMakeLists.txt` pridej:

```cmake
add_executable(test_sample_store test_sample_store.cpp)
target_link_libraries(test_sample_store PRIVATE ithaca_core doctest)
add_test(NAME test_sample_store COMMAND test_sample_store)
```

- [ ] **Step 5: Build + test**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_sample_store
```
Expected: `test_sample_store` projde (2 test cases).

- [ ] **Step 6: Commit**

```bash
git add engine/sample/sample_store.h engine/sample/sample_store.cpp tests/test_sample_store.cpp CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze2): sample_store — nacteni legacy banky do RAM + stavba NoteMap

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: CLI --inspect + integracni overeni na realne bance

**Files:**
- Modify: `app/cli/main.cpp` (pridat prikaz `--inspect <bankdir>`)

`--inspect` nacte banku a vypise prehled: format, pocet not se samply, pro vzorek not pocet slotu
a jejich RMS, celkove frames/RAM. Slouzi k overeni na realne bance `/Users/j/SoundBanks/Ithaca/as-blackgrand`.

- [ ] **Step 1: Rozsir `app/cli/main.cpp`**

Pridej `#include "sample/sample_store.h"` k existujicim includes. Do parsovani argumentu pridej
vetev `--inspect` s nasledujicim chovanim a do usage textu radek. Implementace prikazu:

```cpp
// (uvnitr main, nova vetev pro --inspect <dir>)
// else if (a == "--inspect" && i + 1 < argc) { inspect_dir = argv[++i]; }

// ... po nastaveni loggeru, pred/misto selftest bloku:
if (!inspect_dir.empty()) {
    auto& L = log::Logger::default_();
    L.setMinSeverity(level);
    L.setOutputMode(/*console=*/true, /*file=*/false);
    LOG_INFO("inspect", "Nacitam banku: %s", inspect_dir.c_str());

    Bank bank = loadLegacyBank(inspect_dir, L);
    if (bank.loaded_samples == 0) {
        LOG_ERROR("inspect", "Zadne samply nenacteny z %s", inspect_dir.c_str());
        return 1;
    }

    int notes_with_samples = 0;
    for (int n = 0; n < 128; ++n)
        if (bank.notes[n].recorded) notes_with_samples++;

    LOG_INFO("inspect", "Format: %s", bankFormatName(bank.format));
    LOG_INFO("inspect", "Not se samply: %d", notes_with_samples);
    LOG_INFO("inspect", "Celkem samplu: %d, frames: %zu, RAM ~%zu MB",
             bank.loaded_samples, bank.total_frames,
             bank.total_bytes / (1024 * 1024));

    // Vypis detail pro prvni nalezenou notu se samply (vzorek).
    for (int n = 0; n < 128; ++n) {
        if (!bank.notes[n].recorded) continue;
        LOG_INFO("inspect", "Nota %d: %zu slotu", n, bank.notes[n].slots.size());
        for (size_t s = 0; s < bank.notes[n].slots.size(); ++s)
            LOG_INFO("inspect", "  slot %zu: RMS %.1f dBFS, attack_end %d",
                     s, bank.notes[n].slots[s].rms_db,
                     bank.notes[n].slots[s].variants[0].attack_end_frame);
        break;   // jen prvni nota jako vzorek
    }
    return 0;
}
```

Deklaruj `std::string inspect_dir;` k ostatnim lokalnim promennym na zacatku main a v usage textu
pridej radek `"  --inspect <dir>      nacti banku a vypis prehled\n"`.

- [ ] **Step 2: Build a over na realne bance**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy && make build && ./build/ithaca-cli --inspect /Users/j/SoundBanks/Ithaca/as-blackgrand
```
Expected: vypise `Format: legacy`, `Not se samply: 88`, `Celkem samplu: 704`, a pro notu 21
`8 slotu` s rostoucimi RMS hodnotami. Exit 0.

- [ ] **Step 3: Over ze stavajici testy + smoke porad prochazi**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy && make test && make smoke
```
Expected: vsechny ctest targety projdou (test_log/ithaca_tests, test_wav_reader, test_bank_index,
test_sample_loader, test_sample_store); smoke OK.

- [ ] **Step 4: Commit**

```bash
git add app/cli/main.cpp
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze2): ithaca-cli --inspect — nacti banku a vypis prehled NoteMap

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Hotovo — kriteria dokonceni faze 2

- `make build` cisty na macOS; `make test` — vsechny doctest targety projdou (logger + 4 nove
  moduly).
- `ithaca-cli --inspect /Users/j/SoundBanks/Ithaca/as-blackgrand` vypise: legacy, 88 not, 704
  samplu, ~RAM, a per-nota sloty s RMS.
- Datovy model `Bank → NoteSlots → VelocitySlot → SampleAsset → MicLayer` stoji a je naplneny z
  legacy banky; sloty serazene podle RMS.
- WAV reader cte 16/24/32-bit PCM + float, mono→stereo; `peekWavInfo` cte hlavicku.
- Format se auto-detekuje (legacy plne; extended rozpoznan a odlozen na fazi 7).

Tim je banka v RAM. Faze 3 navese voice pool + engine fasadu + CLI prehravani (polyfonie,
velocity vyber slotu, retrigger, pitch-shift chybejicich not) — porad all-in-RAM, streaming az faze 4.
