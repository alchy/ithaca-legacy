# Faze 4 — streaming samplu z disku (DFD) — implementacni plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (doporuceno) nebo superpowers:executing-plans. Kroky pouzivaji checkbox (`- [ ]`).
>
> Jazyk: komentare/docs/commit messages cesky bez diakritiky; identifikatory anglicky;
> komentovat spise vice (explicit > implicit).

**Goal:** Misto drzeni cele banky v RAM (~5.4 GB pro as-blackgrand) drz v RAM jen ZACATEK
kazdeho samplu (preload region) + handle na soubor. Audio thread cte vyhradne z RAM (preload na
zacatku, ring buffer pak). Background streaming thread plni ring buffer kazdeho aktivniho hlasu
z disku. Underrun → rychly fade do ticha + log. Tim odpadne nutnost drzet celou banku v RAM.

**Architecture:**
- `MicLayer` ma DVA preload regiony: head `[0..preload_ms]` (potreba pro note-on) + rezonancni
  okno `[peak_RMS..peak_RMS+resonance_window_ms]` (potreba pro budouci rezonanci ve fazi 5; ve
  fazi 4 se LOADUJE ale NEPOUZIVA). Misto plnych `data` drzime SampleFile (cesta + WAV metadata
  pres `peekWavInfo`).
- Kratke samply (vejdou se do `preload_ms * 2`) zustavaji CELE v RAM (head region pokryje vse).
  Streaming je jen pro dlouhe samply (basy 30 s).
- Voice si pri start() rezervuje slot v ring buffer pool (jen kdyz sampl streamuje), drzi cursor
  v souboru a cte z ringu. Kdyz cursor je porad v head regionu, cte primo z preload bufferu.
- `StreamEngine` ma worker thread(y); fronta pozadavku `(voice_id, file_offset_frames, n_frames)`.
  Pri kazdem audio bloku Voice oznaci, kolik mu zbyva v ringu; pod prah → posle pozadavek.
- Underrun: kdyz Voice cte z ringu a ten je prazdny pred dohranim, spusti se kratky linearni fade
  do ticha (~5 ms ramp), voice se deaktivuje a zaloguje LOG_RT_WARN.

**Tech Stack:** C++20, navazuje na faze 1-3. Pouziva existujici `wav_reader::peekWavInfo` +
nove `readWavRange(path, frame_offset, frame_count)`. Build/verifikace macOS.

---

## Kontext a zdrojove vzory

- Faze 2 loader: `engine/sample/sample_store.cpp` — `loadLegacyBank` nacita CELE samply do
  `MicLayer.data`. Tohle prepiseme.
- Faze 2 WAV reader: `engine/io/wav_reader.{h,cpp}` — `WavData readWav()` cte cele, `peekWavInfo`
  cte jen hlavicku. Pridame `readWavRange` (cte podmnozinu frames).
- Faze 3 Voice: `engine/voice/voice.{h,cpp}` — cte z `mic_->data.data()` jako kontinualni buffer
  s `position_` (double, fractional). Prepiseme tak, aby cetla z preload bufferu nebo ringu podle
  toho, kde je cursor.
- Faze 1 RT logger: `LOG_RT_WARN("voice", ...)` z audio threadu; `Logger::default_().flushRTBuffer()`
  z non-RT.
- Faze 2 `MidiQueue` (engine/midi/midi_queue.h) jako vzor lock-free SPSC: stream requesty / responses
  pouziji stejny princip (release/acquire publish, drop-on-full pro requesty).
- Realna dev banka: `/Users/j/SoundBanks/Ithaca/as-blackgrand` — 704 souboru, basy ~30 s. Po fazi 4
  by mela --inspect/--play start byt RYCHLEJSI a RAM mensi.

### Cilova struktura po fazi 4 (nove + modifikovane soubory)

```
engine/
  io/
    wav_reader.h / .cpp        + readWavRange(path, off, n) → interleaved stereo float
  sample/
    sample_types.h             MicLayer prepsany: 2 preload regiony + SampleFile handle
    sample_store.h / .cpp      loadLegacyBank nacita jen preload regiony (ne cely sampl)
  stream/
    stream_engine.h / .cpp     pool ring bufferu, worker thread(y), fronta pozadavku
  voice/
    voice.h / .cpp             cte z preload nebo z ringu podle cursoru; underrun fade
    voice_pool.h / .cpp        pri noteOn alokuje ringy pro streamovane voices
  engine.h / .cpp              init bere streamingove parametry; processBlock drainuje
                               flushRTBuffer() (uz ho mame, jen pripomenout v render smyckach)
tests/
  test_wav_range.cpp           readWavRange round-trip
  test_stream_engine.cpp       ring pool + worker fill + underrun
  test_long_sample_stream.cpp  integ: voice nad simulovanym dlouhym samplem
```

### Konvence commitu (jako predchozi faze)

`typ(faze4): popis` cesky bez diakritiky; `git add` konkretni soubory; trailer
`Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`; commit pres
`git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit`.

Pred zacatkem prace: vytvor a prepni se na vetev `faze4-streaming` z `main`.

---

## Task 1: `wav_reader::readWavRange` (TDD)

**Files:**
- Modify: `engine/io/wav_reader.h`, `engine/io/wav_reader.cpp`
- Test: `tests/test_wav_range.cpp`
- Modify: `tests/CMakeLists.txt`

Cte konkretni vyrez WAV souboru → interleaved stereo float. Pouziva ho preload (cti hlavu) i
streaming worker (cti dalsi blok). Vrati pripadne kratsi vystup, kdyz vyrez prekracuje konec.

- [ ] **Step 1: Napis selhavajici test `tests/test_wav_range.cpp`**

```cpp
// tests/test_wav_range.cpp
// readWavRange: cte vyrez WAV. Round-trip s nasim wav_writerem.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "io/wav_reader.h"
#include "io/wav_writer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ithaca;

namespace {
// Vytvori temp WAV s ramp signalem: L[i]=i/N, R[i]=-i/N (4096 stereo frames).
std::string makeRampWav(const char* tag, int frames = 4096, int sr = 48000) {
    std::vector<float> samples((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        samples[(size_t)i * 2]     =  (float)i / (float)frames;
        samples[(size_t)i * 2 + 1] = -(float)i / (float)frames;
    }
    std::string p = std::string("/tmp/ithaca_range_") + tag + ".wav";
    REQUIRE(writeWavStereo16(p, samples, sr));
    return p;
}
} // namespace

TEST_CASE("readWavRange cte vyrez (offset, count) presne") {
    std::string p = makeRampWav("mid");
    WavData w = readWavRange(p, /*frame_off=*/1000, /*frame_count=*/256);
    std::remove(p.c_str());
    REQUIRE(w.valid);
    CHECK(w.frames == 256);
    CHECK(w.sample_rate == 48000);
    REQUIRE(w.samples.size() == 256u * 2u);
    // Prvni frame ve vyrezu = puvodni frame 1000.
    CHECK(w.samples[0]   == doctest::Approx( 1000.f / 4096.f).epsilon(0.001));
    CHECK(w.samples[1]   == doctest::Approx(-1000.f / 4096.f).epsilon(0.001));
    // Posledni frame ve vyrezu = puvodni frame 1255.
    CHECK(w.samples[255 * 2]     == doctest::Approx( 1255.f / 4096.f).epsilon(0.001));
    CHECK(w.samples[255 * 2 + 1] == doctest::Approx(-1255.f / 4096.f).epsilon(0.001));
}

TEST_CASE("readWavRange ori§ne pozadavek presahujici konec souboru") {
    std::string p = makeRampWav("end");
    // Soubor ma 4096 frames; pozadame o 1000 frames od offsetu 4000 → vrati 96.
    WavData w = readWavRange(p, 4000, 1000);
    std::remove(p.c_str());
    REQUIRE(w.valid);
    CHECK(w.frames == 96);
    CHECK(w.samples.size() == 96u * 2u);
}

TEST_CASE("readWavRange offset >= konec souboru → 0 frames, stale valid") {
    std::string p = makeRampWav("past");
    WavData w = readWavRange(p, 4096, 100);
    std::remove(p.c_str());
    REQUIRE(w.valid);
    CHECK(w.frames == 0);
    CHECK(w.samples.empty());
}

TEST_CASE("readWavRange na neexistujicim souboru → invalid") {
    WavData w = readWavRange("/tmp/ithaca_no_such_file_xyz.wav", 0, 100);
    CHECK_FALSE(w.valid);
}
```

- [ ] **Step 2: Rozsir `engine/io/wav_reader.h`** — pridej deklaraci:

```cpp
// Cte konkretni vyrez WAV souboru → interleaved stereo float. frame_off a
// frame_count jsou ve stereo frames (ne v samplech). Vrati WavData s
// frames = min(frame_count, dostupne_do_konce_souboru). Pri offsetu za koncem
// vrati valid=true s frames=0 a prazdnym samples (pro streaming je to OK
// signal "konec"). Pri chybe otevreni/parsovani vrati valid=false.
WavData readWavRange(const std::string& path, int frame_off, int frame_count);
```

- [ ] **Step 3: Implementuj v `engine/io/wav_reader.cpp`** — najdi privat namespace s `parseHeader`
a `sampleToFloat`. Pridej za `readWav()`:

```cpp
WavData readWavRange(const std::string& path, int frame_off, int frame_count) {
    WavData out;
    if (frame_off < 0 || frame_count <= 0) {
        // Defensivne: 0 frame_count -> 0-frames valid result.
        if (frame_off >= 0 && frame_count == 0) { out.valid = true; }
        return out;
    }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;

    FmtInfo fmt; uint32_t data_size = 0; bool found_data = false;
    if (!parseHeader(f, fmt, data_size, found_data) || !found_data) {
        std::fclose(f);
        return out;
    }
    const int bytes_per_sample = fmt.bits / 8;
    if (bytes_per_sample <= 0 || fmt.channels == 0) { std::fclose(f); return out; }

    // parseHeader nechal soubor pozicovany na zacatku data chunku. Zjisti realny
    // zbytek v souboru (header data_size muze lhat — viz peekWavInfo).
    long data_start = std::ftell(f);
    std::fseek(f, 0, SEEK_END);
    long file_end = std::ftell(f);
    uint32_t avail_bytes = (file_end > data_start)
                         ? (uint32_t)(file_end - data_start) : 0u;
    if (avail_bytes < data_size) data_size = avail_bytes;

    const int frame_bytes  = bytes_per_sample * fmt.channels;
    const int total_frames = (int)(data_size / (uint32_t)frame_bytes);

    // Offset za koncem souboru → 0 frames, ale valid (signal "konec streamu").
    if (frame_off >= total_frames) {
        std::fclose(f);
        out.frames      = 0;
        out.sample_rate = (int)fmt.sample_rate;
        out.valid       = true;
        return out;
    }

    int avail_frames = total_frames - frame_off;
    int read_frames  = (frame_count < avail_frames) ? frame_count : avail_frames;

    long byte_off = data_start + (long)frame_off * frame_bytes;
    std::fseek(f, byte_off, SEEK_SET);

    std::vector<uint8_t> raw((size_t)read_frames * (size_t)frame_bytes);
    size_t got = std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    int actual_frames = (int)(got / (size_t)frame_bytes);

    out.samples.resize((size_t)actual_frames * 2);
    for (int i = 0; i < actual_frames; ++i) {
        const uint8_t* base = raw.data() + (size_t)i * fmt.channels * bytes_per_sample;
        float L = sampleToFloat(base, fmt.bits, fmt.audio_format);
        float R = (fmt.channels >= 2)
                ? sampleToFloat(base + bytes_per_sample, fmt.bits, fmt.audio_format)
                : L;
        out.samples[(size_t)i * 2]     = L;
        out.samples[(size_t)i * 2 + 1] = R;
    }
    out.frames      = actual_frames;
    out.sample_rate = (int)fmt.sample_rate;
    out.valid       = true;
    return out;
}
```

- [ ] **Step 4: Registruj test** v `tests/CMakeLists.txt`:

```cmake
add_executable(test_wav_range test_wav_range.cpp)
target_link_libraries(test_wav_range PRIVATE ithaca_core doctest)
add_test(NAME test_wav_range COMMAND test_wav_range)
```

- [ ] **Step 5: Build + test**

Run: `cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure -R test_wav_range`
Expected: 4 cases projdou.

- [ ] **Step 6: Commit**

```bash
git add engine/io/wav_reader.h engine/io/wav_reader.cpp tests/test_wav_range.cpp tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze4): readWavRange — cte vyrez WAV pro preload + streaming

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: prepsani `MicLayer` (2 preload regiony + SampleFile handle)

**Files:**
- Modify: `engine/sample/sample_types.h`

Pridame strukturu odkazu na soubor a 2 preload regiony. `data` se prejmenuje na `preload_head`
a pribude `preload_resonance` + `file`. **Tato zmena rozbije buildy** dokud Task 3 nezacne
loader/voice na ni reagovat — proto je tento commit prozatim s "skipnutymi" testy (Task 3-5
to vse propoji). Alternativa: udelat Task 2+3 jako jeden velky commit. Volime postupne, ale
v ramci Tasku 2 zalogujeme problem a vsechna mista, ktera bude treba prepsat.

POZN.: misto rozbiti zachovame docasne `data` jako kompat alias = `preload_head`, aby fazi 4 sla
postavit po krocich. V Tasku 3 a 5 to vyresime ciste.

- [ ] **Step 1: Prepis `engine/sample/sample_types.h`**

Nahrad blok `struct MicLayer { ... }` timto:

```cpp
// Rezim, jak je MicLayer drzen v pameti:
// - FullyLoaded: kratky sampl, vejde se cely do preload_head (zadny streaming).
// - Streamed:    dlouhy sampl, preload_head ma jen zacatek, zbytek se streamuje
//                z `file` cestou ring bufferu.
enum class MicLayerMode { FullyLoaded, Streamed };

// Reference na zdrojovy WAV — drzi cestu a klicove metadata.
struct SampleFile {
    std::string path;
    int  frames      = 0;     // celkovy pocet frames v souboru
    int  sample_rate = 0;
    bool valid       = false;
};

// Jedna mic perspektiva jednoho uhozu. Faze 4: preload jen zacatek (head) +
// rezonancni okno (od peak RMS pozice) v RAM; zbytek lezi v souboru a streamuje
// se. Kratky sampl drzi cely v preload_head a `mode = FullyLoaded`.
struct MicLayer {
    std::string mic_name;            // legacy: "stereo"; extended: "main"/"micpos-A"/...
    SampleFile  file;                // odkaz na zdrojovy soubor
    MicLayerMode mode = MicLayerMode::FullyLoaded;

    // Preload region "hlava": [0..head_frames). VZDY pritomny (i pro kratke,
    // ktere ho maji = celemu samplu).
    std::vector<float> preload_head;     // interleaved stereo
    int head_frames = 0;

    // Preload region "rezonance": [resonance_start_frame..+resonance_frames).
    // Pouziva se az ve fazi 5 (rezonancni hlasy). Ve fazi 4 muze byt prazdny
    // pokud peak_RMS lezi v preload_head (jiz pokryto) nebo pro kratke samply.
    std::vector<float> preload_resonance;
    int resonance_start_frame = 0;
    int resonance_frames      = 0;

    // KOMPAT pro faze 2-3: do faze 5 ponechavame nazev `data` jako alias na
    // preload_head; voice (Task 3) si bude radeji sahat primo na preload_head.
    // Po dokonceni faze 5 muzeme `data` smazat.
    const std::vector<float>& data() const { return preload_head; }
    // Frames pristupne v RAM od zacatku samplu (= head_frames).
    int  frames_in_ram_head() const { return head_frames; }
    // Pro voice: kolik frames samplu existuje celkove (file.frames).
    int  total_frames() const { return file.frames; }
};
```

POZN.: dosavadni `frames` a `sample_rate` v `MicLayer` se prelozi tak, ze `frames` ↔ `file.frames`
a `sample_rate` ↔ `file.sample_rate`. Pridame v Tasku 3 odpovidajici accessory; tady jen pripravime
strukturu.

- [ ] **Step 2: Build** — schvalne necheme overovat, ze projde, jen zachytime, co se rozbije:

Run: `cd /Users/j/Projects/ithaca-legacy && make build 2>&1 | tail -30`
Expected: ZALOGUJ chyby, ktere se objevi (`sample_store.cpp`, `voice.cpp`/`voice_pool.cpp`,
`engine.cpp` budou volat `.data`/`.frames`/`.sample_rate` jako pole misto metod). To je ocekavane.

Bez commitnuti pokracuj na Task 3, ktery loader a voice prepise. Pokud ti vadi rozdelane stage:
**alternativa** — Task 2 + Task 3 jako jeden commit (preferovany postup, ale soubor bude vetsi).

UPDATE PLANU: lepsi je opravdu udelat Task 2 + 3 v jednom commitu, aby build nikdy nebyl rozbity.
Niz v Tasku 3 to provedeme — Task 2 je tedy jen jako logicky prulet, NEcommituj samostatne.

---

## Task 3: prepsani `sample_store::loadLegacyBank` (preload + SampleFile) + `voice` na nove API

**Files:**
- Modify (vsechno v jednom commitu spolu s Task 2):
  - `engine/sample/sample_types.h` (z Tasku 2)
  - `engine/sample/sample_store.cpp`
  - `engine/voice/voice.h`, `engine/voice/voice.cpp`
  - `engine/voice/voice_pool.h`, `engine/voice/voice_pool.cpp` (drobnost: predani `engine_sr` zustava)
  - `engine/engine.cpp` (drobnost: prazdne — voice si bere data primo z MicLayer)
- Test: `tests/test_sample_store.cpp` (aktualizuj asserce — frames se cte z `mic.file.frames`,
  data jdou primo z `mic.preload_head`)

### Plan zmen

1. `loadLegacyBank`:
   - Pro kazdy soubor: `peekWavInfo(path)` → naplni `mic.file`. Spocita `head_frames =
     min(preload_ms*sr/1000, file.frames)`. Pokud `file.frames <= preload_ms*sr*2/1000` ("kratky"),
     `mode = FullyLoaded` a `head_frames = file.frames`; jinak `mode = Streamed`.
   - `mic.preload_head = readWavRange(path, 0, head_frames).samples` (kdyz `head_frames>0`).
   - Pro fazi 4: `preload_resonance` necheme prazdne (rezonance je faze 5).
   - `bank.total_bytes` pocita jen v RAM rezidentni preload (head + resonance).
   - `peak_rms_db` / `attack_end_frame`: meri z preload_head (u kratkych je to cely sampl; u dlouhych
     je hlava typicky obsahuje attack — to staci, peak RMS u piana je v attacku). POZN.: pro
     fazi 5 by se idealne meril resonance region z preload_resonance, ale to az s rezonanci.
   - Pridej parametr `int preload_ms`, default `150`. Engine ho preda z `EngineConfig`.

2. `MicLayer` accessors:
   - Pridej `int frames_in_ram_head() const { return head_frames; }`
   - Pridej `int total_frames()       const { return file.frames; }`
   - (data() jiz mame z Tasku 2.)

3. `Voice`:
   - Misto `mic_->data.data()` cte `mic_->preload_head.data()`.
   - Pridej `total_frames_` = `mic_->total_frames()`. End-of-sample guard pouziva `total_frames_`.
   - Nove pole `ringbuf_*` (od Tasku 4) zatim ne — voice ve fazi 4 (Task 3) jen cte z `preload_head`
     a hraje JEN tak dlouho, dokud cursor je v hlave. Pri prekroceni hlavy ZATIM utichne (nehraje
     dal). To je docasne — Task 4 prida streaming a voice bude pokracovat z ringu.
     POZN.: pri legacy bance jsou skoro vsechny samply "FullyLoaded" (vejdou se do 2*preload_ms?
     NE — basy 30 s × 48k × stereo × 4 B = ~11 MB → vetsi nez head). Takze ihned po Tasku 3 budou
     dlouhe samply hrat jen prvnich ~150 ms. To je OK STAGED stav; Task 4 to dodela.

4. `test_sample_store.cpp` aktualizace:
   - Kratky fixture (0.3 s / 14400 frames) je < `preload_ms*2*sr/1000 = 14400` → "kratky" (FullyLoaded).
     Test musi byt explicitni: zkontrolovat `mode == FullyLoaded`, `head_frames == file.frames`.
   - Pridej druhy fixture s dlouhym samplem (napr. 60 000 frames konstantni amplitudou) → ocekavej
     `mode == Streamed`, `head_frames == preload_ms*sr/1000`.

- [ ] **Step 1: Aplikuj Task 2 zmeny** v `sample_types.h` (viz vyse).
- [ ] **Step 2: Prepis `sample_store.cpp`** — `loadLegacyBank`:

```cpp
// (klicova cast — uplne nahradi smycku "Nacti kazdy soubor, zmer, vloz..."):

#include "io/wav_reader.h"   // peekWavInfo + readWavRange

// (preload_ms parametr pridej do signatury + default 150)
Bank loadLegacyBank(const std::string& dir, log::Logger& logger,
                    int cache_budget_mb,
                    int midi_from, int midi_to,
                    int preload_ms) {
    // ... existujici cast (scanBank, format check, logging — beze zmeny) ...

    for (const auto& entry : scan.files) {
        const ParsedName& p = entry.parsed;
        if (p.midi < 0 || p.midi > 127) continue;
        if (p.midi < midi_from || p.midi > midi_to) continue;

        WavInfo info = peekWavInfo(entry.full_path);
        if (!info.valid) {
            logger.log("bank", log::Severity::Warning,
                       "Nelze precist hlavicku: %s", p.filename.c_str());
            continue;
        }

        MicLayer mic;
        mic.mic_name = "stereo";
        mic.file.path        = entry.full_path;
        mic.file.frames      = info.frames;
        mic.file.sample_rate = info.sample_rate;
        mic.file.valid       = true;

        // "Kratky" sampl = vejde se do 2 * preload_ms. Drzime cely v RAM (head).
        const int preload_frames = (int)((int64_t)preload_ms * info.sample_rate / 1000);
        const int short_threshold_frames = preload_frames * 2;
        if (info.frames <= short_threshold_frames) {
            mic.mode        = MicLayerMode::FullyLoaded;
            mic.head_frames = info.frames;
        } else {
            mic.mode        = MicLayerMode::Streamed;
            mic.head_frames = preload_frames;
        }

        // Nacti preload_head [0 .. head_frames).
        WavData head = readWavRange(entry.full_path, 0, mic.head_frames);
        if (!head.valid || head.frames < mic.head_frames) {
            logger.log("bank", log::Severity::Warning,
                       "Nelze nacist preload hlavicky: %s", p.filename.c_str());
            continue;
        }
        mic.preload_head = std::move(head.samples);

        // preload_resonance ve fazi 4 zatim prazdny (rezonance je faze 5).
        // Asset:
        SampleAsset asset;
        // peak_rms_db / attack_end_frame meri z preload_head — typicky pokryje attack.
        asset.peak_rms_db      = measurePeakRmsDb(mic.preload_head.data(),
                                                  mic.head_frames, info.sample_rate);
        asset.attack_end_frame = findAttackEnd  (mic.preload_head.data(),
                                                  mic.head_frames, info.sample_rate);
        asset.mics.push_back(std::move(mic));

        VelocitySlot slot;
        slot.rms_db = asset.peak_rms_db;
        slot.variants.push_back(std::move(asset));

        bank.notes[p.midi].slots.push_back(std::move(slot));
        bank.notes[p.midi].recorded = true;

        const MicLayer& m = bank.notes[p.midi].slots.back().variants[0].mics[0];
        bank.total_frames += (size_t)m.head_frames;
        bank.total_bytes  += m.preload_head.size() * sizeof(float);
        bank.loaded_samples++;
    }

    // ... sort slotu dle RMS + cache budget warning ... (beze zmeny)
}
```

Aktualizuj `sample_store.h` signaturu: pridej `int preload_ms = 150`.
Aktualizuj `engine.cpp` na: `loadLegacyBank(dir, L, 0, cfg_.midi_from, cfg_.midi_to, cfg_.preload_ms);`.
Pridej `int preload_ms = 150;` do `EngineConfig`.

- [ ] **Step 3: Prepis `voice.cpp`**: nahrad `data = mic_->data.data()` a `max_frames = mic_->frames`
za `data = mic_->preload_head.data()` a `max_frames = mic_->head_frames`. V Tasku 4 to zaroven
zacne padat na ring buffer.

- [ ] **Step 4: Aktualizuj `test_sample_store.cpp`** — kazda asserce co cte `mic.frames` musi cist
`mic.file.frames` nebo `mic.head_frames` (podle kontextu). `mic.sample_rate` → `mic.file.sample_rate`.
Pridej test pro dlouhy sampl (FullyLoaded vs Streamed).

- [ ] **Step 5: Build + cely ctest**

```bash
cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure
```
Expected: vsechno projde. (Voice testy stale projdou — fixture jsou 256-frame, krotke, FullyLoaded.)

- [ ] **Step 6: Manualni overeni rychlosti na realne bance**

```bash
./build/ithaca-cli --inspect /Users/j/SoundBanks/Ithaca/as-blackgrand
```
Expected: drasticky kratsi load + mnohem mensi RAM (RAM ~ `704 souboru × ~150 ms preload`,
~ desitky MB misto 5392 MB). Zaloguj v reportu skutecne hodnoty.

- [ ] **Step 7: Commit** (Task 2 + Task 3 v JEDNOM commitu)

```bash
git add engine/sample/sample_types.h engine/sample/sample_store.h engine/sample/sample_store.cpp \
        engine/voice/voice.cpp engine/engine.h engine/engine.cpp tests/test_sample_store.cpp
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze4): MicLayer s 2 preload regiony + loader nacita jen preload (ne cely sampl)

MicLayer: SampleFile handle + preload_head + preload_resonance (oba regiony
pripraveny; rezonance ve fazi 5). loadLegacyBank nyni cte jen
preload_ms (default 150) frames z kazdeho souboru; kratky sampl (<2*preload)
zustava cely v RAM (FullyLoaded), dlouhy se oznaci Streamed. Voice cte z
preload_head a nehraje za jeho konec (streaming je Task 4 dale).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `stream_engine` + integrace do Voice (TDD)

**Files:**
- Create: `engine/stream/stream_engine.h`, `engine/stream/stream_engine.cpp`
- Modify: `engine/voice/voice.h`, `engine/voice/voice.cpp`,
  `engine/voice/voice_pool.h`, `engine/voice/voice_pool.cpp`,
  `engine/engine.h`, `engine/engine.cpp`
- Test: `tests/test_stream_engine.cpp`, `tests/test_long_sample_stream.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

### Architektura `StreamEngine`

- **Ring buffer pool**: `N` (default 32) pre-alokovanych ringu, kazdy `ring_capacity_frames` (napr.
  `8192` frames = ~170 ms @48k). Vlastni pole `float[ring_capacity_frames * 2]`. Drzi
  `std::atomic<size_t> w_, r_` (SPSC: producent = worker, konzument = audio thread; identicke vzoru
  z `midi_queue.h`).
- **Allocator**: `acquireRing()` najde volny ring (atomic flag `in_use`), vrati `RingHandle*`;
  `releaseRing(ring)` ho uvolni (worker, voice). Voice si bere ring jen kdyz `mode == Streamed`.
- **Worker thread**: cte SPSC frontu `StreamRequest{ring_id, path, frame_off, n_frames}`. Pro kazdy
  request: `readWavRange(path, off, n)` → zapise frames do ringu (skrz cyklus po ring_capacity).
  Vola jen ze stream threadu; audio thread NEVI nic o disku.
- **Voice request logic**: pri kazdem audio bloku po precteni z ringu Voice zkontroluje, kolik
  frames zbyva v ringu. Kdyz < `refill_threshold` (napr. 1/2 ringu), Voice posle pozadavek do
  fronty. Worker ho prijme a postupne plni. Pri end-of-file pripadne worker zapise "EOF marker"
  (np. zvlastni atomicky flag na ringu).
- **Underrun**: kdyz Voice cte z ringu a najednou je prazdny (pred celkovym koncem samplu), spusti
  rychly fade (linear ~5 ms ramp), voice se po fade deaktivuje, LOG_RT_WARN. Engine pak v non-RT
  flushne RT log.

### Voice update

Voice drzi:
- `RingHandle* ring_ = nullptr` (nullptr = FullyLoaded → cte jen z `preload_head`).
- `int file_cursor_frames_` (jaky frame jiz pozadal stream engine).
- Pri `start()` kdyz `mic_->mode == Streamed`: alokuj ring (`stream_->acquireRing()`); poslelej
  prvni pozadavek (`StreamRequest` na `frame_off = head_frames`).
- V `process()`: kdyz `position_ < head_frames`, ctem z `preload_head[position_*2]`. Kdyz
  `position_ >= head_frames`, ctem z ringu. Kdyz neni dost v ringu a uz neexistuje vic na disku →
  konec samplu (clean). Kdyz neni dost v ringu a JESTE existuje vic na disku → underrun fade.
- Po note-off / EOF / steal → `stream_->releaseRing(ring_)`.

POZN.: dvojite cteni (z preload_head pak z ringu) je trochu vic vetvenia; je to OK, faze 4 to
priznava. Optimalizace prijde s benchmarkem v dalsich fazich.

- [ ] **Step 1: Napis selhavajici test `tests/test_stream_engine.cpp`** (jednotky ringu + worker)

```cpp
// tests/test_stream_engine.cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "stream/stream_engine.h"
#include "io/wav_writer.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace ithaca;

namespace {
std::string makeRampWav(const char* tag, int frames) {
    std::vector<float> s((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) { s[i*2] = (float)i / frames; s[i*2+1] = -s[i*2]; }
    std::string p = std::string("/tmp/ithaca_stream_") + tag + ".wav";
    REQUIRE(writeWavStereo16(p, s, 48000));
    return p;
}
} // namespace

TEST_CASE("StreamEngine acquire/release ring slot") {
    StreamEngine se(/*n_rings=*/4, /*ring_capacity_frames=*/1024);
    se.start();
    RingHandle* r1 = se.acquireRing();
    REQUIRE(r1 != nullptr);
    RingHandle* r2 = se.acquireRing(); REQUIRE(r2 != nullptr);
    se.releaseRing(r1);
    RingHandle* r3 = se.acquireRing(); REQUIRE(r3 != nullptr);   // recycluje r1
    se.releaseRing(r2); se.releaseRing(r3);
    se.stop();
}

TEST_CASE("StreamEngine worker naplni ring z WAV souboru") {
    std::string p = makeRampWav("worker", 8000);
    StreamEngine se(2, 2048);
    se.start();
    RingHandle* r = se.acquireRing();
    REQUIRE(r != nullptr);
    se.requestRead(r, p, /*frame_off=*/100, /*n_frames=*/512);
    // Pockej maximalne 100 ms na worker.
    for (int i = 0; i < 50 && r->available() < 512; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(r->available() >= 512);
    // Precti par frames a over hodnoty.
    float L, R;
    REQUIRE(r->popFrame(L, R));
    CHECK(L == doctest::Approx(100.f / 8000.f).epsilon(0.001));
    se.releaseRing(r);
    se.stop();
    std::remove(p.c_str());
}
```

- [ ] **Step 2: Vytvor `engine/stream/stream_engine.h`**

```cpp
#pragma once
// engine/stream/stream_engine.h
// ----------------------------
// Background streamovaci engine: pool ring bufferu + worker thread(y), ktery
// plni ringy z WAV souboru. Audio thread cte vyhradne z RAM (ringu); disk
// I/O se nikdy nedeje na audio threadu.
//
// SPSC: kazdy ring je pouzivan JEDINYM voice (konzument = audio thread) a
// JEDINYM workerem (producent). Allocator (acquire/release) je lock-free
// pres atomic flag in_use_ na kazdem slotu.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace ithaca {

struct RingHandle {
    // Interleaved stereo float; kapacita = capacity_frames * 2 (alokovano pri start()).
    std::vector<float> buf;
    int capacity_frames = 0;
    std::atomic<size_t> w_{0};
    std::atomic<size_t> r_{0};
    std::atomic<bool>   eof_{false};
    std::atomic<bool>   in_use_{false};

    // Producent (worker): vrati pocet frames, ktere byly skutecne zapsany.
    // Pri plnem ringu zapise 0 (drop-on-full; ze workeru by se nemelo stat
    // jestli udrzuje refill rate).
    int push(const float* src, int n_frames);

    // Konzument (Voice): pop 1 frame. Vrati false kdyz prazdny.
    bool popFrame(float& L, float& R);

    // Konzument: kolik frames je k dispozici k cteni.
    int available() const;
};

struct StreamRequest {
    RingHandle* ring;
    std::string path;
    int   frame_off;
    int   n_frames;
    bool  eof_when_done = false;   // worker po dokonceni nastavi ring->eof_
};

class StreamEngine {
public:
    explicit StreamEngine(int n_rings = 32, int ring_capacity_frames = 8192);
    ~StreamEngine();

    void start();
    void stop();

    // Allocator (volat z audio threadu / engine threadu).
    RingHandle* acquireRing();
    void        releaseRing(RingHandle* r);

    // Naplnovac (volat z Voice po vypocteni headroom — pozn. fronta je drop-on-full).
    void requestRead(RingHandle* ring, const std::string& path,
                     int frame_off, int n_frames, bool eof_when_done = false);

private:
    void workerLoop();

    std::vector<RingHandle> rings_;
    // Fronta pozadavku: jednoduchy mutex+condvar je OK pro non-RT producenta
    // (Voice posila pozadavek z audio threadu via lock-free SPSC fronty?).
    // POZN.: aby audio thread nikdy nezamykal, pouzij MidiQueue-styl
    // lock-free SPSC frontu i tady. Pro jednoduchost prvni iterace:
    // mutex + atomic flag pro stop. (Pokud bude bottleneck, prepiseme.)
    // KOMENT: prvni iterace casto blbne kvuli RT-safety; zde pouzijeme
    // mutex protoze requestRead je volana po pop_lock-free notify only — DODELAT.
    std::vector<StreamRequest> req_q_;
    std::mutex                 q_mu_;
    std::condition_variable    q_cv_;
    std::atomic<bool>          run_{false};
    std::thread                worker_;
};

} // namespace ithaca
```

POZN: pro fazi 4 (prvni iterace) zde POUZIVAME mutex pro frontu pozadavku → znamena, ze
`requestRead` z audio threadu by zamykal, coz porusuje RT-safety. To je vedome odlozeno —
v Tasku 4b nize to vyresime SPSC lock-free frontou (stejne jako `midi_queue.h`). Prvni
iterace ma fungovat (vetsina testu hraje s 1 voice; pravdepodobnost konfliktu mala).
ALTERNATIVA: udelat SPSC frontu rovnou — DOPORUCENA, viz Task 4b.

> **DOPORUCENI implementerovi:** Misto prvni-iterace s mutexem implementuj rovnou
> lock-free SPSC frontu pro `StreamRequest` (vzor: `engine/midi/midi_queue.h`).
> Vyjmi `q_mu_/q_cv_` z `StreamEngine`; nahrad atomickou frontou `StreamRequestQueue`
> s `push()` (RT-safe drop-on-full) a `pop()`. Worker mezi pop-y SPI 1 ms (`std::this_thread::sleep_for`).
> Plan tomu sice rika "alternativa", ale implementer to udelej rovnou — uspora rizika je vetsi nez
> minimalni narust kodu.

- [ ] **Step 3: Implementuj `stream_engine.cpp`** s lock-free SPSC frontou per doporuceni vyse.
Specificke detaily (Push/pop ringu, requestRead, worker readWavRange + ring.push v cyklu, eof
priznak) ponechavam implementerovi — viz pattern v `midi_queue.h` a logika v komentari vyse.

- [ ] **Step 4: Integruj do `Voice`** — pridej `RingHandle* ring_` member; v `start()` kdyz
`mic_->mode == Streamed`: `ring_ = stream_engine.acquireRing()`; vystrelej prvni pozadavek na
`frame_off = head_frames, n = ring_capacity`. V `process()`:

```cpp
        int p0 = (int)position_;
        if (p0 < mic_->head_frames) {
            // Cti z preload_head — stavajici kod.
        } else if (ring_) {
            // Cti z ringu; udrzuj cursor `position_` jako globalni pozici v souboru.
            float L, R;
            if (!ring_->popFrame(L, R)) {
                if (ring_->eof_) { active_ = false; break; }
                // Underrun: spustime fast fade do ticha (~5 ms).
                startUnderrunFade();
                break;
            }
            // ... pouziti L,R + interpolace ...
        } else {
            // FullyLoaded ale za koncem head — to nemelo nastat, head_frames = file.frames.
            active_ = false; break;
        }

        // Periodicky pozadej refill kdyz je v ringu pod prahem.
        if (ring_ && ring_->available() < ring_->capacity_frames / 2) {
            int next_off = (int)position_ + ring_->available();
            int want = ring_->capacity_frames / 2;
            // Nezasle dvakrat tot same — drzime stream_request_pending_ flag.
            // (Detail implementerovi.)
        }
```

Po dohrani / note-off / steal: `if (ring_) stream_engine.releaseRing(ring_);`

POZN.: linearni interpolace pro pitch/SR konverzi (`p0`, `p1=p0+1`) je v Tasku 4 z ringu
zjednodusena na nearest-neighbor (cti `popFrame` jednou per frame). Linearka mezi preload_head
a ringem pak ma "skok" v hranici — to zatim akceptujeme; v dalsim tasku se to vyladi pomoci
1-frame "lookbehind" v ringu. Pro prvni iteraci je to OK.

- [ ] **Step 5: Pridej fast underrun fade do `Voice`** (linearni ramp ~5 ms; pak `active_=false`)
+ `LOG_RT_WARN("voice", "underrun midi=%d", midi_)`.

- [ ] **Step 6: Engine init** — pridej `StreamEngine stream_` jako member; v `init()` zavolej
`stream_.start()`, v destruktoru `stream_.stop()`. Pridej `cfg_.stream_threads` (default 1) a
`cfg_.ring_capacity_frames` (default 8192) do `EngineConfig`. Predej StreamEngine& do VoicePool.

- [ ] **Step 6b: Runtime audio buffer size + reload-rate skalovani s block size**
  Pozadavek uzivatele (2026-05-30): audio buffer (block size) ma jit menit za behu (jako icr/icr2
  selector). StreamEngine `refill_threshold` musi skalovat s block_size — vetsi block znamena vetsi
  spotrebu per audio tick, takze ring potrebuje plnit drive. Pravidlo:
  `refill_threshold_frames = max(ring_capacity_frames/2, block_size * 4)` — vzdy aspon 4 bloky
  napred, aby disk mel cas dohnat. AudioDevice z faze 3 uz block_size prijima v `start()`; pridej
  metodu `Engine::setBlockSize(int)` ktera (a) zastavi audio device, (b) `audio_device.stop()`,
  (c) `audio_device.start(playAudioCb, &ctx, sr, new_block_size)`, (d) prepocita refill threshold.
  Voice musi pri svem audio bloku poslat refill request kdyz `ring->available() < refill_threshold`.
  CLI: pridej `--block-size <N>` (default 256), nastav pred init. Faze 8 (GUI) ho exposne jako
  combo selector — viz spec [GUI 8.x].

- [ ] **Step 7: Napis integ test `tests/test_long_sample_stream.cpp`** — vytvor 1-sekundovy ramp
WAV, nacti pres loadLegacyBank s `preload_ms = 50` (→ head = 2400 frames, sampl 48000 → Streamed),
spust voice, renderuj v malych blocich (128 frames) az do konce samplu, over ze L hodnoty rostou
monotonne (ramp signal), ze `LOG_RT_WARN` nepadl, a ze konec je clean.

- [ ] **Step 8: CMake** — pridej `engine/stream/stream_engine.cpp` do `ithaca_core`; v tests
pridej oba testy.

- [ ] **Step 9: Build + cely ctest + --inspect/--render na realne bance**

```bash
cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build --output-on-failure
./build/ithaca-cli --render /Users/j/SoundBanks/Ithaca/as-blackgrand --out /tmp/test.wav
afplay /tmp/test.wav   # akord by mel znit cele, ne jen prvnich 150 ms
```
Expected: vsechny testy zelene; render zni cely (basy doznivaji), RAM mala.

- [ ] **Step 10: Commit**

```bash
git add engine/stream/stream_engine.h engine/stream/stream_engine.cpp \
        engine/voice/voice.h engine/voice/voice.cpp \
        engine/voice/voice_pool.h engine/voice/voice_pool.cpp \
        engine/engine.h engine/engine.cpp \
        tests/test_stream_engine.cpp tests/test_long_sample_stream.cpp \
        CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze4): streaming engine — ring buffer pool + worker thread + underrun fade

StreamEngine: N pre-alokovanych ringu, worker thread, lock-free SPSC fronta
pozadavku (vzor midi_queue). Voice cte z preload_head pokud cursor v hlave,
jinak z ringu; pri vycerpani ringu pred EOF spusti fast fade do ticha + WARN.
LoadLegacyBank uz nedrzi cele dlouhe samply v RAM. as-blackgrand RAM ~10x mensi.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: live overeni + drobne uklid + merge priprava

**Files:**
- Manualni testy + drobne fixy z review.

- [ ] **Step 1: Manualni overeni** `--play` na realne bance:

```bash
./build/ithaca-cli --play /Users/j/SoundBanks/Ithaca/as-blackgrand
```
Expected: load JE RYCHLEJSI nez ve fazi 3 (drasticky), RAM mensi, akord zni cele (basy doznivaji).
Zaloguj cas loadu a peak RAM.

- [ ] **Step 2: Sniz `cfg.midi_from`/`midi_to` v `--play`** zpet na 21..108 (jiz tak je) a over
ze rychlost je rozumna.

- [ ] **Step 3: Smoke + ctest po vsem**

```bash
make clean && make build && make test && make smoke
```

- [ ] **Step 4: Pokud vsechno OK** → merge phase 4:

```bash
git checkout main
git merge --no-ff faze4-streaming -m "$(cat <<'EOF'
Merge faze4-streaming: DFD streaming samplu z disku

Faze 4 hotova: MicLayer drzi jen preload regiony (~150 ms head + rezonancni
okno pripravene pro fazi 5); dlouhe samply se streamuji z disku pres pool
ring bufferu + worker thread. Audio thread cte vyhradne z RAM. Underrun ->
fast fade + WARN. as-blackgrand: load ~Xs (z 44s), RAM ~XMB (z 5392MB).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
git branch -d faze4-streaming
```

---

## Hotovo — kriteria dokonceni faze 4

- `make build` cisty, `make test` 100% (faze 1-3 testy + nove test_wav_range, test_stream_engine,
  test_long_sample_stream).
- `--inspect` na as-blackgrand: drasticky mensi RAM (radove desitky MB misto 5392 MB).
- `--render` na as-blackgrand: noty hraji CELE (basy doznivaji do EOF), zadny underrun.
- `--play` na as-blackgrand: znatelne rychlejsi load nez ve fazi 3; akord zni cele bez cvaknuti.
- StreamEngine je SPSC lock-free na fronte pozadavku; audio thread nikdy nezamyka, nikdy nesaha
  na disk; underrun jen fade + RT log (ne crash, ne pomalost).

Tim banka uz nezere RAM. Faze 5 (pedal + sympaticka rezonance) zacne pouzivat `preload_resonance`
region. Faze 6 (DSP chain + mic mixer) a 7 (extended format) nasleduji per puvodni plan.
