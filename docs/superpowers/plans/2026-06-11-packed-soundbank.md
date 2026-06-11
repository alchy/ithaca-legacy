# Pakovaná soundbanka `soundbank.ithaca` — implementační plán

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Jednosouborová pakovaná banka `soundbank.ithaca` (v1 bez šifry): blob doslovných WAVů + index s předpočítanou analýzou, abstrakce čtení (pread), CLI dump analýzy a python bake nástroj.

**Architecture:** Blob = zřetězené WAVy zarovnané na 4 KB; 408B hlavička + 64B/záznam index (předřazený dle midi, rms). C++ čte přes `IFileHandle::readAt` (pread, bez sdíleného kurzoru) a dispatcher `readSampleRange(SampleFile&,…)`, který deleguje buď na stávající `readWavRange`, nebo dekóduje z blobu. Loader má novou větev `BankFormat::PackedIthaca` (žádný scan/analýza/sort). Python `bake_soundbank.py` přebírá analýzu z nového CLI příkazu `--dump-bank-index`.

**Tech Stack:** C++20 (CMake, doctest), python3 stdlib only. Spec: `docs/superpowers/specs/2026-06-10-packed-soundbank-design.md`. Větev: `feat/packed-soundbank`.

**Build/test příkazy:**
- build: `make build` (nebo `cmake --build build --parallel 8`)
- všechny testy: `make test`
- jeden test: `cmake --build build --parallel 8 && ./build/tests/<nazev>`

---

## Struktura souborů

| Soubor | Akce | Zodpovědnost |
|---|---|---|
| `engine/util/sha256.{h,cpp}` | nový | SHA-256 (FIPS 180-4), streaming API |
| `engine/io/file_handle.{h,cpp}` | nový | `IFileHandle` (pread) + `openFileHandle()` |
| `engine/io/wav_reader.{h,cpp}` | úprava | expose `wavSampleToFloat()` (dnes statická `sampleToFloat`) |
| `engine/sample/ithaca_format.{h,cpp}` | nový | konstanty formátu, PODy, čisté parsery nad byte bufferem |
| `engine/sample/ithaca_bank.{h,cpp}` | nový | `openIthacaBank()`: I/O + validace (magic, verze, hash, rozsahy) |
| `engine/io/sample_read.{h,cpp}` | nový | dispatcher `readSampleRange()` (WAV cesta vs. blob) |
| `engine/sample/sample_types.h` | úprava | `BankFormat::PackedIthaca`, `SampleFile` lokátor (blob, pcm_offset, …) |
| `engine/sample/bank_index.cpp` | úprava | scanBank stupeň 0: detekce `soundbank.ithaca` |
| `engine/sample/sample_store.cpp` | úprava | packed větev `loadBank` + `readSampleRange` v preload/rezo cache |
| `engine/stream/stream_engine.{h,cpp}` | úprava | `StreamRequest.file` (SampleFile), worker čte přes dispatcher |
| `engine/stream/streamed_reader.{h,cpp}` | úprava | `begin/refill` berou `const SampleFile&` |
| `engine/voice/voice.cpp`, `engine/voice/resonance_voice.cpp` | úprava | předávají `mic_->file` místo `mic_->file.path` |
| `app/cli/main.cpp` | úprava | `--dump-bank-index <dir> --out <json> [--preload-ms N]` |
| `tools/bake_soundbank.py` | nový | bake: dump → RIFF parse → zápis .ithaca (+ `--verify`) |
| `tests/test_sha256.cpp`, `tests/test_file_handle.cpp`, `tests/test_ithaca_format.cpp`, `tests/test_ithaca_bank.cpp`, `tests/test_sample_read.cpp`, `tests/test_packed_bank_load.cpp`, `tests/test_packed_stream.cpp` | nové | unit testy |
| `tests/ithaca_test_blob.h` | nový | helper: postaví malý validní .ithaca v temp souboru |
| `tests/test_bake_soundbank.py` | nový | python unit testy bake (bez CLI závislosti) |
| `tests/roundtrip_packed_bank.sh` | nový | end-to-end: bake reálnou CLI + porovnání dump výstupů |
| `tests/CMakeLists.txt`, `CMakeLists.txt` | úprava | registrace nových testů a zdrojáků |
| `docs/bank-format-packed.md` | nový | popis formátu |
| `docs/reference/F-loader.md`, `docs/config-file.md` | úprava | packed load path, zmínka u bank_path |

Pozn. ke konvencím: komentáře v kódu česky bez diakritiky (jako zbytek enginu), docs s diakritikou. Commity stylem `feat(...): ...` malými písmeny bez diakritiky.

---

### Task 1: SHA-256 utilita

**Files:**
- Create: `engine/util/sha256.h`, `engine/util/sha256.cpp`
- Test: `tests/test_sha256.cpp`
- Modify: `CMakeLists.txt` (přidat `engine/util/sha256.cpp` do `ithaca_core`), `tests/CMakeLists.txt`

- [ ] **Step 1: Napiš failing test**

`tests/test_sha256.cpp`:

```cpp
// tests/test_sha256.cpp
// SHA-256 proti znamym NIST vektorum (FIPS 180-4).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "util/sha256.h"

#include <cstdio>
#include <string>

using namespace ithaca;

namespace {
std::string hex(const std::array<uint8_t, 32>& d) {
    char out[65];
    for (int i = 0; i < 32; ++i) std::snprintf(out + i * 2, 3, "%02x", d[i]);
    return std::string(out, 64);
}
} // namespace

TEST_CASE("sha256 prazdny vstup") {
    CHECK(hex(Sha256::hash("", 0)) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("sha256 'abc'") {
    CHECK(hex(Sha256::hash("abc", 3)) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256 dvoublokova zprava") {
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hex(Sha256::hash(m, 56)) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256 streaming update == one-shot") {
    Sha256 s;
    s.update("ab", 2);
    s.update("c", 1);
    CHECK(hex(s.finish()) == hex(Sha256::hash("abc", 3)));
}
```

- [ ] **Step 2: Zaregistruj test a ověř, že selže (nekompiluje — chybí header)**

Do `tests/CMakeLists.txt` přidej na konec:

```cmake
add_executable(test_sha256 test_sha256.cpp)
target_link_libraries(test_sha256 PRIVATE ithaca_core doctest)
add_test(NAME test_sha256 COMMAND test_sha256)
```

Run: `cmake --build build --parallel 8`
Expected: FAIL — `util/sha256.h: No such file or directory`

- [ ] **Step 3: Implementuj**

`engine/util/sha256.h`:

```cpp
#pragma once
// engine/util/sha256.h
// Minimalni SHA-256 (FIPS 180-4) pro integritu .ithaca banky. Zadne externi
// zavislosti; streaming update/finish (hash indexu se pocita po sekcich).

#include <array>
#include <cstddef>
#include <cstdint>

namespace ithaca {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    std::array<uint8_t, 32> finish();   // po finish() nutny reset()

    static std::array<uint8_t, 32> hash(const void* data, size_t len) {
        Sha256 s; s.update(data, len); return s.finish();
    }

private:
    void processBlock(const uint8_t* p);
    uint32_t h_[8];
    uint64_t total_   = 0;   // bajty celkem
    uint8_t  buf_[64];
    size_t   buf_len_ = 0;
};

} // namespace ithaca
```

`engine/util/sha256.cpp`:

```cpp
// engine/util/sha256.cpp — viz sha256.h. Ucebnicova implementace FIPS 180-4.
#include "util/sha256.h"

#include <cstring>

namespace ithaca {

namespace {
constexpr uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
} // namespace

void Sha256::reset() {
    h_[0]=0x6a09e667; h_[1]=0xbb67ae85; h_[2]=0x3c6ef372; h_[3]=0xa54ff53a;
    h_[4]=0x510e527f; h_[5]=0x9b05688c; h_[6]=0x1f83d9ab; h_[7]=0x5be0cd19;
    total_ = 0; buf_len_ = 0;
}

void Sha256::processBlock(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
               (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],h=h_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d;
    h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=h;
}

void Sha256::update(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    total_ += len;
    if (buf_len_ > 0) {
        size_t take = (len < 64 - buf_len_) ? len : 64 - buf_len_;
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take; p += take; len -= take;
        if (buf_len_ == 64) { processBlock(buf_); buf_len_ = 0; }
    }
    while (len >= 64) { processBlock(p); p += 64; len -= 64; }
    if (len > 0) { std::memcpy(buf_, p, len); buf_len_ = len; }
}

std::array<uint8_t, 32> Sha256::finish() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (buf_len_ != 56) update(&zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    // update() by pricetl delku paddingu do total_ — to uz nevadi, bits mame.
    std::memcpy(buf_ + 56, lenb, 8);
    processBlock(buf_);
    buf_len_ = 0;
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = (uint8_t)(h_[i] >> 24);
        out[i*4+1] = (uint8_t)(h_[i] >> 16);
        out[i*4+2] = (uint8_t)(h_[i] >> 8);
        out[i*4+3] = (uint8_t)(h_[i]);
    }
    return out;
}

} // namespace ithaca
```

Do `CMakeLists.txt` přidej `engine/util/sha256.cpp` do seznamu zdrojáků `ithaca_core` (za `engine/util/rt_priority.cpp`).

- [ ] **Step 4: Ověř, že testy projdou**

Run: `cmake --build build --parallel 8 && ./build/tests/test_sha256`
Expected: `[doctest] ... 4 test cases ... all passed`

- [ ] **Step 5: Commit**

```bash
git add engine/util/sha256.h engine/util/sha256.cpp tests/test_sha256.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(util): sha256 pro integritu pakovane banky"
```

---

### Task 2: IFileHandle — pozicované čtení (pread)

**Files:**
- Create: `engine/io/file_handle.h`, `engine/io/file_handle.cpp`
- Test: `tests/test_file_handle.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Napiš failing test**

`tests/test_file_handle.cpp`:

```cpp
// tests/test_file_handle.cpp
// IFileHandle: pozicovane cteni (pread) bez sdileneho kurzoru.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "io/file_handle.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ithaca;

namespace {
struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { if (!path.empty()) std::remove(path.c_str()); }
};

// Vyrobi temp soubor s bajty 0..255 opakovane (1024 B).
std::string makePatternFile() {
    std::string p = "/tmp/ithaca_fh_pattern.bin";
    std::FILE* f = std::fopen(p.c_str(), "wb");
    REQUIRE(f != nullptr);
    for (int i = 0; i < 1024; ++i) { uint8_t b = (uint8_t)(i & 0xFF); std::fwrite(&b, 1, 1, f); }
    std::fclose(f);
    return p;
}
} // namespace

TEST_CASE("openFileHandle + size + readAt presne") {
    TempFile p{ makePatternFile() };
    auto h = openFileHandle(p.path);
    REQUIRE(h != nullptr);
    CHECK(h->size() == 1024u);
    uint8_t buf[16];
    REQUIRE(h->readAt(256, buf, 16));
    for (int i = 0; i < 16; ++i) CHECK(buf[i] == (uint8_t)(i & 0xFF));
}

TEST_CASE("readAt pres konec souboru → false") {
    TempFile p{ makePatternFile() };
    auto h = openFileHandle(p.path);
    REQUIRE(h != nullptr);
    uint8_t buf[16];
    CHECK_FALSE(h->readAt(1020, buf, 16));   // jen 4 B dostupne → kratke cteni = chyba
}

TEST_CASE("openFileHandle na neexistujicim souboru → nullptr") {
    CHECK(openFileHandle("/tmp/ithaca_no_such_file_fh.bin") == nullptr);
}
```

Registrace v `tests/CMakeLists.txt`:

```cmake
add_executable(test_file_handle test_file_handle.cpp)
target_link_libraries(test_file_handle PRIVATE ithaca_core doctest)
add_test(NAME test_file_handle COMMAND test_file_handle)
```

- [ ] **Step 2: Ověř fail**

Run: `cmake --build build --parallel 8`
Expected: FAIL — `io/file_handle.h: No such file or directory`

- [ ] **Step 3: Implementuj**

`engine/io/file_handle.h`:

```cpp
#pragma once
// engine/io/file_handle.h
// Bezstavove pozicovane cteni (pread) — zadny sdileny seek kurzor, takze
// paralelni loader workery i streaming worker ctou TYZ handle bez zamku.
// Pouziva pakovana banka (.ithaca blob); bezne WAV cesty ctou dal pres
// wav_reader (fopen per call).

#include <cstdint>
#include <memory>
#include <string>

namespace ithaca {

struct IFileHandle {
    virtual ~IFileHandle() = default;
    // Precte PRESNE n bajtu z absolutniho offsetu. false = kratke cteni nebo
    // chyba (volajici zna velikosti z indexu — kratke cteni je vzdy chyba).
    virtual bool readAt(uint64_t off, void* buf, size_t n) const = 0;
    virtual uint64_t size() const = 0;
};

// Otevre soubor pro pread (sdileny mezi vlakny). nullptr pri chybe.
std::shared_ptr<IFileHandle> openFileHandle(const std::string& path);

} // namespace ithaca
```

`engine/io/file_handle.cpp`:

```cpp
// engine/io/file_handle.cpp — viz file_handle.h.
#include "io/file_handle.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ithaca {
namespace {

#if defined(_WIN32)
// Win32: ReadFile se sync handle + OVERLAPPED offsetem = pozicovane cteni
// bez posunu file pointeru (ekvivalent pread).
class Win32FileHandle : public IFileHandle {
public:
    Win32FileHandle(HANDLE h, uint64_t size) : h_(h), size_(size) {}
    ~Win32FileHandle() override { CloseHandle(h_); }
    bool readAt(uint64_t off, void* buf, size_t n) const override {
        uint8_t* p = static_cast<uint8_t*>(buf);
        while (n > 0) {
            OVERLAPPED ov{};
            ov.Offset     = (DWORD)(off & 0xFFFFFFFFu);
            ov.OffsetHigh = (DWORD)(off >> 32);
            DWORD chunk = (n > 0x40000000u) ? 0x40000000u : (DWORD)n;
            DWORD got = 0;
            if (!ReadFile(h_, p, chunk, &got, &ov) || got == 0) return false;
            p += got; off += got; n -= got;
        }
        return true;
    }
    uint64_t size() const override { return size_; }
private:
    HANDLE   h_;
    uint64_t size_;
};
#else
class PosixFileHandle : public IFileHandle {
public:
    PosixFileHandle(int fd, uint64_t size) : fd_(fd), size_(size) {}
    ~PosixFileHandle() override { ::close(fd_); }
    bool readAt(uint64_t off, void* buf, size_t n) const override {
        uint8_t* p = static_cast<uint8_t*>(buf);
        while (n > 0) {
            ssize_t got = ::pread(fd_, p, n, (off_t)off);
            if (got <= 0) return false;   // 0 = EOF driv nez n bajtu
            p += got; off += (uint64_t)got; n -= (size_t)got;
        }
        return true;
    }
    uint64_t size() const override { return size_; }
private:
    int      fd_;
    uint64_t size_;
};
#endif

} // namespace

std::shared_ptr<IFileHandle> openFileHandle(const std::string& path) {
#if defined(_WIN32)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return nullptr; }
    return std::make_shared<Win32FileHandle>(h, (uint64_t)sz.QuadPart);
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;
    struct stat st{};
    if (::fstat(fd, &st) != 0) { ::close(fd); return nullptr; }
    return std::make_shared<PosixFileHandle>(fd, (uint64_t)st.st_size);
#endif
}

} // namespace ithaca
```

Do `CMakeLists.txt` přidej `engine/io/file_handle.cpp` (za `engine/io/wav_writer.cpp`).

- [ ] **Step 4: Ověř pass**

Run: `cmake --build build --parallel 8 && ./build/tests/test_file_handle`
Expected: all passed

- [ ] **Step 5: Commit**

```bash
git add engine/io/file_handle.h engine/io/file_handle.cpp tests/test_file_handle.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(io): IFileHandle — pozicovane cteni pro pakovanou banku"
```

---

### Task 3: Expose `wavSampleToFloat` z wav_readeru

Dispatcher (Task 6) musí dekódovat blob bit-exact stejně jako WAV čtení — sdílí se konverzní funkce, dnes statická `sampleToFloat` v anonymním namespace `wav_reader.cpp`.

**Files:**
- Modify: `engine/io/wav_reader.h`, `engine/io/wav_reader.cpp`
- Test: rozšíření `tests/test_wav_reader.cpp` (přidat TEST_CASE na konec)

- [ ] **Step 1: Napiš failing test**

Na konec `tests/test_wav_reader.cpp` přidej:

```cpp
TEST_CASE("wavSampleToFloat zname hodnoty") {
    // PCM16 max/min/nula
    { int16_t v = 32767;  CHECK(wavSampleToFloat((const uint8_t*)&v, 16, 1) == doctest::Approx(32767.f/32768.f)); }
    { int16_t v = -32768; CHECK(wavSampleToFloat((const uint8_t*)&v, 16, 1) == doctest::Approx(-1.f)); }
    { int16_t v = 0;      CHECK(wavSampleToFloat((const uint8_t*)&v, 16, 1) == 0.f); }
    // PCM24 -1.0 (0x800000 sign-extend)
    { uint8_t b[3] = {0x00, 0x00, 0x80}; CHECK(wavSampleToFloat(b, 24, 1) == doctest::Approx(-1.f)); }
    // float32 pass-through
    { float v = 0.5f; CHECK(wavSampleToFloat((const uint8_t*)&v, 32, 3) == 0.5f); }
}
```

- [ ] **Step 2: Ověř fail**

Run: `cmake --build build --parallel 8`
Expected: FAIL — `wavSampleToFloat` not declared

- [ ] **Step 3: Implementuj**

V `engine/io/wav_reader.h` přidej před `} // namespace ithaca`:

```cpp
// Prevod jednoho suroveho vzorku na float [-1,1] dle formatu (bits 16/24/32,
// audio_format 1=PCM / 3=IEEE float). Sdileno s pakovanou bankou — dekodovani
// blobu (sample_read.cpp) je tim bit-exact shodne s WAV ctenim.
float wavSampleToFloat(const uint8_t* p, uint16_t bits, uint16_t audio_format);
```

V `engine/io/wav_reader.cpp`: funkci `sampleToFloat` přesuň VEN z anonymního namespace (nech ji na stejném místě v souboru, jen ji přejmenuj a uzavři anonymní namespace před ní), přejmenuj na `wavSampleToFloat` a uprav obě volání (`readWav`, `readWavRange`) na nové jméno. Tělo funkce beze změny.

- [ ] **Step 4: Ověř pass**

Run: `cmake --build build --parallel 8 && ./build/tests/test_wav_reader && ./build/tests/test_wav_range`
Expected: all passed (včetně stávajících testů — chování beze změny)

- [ ] **Step 5: Commit**

```bash
git add engine/io/wav_reader.h engine/io/wav_reader.cpp tests/test_wav_reader.cpp
git commit -m "refactor(io): expose wavSampleToFloat pro sdileni s blob dekoderem"
```

---

### Task 4: Formát .ithaca — konstanty a čisté parsery

**Files:**
- Create: `engine/sample/ithaca_format.h`, `engine/sample/ithaca_format.cpp`
- Test: `tests/test_ithaca_format.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Napiš failing test**

`tests/test_ithaca_format.cpp`:

```cpp
// tests/test_ithaca_format.cpp
// Ciste parsery hlavicky a indexu .ithaca nad byte bufferem (bez disku).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "sample/ithaca_format.h"

#include <cstring>
#include <vector>

using namespace ithaca;

namespace {
// Sestavi validni 408B hlavicku do bufferu (LE; pole dle spec sekce 1).
std::vector<uint8_t> makeHeaderBytes() {
    std::vector<uint8_t> b(kIthacaHeaderSize, 0);
    std::memcpy(b.data(), kIthacaMagic, 8);
    auto put32 = [&](size_t off, uint32_t v) { std::memcpy(b.data()+off, &v, 4); };
    auto put64 = [&](size_t off, uint64_t v) { std::memcpy(b.data()+off, &v, 8); };
    put32(8,  kIthacaVersion);
    put32(12, 0);                  // flags
    put64(16, 408); put64(24, 50);     // metadata
    put64(32, 458); put64(40, 64);     // index (1 zaznam)
    put64(48, 522); put64(56, 10);     // names
    put64(64, 4096); put64(72, 8192);  // blob
    put32(80, 1);                  // entry_count
    // sha pole nechavame nulova — parser je NEoveruje (to dela openIthacaBank)
    return b;
}

// Sestavi jeden 64B index zaznam.
std::vector<uint8_t> makeEntryBytes() {
    std::vector<uint8_t> b(kIthacaEntrySize, 0);
    auto put16 = [&](size_t off, uint16_t v) { std::memcpy(b.data()+off, &v, 2); };
    auto put32 = [&](size_t off, uint32_t v) { std::memcpy(b.data()+off, &v, 4); };
    auto put64 = [&](size_t off, uint64_t v) { std::memcpy(b.data()+off, &v, 8); };
    put16(0, 60);            // midi
    put16(2, 2);             // channels
    put32(4, 48000);         // sample_rate
    put64(8, 4096);          // entry_offset
    put64(16, 2048);         // entry_size
    put32(24, 44);           // pcm_data_offset
    put16(28, kSampleFmtPcm16);
    int64_t frames = 501; std::memcpy(b.data()+32, &frames, 8);
    float rms = -23.5f;   std::memcpy(b.data()+40, &rms, 4);
    put32(44, 1234);         // attack_end
    put32(48, kIthacaNoName);
    return b;
}
} // namespace

TEST_CASE("parseIthacaHeader valid") {
    auto b = makeHeaderBytes();
    IthacaHeader h;
    REQUIRE(parseIthacaHeader(b.data(), b.size(), h));
    CHECK(h.version == 1);
    CHECK(h.flags == 0);
    CHECK(h.metadata_offset == 408u); CHECK(h.metadata_size == 50u);
    CHECK(h.index_offset == 458u);    CHECK(h.index_size == 64u);
    CHECK(h.names_offset == 522u);    CHECK(h.names_size == 10u);
    CHECK(h.blob_offset == 4096u);    CHECK(h.blob_size == 8192u);
    CHECK(h.entry_count == 1u);
}

TEST_CASE("parseIthacaHeader spatny magic / kratky buffer") {
    auto b = makeHeaderBytes();
    b[0] = 'X';
    IthacaHeader h;
    CHECK_FALSE(parseIthacaHeader(b.data(), b.size(), h));
    auto ok = makeHeaderBytes();
    CHECK_FALSE(parseIthacaHeader(ok.data(), 100, h));   // kratsi nez 408
}

TEST_CASE("parseIthacaIndex valid zaznam") {
    auto b = makeEntryBytes();
    std::vector<IthacaEntry> es;
    REQUIRE(parseIthacaIndex(b.data(), b.size(), 1, es));
    REQUIRE(es.size() == 1u);
    CHECK(es[0].midi == 60);
    CHECK(es[0].channels == 2);
    CHECK(es[0].sample_rate == 48000u);
    CHECK(es[0].entry_offset == 4096u);
    CHECK(es[0].entry_size == 2048u);
    CHECK(es[0].pcm_data_offset == 44u);
    CHECK(es[0].sample_format == kSampleFmtPcm16);
    CHECK(es[0].frames == 501);
    CHECK(es[0].rms_db == doctest::Approx(-23.5f));
    CHECK(es[0].attack_end == 1234u);
    CHECK(es[0].name_offset == kIthacaNoName);
}

TEST_CASE("parseIthacaIndex spatna velikost bufferu") {
    auto b = makeEntryBytes();
    std::vector<IthacaEntry> es;
    CHECK_FALSE(parseIthacaIndex(b.data(), b.size() - 1, 1, es));
    CHECK_FALSE(parseIthacaIndex(b.data(), b.size(), 2, es));   // count nesedi
}
```

Registrace v `tests/CMakeLists.txt`:

```cmake
add_executable(test_ithaca_format test_ithaca_format.cpp)
target_link_libraries(test_ithaca_format PRIVATE ithaca_core doctest)
add_test(NAME test_ithaca_format COMMAND test_ithaca_format)
```

- [ ] **Step 2: Ověř fail**

Run: `cmake --build build --parallel 8`
Expected: FAIL — `sample/ithaca_format.h: No such file`

- [ ] **Step 3: Implementuj**

`engine/sample/ithaca_format.h`:

```cpp
#pragma once
// engine/sample/ithaca_format.h
// Binarni format pakovane banky soundbank.ithaca (little-endian) — konstanty,
// POD struktury a CISTE parsovaci funkce nad byte bufferem (testovatelne bez
// disku). Layout: docs/bank-format-packed.md; rozhodnuti: spec
// docs/superpowers/specs/2026-06-10-packed-soundbank-design.md.
// Soubor: [hlavicka 408 B][metadata JSON][index 64 B/zaznam][names][blob].

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ithaca {

inline constexpr char     kIthacaMagic[8]   = {'I','T','H','A','C','A','B','K'};
inline constexpr uint32_t kIthacaVersion    = 1;
inline constexpr size_t   kIthacaHeaderSize = 408;
inline constexpr size_t   kIthacaEntrySize  = 64;
inline constexpr uint32_t kIthacaNoName     = 0xFFFFFFFFu;
inline constexpr uint64_t kIthacaBlobAlign  = 4096;
inline constexpr const char* kIthacaFileName = "soundbank.ithaca";

// flags bity (v1 musi byt 0 — rezervovano pro v2 sifru/podpis):
inline constexpr uint32_t kIthacaFlagEncrypted = 1u << 0;
inline constexpr uint32_t kIthacaFlagSigned    = 1u << 1;

// sample_format kody (pokryvaji formaty wav_readeru):
inline constexpr uint16_t kSampleFmtPcm16   = 1;
inline constexpr uint16_t kSampleFmtPcm24   = 2;
inline constexpr uint16_t kSampleFmtFloat32 = 3;
inline constexpr uint16_t kSampleFmtPcm32   = 4;

struct IthacaHeader {
    uint32_t version = 0;
    uint32_t flags   = 0;
    uint64_t metadata_offset = 0, metadata_size = 0;
    uint64_t index_offset = 0,    index_size = 0;
    uint64_t names_offset = 0,    names_size = 0;
    uint64_t blob_offset = 0,     blob_size = 0;
    uint32_t entry_count = 0;
    std::array<uint8_t, 32> sha256_index{};     // pres metadata+index+names
    std::array<uint8_t, 32> sha256_payload{};   // pres blob (jen --verify)
};

struct IthacaEntry {
    uint16_t midi = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint64_t entry_offset = 0;      // absolutni offset WAV souboru v .ithaca
    uint64_t entry_size = 0;        // delka WAV souboru v bajtech
    uint32_t pcm_data_offset = 0;   // relativni k entry_offset (prvni vzorek)
    uint16_t sample_format = 0;     // kSampleFmt*
    int64_t  frames = 0;
    float    rms_db = 0.f;          // autoritativni (poradi vrstev)
    uint32_t attack_end = 0;        // frame index v analyzovanem preload oknu
    uint32_t name_offset = kIthacaNoName;
};

// Parsuje 408B hlavicku. false = kratky buffer nebo spatny magic. Verzi,
// flagy a rozsahy NEKONTROLUJE — to dela openIthacaBank (rozlisene hlasky).
bool parseIthacaHeader(const uint8_t* buf, size_t n, IthacaHeader& out);

// Parsuje pole 64B zaznamu; n musi byt presne entry_count * 64.
bool parseIthacaIndex(const uint8_t* buf, size_t n, uint32_t entry_count,
                      std::vector<IthacaEntry>& out);

} // namespace ithaca
```

`engine/sample/ithaca_format.cpp`:

```cpp
// engine/sample/ithaca_format.cpp — viz ithaca_format.h.
#include "sample/ithaca_format.h"

#include <cstring>

namespace ithaca {

namespace {
// LE cteni pres memcpy. Pozn.: vsechny cilove platformy jsou LE (stejna
// konvence jako wav_reader.cpp); big-endian port by zde byte-swapoval.
template <typename T>
T rdLE(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }
} // namespace

bool parseIthacaHeader(const uint8_t* buf, size_t n, IthacaHeader& out) {
    if (n < kIthacaHeaderSize) return false;
    if (std::memcmp(buf, kIthacaMagic, 8) != 0) return false;
    out.version         = rdLE<uint32_t>(buf + 8);
    out.flags           = rdLE<uint32_t>(buf + 12);
    out.metadata_offset = rdLE<uint64_t>(buf + 16);
    out.metadata_size   = rdLE<uint64_t>(buf + 24);
    out.index_offset    = rdLE<uint64_t>(buf + 32);
    out.index_size      = rdLE<uint64_t>(buf + 40);
    out.names_offset    = rdLE<uint64_t>(buf + 48);
    out.names_size      = rdLE<uint64_t>(buf + 56);
    out.blob_offset     = rdLE<uint64_t>(buf + 64);
    out.blob_size       = rdLE<uint64_t>(buf + 72);
    out.entry_count     = rdLE<uint32_t>(buf + 80);
    std::memcpy(out.sha256_index.data(),   buf + 88,  32);
    std::memcpy(out.sha256_payload.data(), buf + 120, 32);
    return true;
}

bool parseIthacaIndex(const uint8_t* buf, size_t n, uint32_t entry_count,
                      std::vector<IthacaEntry>& out) {
    if (n != (size_t)entry_count * kIthacaEntrySize) return false;
    out.clear();
    out.reserve(entry_count);
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint8_t* p = buf + (size_t)i * kIthacaEntrySize;
        IthacaEntry e;
        e.midi            = rdLE<uint16_t>(p + 0);
        e.channels        = rdLE<uint16_t>(p + 2);
        e.sample_rate     = rdLE<uint32_t>(p + 4);
        e.entry_offset    = rdLE<uint64_t>(p + 8);
        e.entry_size      = rdLE<uint64_t>(p + 16);
        e.pcm_data_offset = rdLE<uint32_t>(p + 24);
        e.sample_format   = rdLE<uint16_t>(p + 28);
        e.frames          = rdLE<int64_t> (p + 32);
        e.rms_db          = rdLE<float>   (p + 40);
        e.attack_end      = rdLE<uint32_t>(p + 44);
        e.name_offset     = rdLE<uint32_t>(p + 48);
        out.push_back(e);
    }
    return true;
}

} // namespace ithaca
```

Do `CMakeLists.txt` přidej `engine/sample/ithaca_format.cpp` (za `engine/sample/bank_index.cpp`).

- [ ] **Step 4: Ověř pass**

Run: `cmake --build build --parallel 8 && ./build/tests/test_ithaca_format`
Expected: all passed

- [ ] **Step 5: Commit**

```bash
git add engine/sample/ithaca_format.h engine/sample/ithaca_format.cpp tests/test_ithaca_format.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(sample): format soundbank.ithaca — konstanty a parsery"
```

---

### Task 5: Test-blob helper + openIthacaBank (I/O + validace)

**Files:**
- Create: `tests/ithaca_test_blob.h` (helper pro testy), `engine/sample/ithaca_bank.h`, `engine/sample/ithaca_bank.cpp`
- Test: `tests/test_ithaca_bank.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Napiš test helper**

`tests/ithaca_test_blob.h` — sestaví malou validní `.ithaca` banku v temp souboru. Vnitřní WAVy generuje přes `writeWavStereo16` do temp souborů a vkládá je doslovně (verbatim). Používají ho Tasky 5–8.

```cpp
#pragma once
// tests/ithaca_test_blob.h
// Helper: postavi maly validni soundbank.ithaca v temp adresari. Vnitrni
// WAVy = ramp signal pres writeWavStereo16 (PCM16 stereo). Vraci cesty +
// ocekavane hodnoty pro asserty. Pouzivaji testy ithaca_bank / sample_read /
// packed_bank_load / packed_stream.

#include "io/wav_reader.h"
#include "io/wav_writer.h"
#include "sample/ithaca_format.h"
#include "util/sha256.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ithaca_test {

struct BlobSpec {
    int      midi;
    int      frames;
    int      sample_rate;
    float    rms_db;        // bake hodnota (testy ji jen prenaseji)
    uint32_t attack_end;
};

struct BuiltBlob {
    std::string dir;            // temp adresar banky
    std::string ithaca_path;    // dir + "/soundbank.ithaca"
    std::vector<ithaca::IthacaEntry> entries;   // co bylo zapsano
    // Ramp data k porovnani: pro kazdy zaznam plne interleaved stereo float
    // tak, jak je vrati readWav (PCM16 kvantovane).
    std::vector<std::vector<float>> expected_samples;
};

// Ramp signal jako v tests/test_wav_range.cpp: L[i]=i/N, R[i]=-i/N.
inline std::vector<float> rampSamples(int frames) {
    std::vector<float> s((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        s[(size_t)i*2]   =  (float)i / (float)frames;
        s[(size_t)i*2+1] = -(float)i / (float)frames;
    }
    return s;
}

// Najde offset zacatku PCM dat v hotovem WAV souboru (po RIFF hlavicce):
// hleda "data" chunk stejnym pruchodem jako wav_reader::parseHeader.
inline uint32_t findDataOffset(const std::vector<uint8_t>& wav) {
    size_t pos = 12;   // RIFF(4) + size(4) + WAVE(4)
    while (pos + 8 <= wav.size()) {
        uint32_t sz; std::memcpy(&sz, wav.data() + pos + 4, 4);
        if (std::memcmp(wav.data() + pos, "data", 4) == 0)
            return (uint32_t)(pos + 8);
        pos += 8 + sz + (sz & 1u);
    }
    return 0;
}

inline std::vector<uint8_t> readFileBytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

// Postavi .ithaca z danych specu. Zaznamy zapise v poradi specu (testy je
// predavaji uz serazene dle (midi, rms)). corrupt_*: zamerne poskozeni pro
// negativni testy.
inline BuiltBlob buildTestIthaca(const char* tag,
                                 const std::vector<BlobSpec>& specs,
                                 bool corrupt_index_hash = false,
                                 uint32_t force_version = ithaca::kIthacaVersion,
                                 uint32_t force_flags = 0) {
    namespace fs = std::filesystem;
    using namespace ithaca;
    BuiltBlob out;
    out.dir = std::string("/tmp/ithaca_blob_") + tag;
    fs::remove_all(out.dir);
    fs::create_directories(out.dir);
    out.ithaca_path = out.dir + "/" + kIthacaFileName;

    // 1) Vyrob vnitrni WAVy (temp) a nacti jejich bajty.
    std::vector<std::vector<uint8_t>> wavs;
    for (size_t i = 0; i < specs.size(); ++i) {
        std::string wp = out.dir + "/tmp_" + std::to_string(i) + ".wav";
        auto samples = rampSamples(specs[i].frames);
        writeWavStereo16(wp, samples, specs[i].sample_rate);
        wavs.push_back(readFileBytes(wp));
        // Ocekavana data = round-trip pres NAS wav_reader (zadna rucni
        // kvantizace — garantovana shoda s tim, co cte loader).
        ithaca::WavData rd = ithaca::readWav(wp);
        out.expected_samples.push_back(std::move(rd.samples));
        std::remove(wp.c_str());
    }

    // 2) Sekce: metadata JSON, index, names, blob (4K align).
    std::string metadata = "{\"bank_name\":\"test\",\"created_at\":\"2026-06-11\","
        "\"bake_tool_version\":\"test\",\"analysis_preload_ms\":150,"
        "\"source_format\":\"dynamic\"}";
    const uint64_t metadata_offset = kIthacaHeaderSize;
    const uint64_t index_offset    = metadata_offset + metadata.size();
    const uint64_t index_size      = specs.size() * kIthacaEntrySize;
    const uint64_t names_offset    = index_offset + index_size;
    const uint64_t names_size      = 0;     // testy jmena nepotrebuji
    uint64_t blob_offset = names_offset + names_size;
    blob_offset = (blob_offset + kIthacaBlobAlign - 1) / kIthacaBlobAlign
                  * kIthacaBlobAlign;

    std::vector<uint8_t> blob;
    for (size_t i = 0; i < specs.size(); ++i) {
        uint64_t off = blob_offset + blob.size();
        IthacaEntry e;
        e.midi            = (uint16_t)specs[i].midi;
        e.channels        = 2;
        e.sample_rate     = (uint32_t)specs[i].sample_rate;
        e.entry_offset    = off;
        e.entry_size      = wavs[i].size();
        e.pcm_data_offset = findDataOffset(wavs[i]);
        e.sample_format   = kSampleFmtPcm16;
        e.frames          = specs[i].frames;
        e.rms_db          = specs[i].rms_db;
        e.attack_end      = specs[i].attack_end;
        e.name_offset     = kIthacaNoName;
        out.entries.push_back(e);
        blob.insert(blob.end(), wavs[i].begin(), wavs[i].end());
        // 4K zarovnani dalsiho zaznamu.
        while ((blob_offset + blob.size()) % kIthacaBlobAlign != 0)
            blob.push_back(0);
    }
    const uint64_t blob_size = blob.size();

    // 3) Serializace indexu (LE memcpy — zrcadlo parseIthacaIndex).
    std::vector<uint8_t> index_bytes(index_size, 0);
    for (size_t i = 0; i < out.entries.size(); ++i) {
        uint8_t* p = index_bytes.data() + i * kIthacaEntrySize;
        const IthacaEntry& e = out.entries[i];
        std::memcpy(p + 0,  &e.midi, 2);
        std::memcpy(p + 2,  &e.channels, 2);
        std::memcpy(p + 4,  &e.sample_rate, 4);
        std::memcpy(p + 8,  &e.entry_offset, 8);
        std::memcpy(p + 16, &e.entry_size, 8);
        std::memcpy(p + 24, &e.pcm_data_offset, 4);
        std::memcpy(p + 28, &e.sample_format, 2);
        std::memcpy(p + 32, &e.frames, 8);
        std::memcpy(p + 40, &e.rms_db, 4);
        std::memcpy(p + 44, &e.attack_end, 4);
        std::memcpy(p + 48, &e.name_offset, 4);
    }

    // 4) Hashe: sha256_index pres metadata+index+names; payload pres blob.
    Sha256 hi;
    hi.update(metadata.data(), metadata.size());
    hi.update(index_bytes.data(), index_bytes.size());
    auto sha_index = hi.finish();
    if (corrupt_index_hash) sha_index[0] ^= 0xFF;
    auto sha_payload = Sha256::hash(blob.data(), blob.size());

    // 5) Hlavicka.
    std::vector<uint8_t> header(kIthacaHeaderSize, 0);
    std::memcpy(header.data(), kIthacaMagic, 8);
    std::memcpy(header.data() + 8,  &force_version, 4);
    std::memcpy(header.data() + 12, &force_flags, 4);
    std::memcpy(header.data() + 16, &metadata_offset, 8);
    uint64_t msz = metadata.size();
    std::memcpy(header.data() + 24, &msz, 8);
    std::memcpy(header.data() + 32, &index_offset, 8);
    std::memcpy(header.data() + 40, &index_size, 8);
    std::memcpy(header.data() + 48, &names_offset, 8);
    std::memcpy(header.data() + 56, &names_size, 8);
    std::memcpy(header.data() + 64, &blob_offset, 8);
    std::memcpy(header.data() + 72, &blob_size, 8);
    uint32_t ec = (uint32_t)out.entries.size();
    std::memcpy(header.data() + 80, &ec, 4);
    std::memcpy(header.data() + 88,  sha_index.data(), 32);
    std::memcpy(header.data() + 120, sha_payload.data(), 32);

    // 6) Zapis: hlavicka + metadata + index + (names) + pad + blob.
    std::ofstream f(out.ithaca_path, std::ios::binary);
    f.write((const char*)header.data(), (std::streamsize)header.size());
    f.write(metadata.data(), (std::streamsize)metadata.size());
    f.write((const char*)index_bytes.data(), (std::streamsize)index_bytes.size());
    uint64_t pad = blob_offset - (names_offset + names_size);
    std::vector<char> zeros((size_t)pad, 0);
    f.write(zeros.data(), (std::streamsize)zeros.size());
    f.write((const char*)blob.data(), (std::streamsize)blob.size());
    f.close();
    return out;
}

// Uklid temp adresare (volat na konci testu).
inline void removeBlob(const BuiltBlob& b) {
    std::filesystem::remove_all(b.dir);
}

} // namespace ithaca_test
```

- [ ] **Step 2: Napiš failing test**

`tests/test_ithaca_bank.cpp`:

```cpp
// tests/test_ithaca_bank.cpp
// openIthacaBank: validace hlavicky, hash indexu, rozsahu zaznamu.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "ithaca_test_blob.h"
#include "sample/ithaca_bank.h"

using namespace ithaca;
using namespace ithaca_test;

TEST_CASE("openIthacaBank valid blob") {
    BuiltBlob b = buildTestIthaca("open_ok", {
        {60, 2000, 48000, -30.f, 100},
        {60, 2000, 48000, -20.f, 120},   // hlasitejsi vrstva stejne noty
        {72, 1500, 48000, -25.f, 90},
    });
    IthacaBankFile f = openIthacaBank(b.ithaca_path);
    REQUIRE(f.ok);
    CHECK(f.entries.size() == 3u);
    CHECK(f.handle != nullptr);
    CHECK(f.entries[0].midi == 60);
    CHECK(f.entries[1].rms_db == doctest::Approx(-20.f));
    CHECK(f.entries[2].frames == 1500);
    removeBlob(b);
}

TEST_CASE("openIthacaBank odmitne spatny hash indexu") {
    BuiltBlob b = buildTestIthaca("bad_hash", {{60, 2000, 48000, -30.f, 100}},
                                  /*corrupt_index_hash=*/true);
    IthacaBankFile f = openIthacaBank(b.ithaca_path);
    CHECK_FALSE(f.ok);
    CHECK(f.error.find("hash") != std::string::npos);
    removeBlob(b);
}

TEST_CASE("openIthacaBank odmitne nepodporovanou verzi") {
    BuiltBlob b = buildTestIthaca("bad_ver", {{60, 2000, 48000, -30.f, 100}},
                                  false, /*force_version=*/99);
    IthacaBankFile f = openIthacaBank(b.ithaca_path);
    CHECK_FALSE(f.ok);
    removeBlob(b);
}

TEST_CASE("openIthacaBank odmitne nastavene flags (v1 neumi sifru/podpis)") {
    BuiltBlob b = buildTestIthaca("bad_flags", {{60, 2000, 48000, -30.f, 100}},
                                  false, kIthacaVersion, /*force_flags=*/1);
    IthacaBankFile f = openIthacaBank(b.ithaca_path);
    CHECK_FALSE(f.ok);
    removeBlob(b);
}

TEST_CASE("openIthacaBank odmitne neexistujici soubor") {
    IthacaBankFile f = openIthacaBank("/tmp/ithaca_no_such_dir/soundbank.ithaca");
    CHECK_FALSE(f.ok);
}
```

Registrace v `tests/CMakeLists.txt` (helper je header-only v tests/, include path tam míří implicitně — doctest testy includují z aktuálního adresáře):

```cmake
add_executable(test_ithaca_bank test_ithaca_bank.cpp)
target_link_libraries(test_ithaca_bank PRIVATE ithaca_core doctest)
add_test(NAME test_ithaca_bank COMMAND test_ithaca_bank)
```

- [ ] **Step 3: Ověř fail**

Run: `cmake --build build --parallel 8`
Expected: FAIL — `sample/ithaca_bank.h: No such file`

- [ ] **Step 4: Implementuj**

`engine/sample/ithaca_bank.h`:

```cpp
#pragma once
// engine/sample/ithaca_bank.h
// Otevreni a validace pakovane banky soundbank.ithaca: hlavicka, SHA-256
// indexovych sekci (metadata+index+names — VZDY pri nacteni; blob hash
// overuje jen bake --verify), rozsahy zaznamu. Vraci zaznamy + otevreny
// handle pro readSampleRange (preload + streaming).

#include "io/file_handle.h"
#include "sample/ithaca_format.h"

#include <string>
#include <vector>

namespace ithaca {

struct IthacaBankFile {
    bool        ok = false;
    std::string error;       // duvod pri ok=false (caller loguje)
    IthacaHeader header;
    std::vector<IthacaEntry> entries;
    std::shared_ptr<IFileHandle> handle;
};

// Otevre <path> (plna cesta k soundbank.ithaca). Pri jakekoliv chybe ok=false
// + error; volajici zaloguje ERROR a vrati prazdnou banku.
IthacaBankFile openIthacaBank(const std::string& path);

} // namespace ithaca
```

`engine/sample/ithaca_bank.cpp`:

```cpp
// engine/sample/ithaca_bank.cpp — viz ithaca_bank.h.
#include "sample/ithaca_bank.h"

#include "util/sha256.h"

#include <cstring>

namespace ithaca {

namespace {
// Precti celou sekci do bufferu; false pri prekroceni souboru/chybe cteni.
bool readSection(const IFileHandle& h, uint64_t off, uint64_t size,
                 std::vector<uint8_t>& out) {
    if (off + size > h.size() || off + size < off) return false;   // overflow guard
    out.resize((size_t)size);
    if (size == 0) return true;
    return h.readAt(off, out.data(), (size_t)size);
}

IthacaBankFile fail(std::string msg) {
    IthacaBankFile f;
    f.error = std::move(msg);
    return f;
}
} // namespace

IthacaBankFile openIthacaBank(const std::string& path) {
    auto handle = openFileHandle(path);
    if (!handle) return fail("nelze otevrit soubor");
    if (handle->size() < kIthacaHeaderSize) return fail("soubor kratsi nez hlavicka");

    uint8_t hdr_buf[kIthacaHeaderSize];
    if (!handle->readAt(0, hdr_buf, sizeof(hdr_buf)))
        return fail("nelze precist hlavicku");

    IthacaBankFile f;
    if (!parseIthacaHeader(hdr_buf, sizeof(hdr_buf), f.header))
        return fail("spatny magic (neni soundbank.ithaca)");
    const IthacaHeader& h = f.header;
    if (h.version != kIthacaVersion)
        return fail("nepodporovana verze formatu (" + std::to_string(h.version) + ")");
    if (h.flags != 0)
        return fail("flags != 0 (sifrovana/podepsana banka — vyzaduje novejsi verzi)");
    if (h.entry_count == 0) return fail("prazdny index (entry_count=0)");
    if (h.index_size != (uint64_t)h.entry_count * kIthacaEntrySize)
        return fail("index_size nesedi s entry_count");

    // Sekce + hash indexu (metadata+index+names v poradi souboru).
    std::vector<uint8_t> metadata, index_bytes, names;
    if (!readSection(*handle, h.metadata_offset, h.metadata_size, metadata) ||
        !readSection(*handle, h.index_offset,    h.index_size,    index_bytes) ||
        !readSection(*handle, h.names_offset,    h.names_size,    names))
        return fail("sekce mimo rozsah souboru");
    if (h.blob_offset + h.blob_size > handle->size() ||
        h.blob_offset + h.blob_size < h.blob_offset)
        return fail("blob mimo rozsah souboru");

    Sha256 sha;
    sha.update(metadata.data(),    metadata.size());
    sha.update(index_bytes.data(), index_bytes.size());
    sha.update(names.data(),       names.size());
    if (sha.finish() != h.sha256_index)
        return fail("hash indexu nesouhlasi (poskozeny soubor)");

    if (!parseIthacaIndex(index_bytes.data(), index_bytes.size(),
                          h.entry_count, f.entries))
        return fail("nelze parsovat index");

    // Per-zaznam validace rozsahu a hodnot.
    for (const IthacaEntry& e : f.entries) {
        if (e.midi > 127)                      return fail("zaznam: midi > 127");
        if (e.channels != 1 && e.channels != 2) return fail("zaznam: channels mimo 1/2");
        if (e.sample_format < kSampleFmtPcm16 || e.sample_format > kSampleFmtPcm32)
            return fail("zaznam: neznamy sample_format");
        if (e.frames <= 0 || e.sample_rate == 0) return fail("zaznam: frames/sample_rate");
        if (e.entry_offset < h.blob_offset ||
            e.entry_offset + e.entry_size > h.blob_offset + h.blob_size ||
            e.entry_offset + e.entry_size < e.entry_offset)
            return fail("zaznam: mimo rozsah blobu");
        const uint64_t bps = (e.sample_format == kSampleFmtPcm16) ? 2
                           : (e.sample_format == kSampleFmtPcm24) ? 3 : 4;
        const uint64_t pcm_bytes = (uint64_t)e.frames * e.channels * bps;
        if ((uint64_t)e.pcm_data_offset + pcm_bytes > e.entry_size)
            return fail("zaznam: PCM data presahuji entry_size");
    }

    f.handle = std::move(handle);
    f.ok = true;
    return f;
}

} // namespace ithaca
```

Do `CMakeLists.txt` přidej `engine/sample/ithaca_bank.cpp` (za `ithaca_format.cpp`).

- [ ] **Step 5: Ověř pass**

Run: `cmake --build build --parallel 8 && ./build/tests/test_ithaca_bank`
Expected: all passed

- [ ] **Step 6: Commit**

```bash
git add engine/sample/ithaca_bank.h engine/sample/ithaca_bank.cpp tests/ithaca_test_blob.h tests/test_ithaca_bank.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(sample): openIthacaBank — otevreni a validace pakovane banky"
```

---

### Task 6: SampleFile lokátor + dispatcher readSampleRange

**Files:**
- Modify: `engine/sample/sample_types.h` (SampleFile + BankFormat)
- Create: `engine/io/sample_read.h`, `engine/io/sample_read.cpp`
- Test: `tests/test_sample_read.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Rozšiř datový model**

V `engine/sample/sample_types.h`:

1. Do `#include` bloku přidej `#include <cstdint>` a `#include <memory>`.
2. Enum + name (náhrada stávajících řádků 16–26):

```cpp
enum class BankFormat { Unknown, FixedVelocity, Extended, DynamicVelocity,
                        PackedIthaca };

inline const char* bankFormatName(BankFormat f) {
    switch (f) {
        case BankFormat::FixedVelocity:   return "fixed-velocity";
        case BankFormat::Extended:        return "extended";
        case BankFormat::DynamicVelocity: return "dynamic-velocity";
        case BankFormat::PackedIthaca:    return "packed-ithaca";
        case BankFormat::Unknown:         return "unknown";
    }
    return "unknown";
}
```

3. `SampleFile` (náhrada stávající struktury, řádky 34–40):

```cpp
struct IFileHandle;   // io/file_handle.h — forward (drzime jen shared_ptr)

// Reference na zdrojovy sampl — bezny WAV soubor (blob == nullptr; cte se
// pres readWavRange z path), NEBO region v pakovane .ithaca bance (blob !=
// nullptr; cte se pres readSampleRange preadem z blobu). path u packed nese
// cestu k .ithaca (logy). shared_ptr na handle drzi blob otevreny i pro
// in-flight streaming requesty pri vymene banky (worker muze docist ze
// stareho handle; gen guard data zahodi).
struct SampleFile {
    std::string path;
    int  frames      = 0;     // celkovy pocet frames v souboru
    int  sample_rate = 0;
    bool valid       = false;
    // -- packed (.ithaca) lokator --
    std::shared_ptr<IFileHandle> blob;   // null = bezny WAV soubor
    uint64_t pcm_offset    = 0;   // absolutni offset prvniho vzorku v .ithaca
    uint16_t channels      = 0;   // 1/2 (bajtova aritmetika rozsahu)
    uint16_t sample_format = 0;   // kSampleFmt* (ithaca_format.h)
};
```

- [ ] **Step 2: Napiš failing test**

`tests/test_sample_read.cpp`:

```cpp
// tests/test_sample_read.cpp
// readSampleRange: dispatcher WAV cesta vs. blob. Blob cteni musi byt
// bit-exact shodne s readWavRange nad puvodnim WAVem (stejna konverze
// wavSampleToFloat, stejna EOF semantika).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "io/sample_read.h"
#include "ithaca_test_blob.h"

using namespace ithaca;
using namespace ithaca_test;

namespace {
// SampleFile lokator pro i-ty zaznam testovaciho blobu.
SampleFile locatorFor(const BuiltBlob& b, std::shared_ptr<IFileHandle> h, int i) {
    const IthacaEntry& e = b.entries[(size_t)i];
    SampleFile f;
    f.path          = b.ithaca_path;
    f.frames        = (int)e.frames;
    f.sample_rate   = (int)e.sample_rate;
    f.valid         = true;
    f.blob          = std::move(h);
    f.pcm_offset    = e.entry_offset + e.pcm_data_offset;
    f.channels      = e.channels;
    f.sample_format = e.sample_format;
    return f;
}
} // namespace

TEST_CASE("readSampleRange blob == ocekavana ramp data (cely + vyrez)") {
    BuiltBlob b = buildTestIthaca("sr_basic", {{60, 4096, 48000, -30.f, 10}});
    auto h = openFileHandle(b.ithaca_path);
    REQUIRE(h != nullptr);
    SampleFile f = locatorFor(b, h, 0);

    WavData whole = readSampleRange(f, 0, 4096);
    REQUIRE(whole.valid);
    CHECK(whole.frames == 4096);
    CHECK(whole.sample_rate == 48000);
    REQUIRE(whole.samples.size() == b.expected_samples[0].size());
    for (size_t i = 0; i < whole.samples.size(); ++i)
        REQUIRE(whole.samples[i] == b.expected_samples[0][i]);   // bit-exact

    WavData mid = readSampleRange(f, 1000, 256);
    REQUIRE(mid.valid);
    CHECK(mid.frames == 256);
    CHECK(mid.samples[0] == b.expected_samples[0][1000 * 2]);
    CHECK(mid.samples[1] == b.expected_samples[0][1000 * 2 + 1]);
    removeBlob(b);
}

TEST_CASE("readSampleRange blob EOF semantika jako readWavRange") {
    BuiltBlob b = buildTestIthaca("sr_eof", {{60, 4096, 48000, -30.f, 10}});
    auto h = openFileHandle(b.ithaca_path);
    SampleFile f = locatorFor(b, h, 0);

    WavData clip = readSampleRange(f, 4000, 1000);   // orizne na 96
    REQUIRE(clip.valid);
    CHECK(clip.frames == 96);

    WavData past = readSampleRange(f, 4096, 100);    // za koncem → 0, valid
    REQUIRE(past.valid);
    CHECK(past.frames == 0);
    CHECK(past.samples.empty());

    WavData neg = readSampleRange(f, -1, 100);       // chyba
    CHECK_FALSE(neg.valid);
    removeBlob(b);
}

TEST_CASE("readSampleRange bez blobu deleguje na readWavRange") {
    // Bezny WAV na disku — dispatcher musi vratit totez co readWavRange.
    std::string p = "/tmp/ithaca_sr_plain.wav";
    auto samples = rampSamples(2048);
    REQUIRE(writeWavStereo16(p, samples, 48000));
    SampleFile f;
    f.path = p; f.frames = 2048; f.sample_rate = 48000; f.valid = true;
    WavData a = readSampleRange(f, 500, 100);
    WavData b2 = readWavRange(p, 500, 100);
    REQUIRE(a.valid); REQUIRE(b2.valid);
    REQUIRE(a.samples.size() == b2.samples.size());
    for (size_t i = 0; i < a.samples.size(); ++i)
        REQUIRE(a.samples[i] == b2.samples[i]);
    std::remove(p.c_str());
}

TEST_CASE("readSampleRange odmitne neznamy sample_format") {
    BuiltBlob b = buildTestIthaca("sr_fmt", {{60, 1024, 48000, -30.f, 10}});
    auto h = openFileHandle(b.ithaca_path);
    SampleFile f = locatorFor(b, h, 0);
    f.sample_format = 99;
    CHECK_FALSE(readSampleRange(f, 0, 10).valid);
    removeBlob(b);
}
```

Registrace v `tests/CMakeLists.txt`:

```cmake
add_executable(test_sample_read test_sample_read.cpp)
target_link_libraries(test_sample_read PRIVATE ithaca_core doctest)
add_test(NAME test_sample_read COMMAND test_sample_read)
```

- [ ] **Step 3: Ověř fail**

Run: `cmake --build build --parallel 8`
Expected: FAIL — `io/sample_read.h: No such file`

- [ ] **Step 4: Implementuj**

`engine/io/sample_read.h`:

```cpp
#pragma once
// engine/io/sample_read.h
// Dispatcher cteni vzorku: bezny WAV soubor (deleguje na readWavRange) NEBO
// region v pakovane .ithaca bance (pread z blobu + dekodovani pres
// wavSampleToFloat). Jednotna semantika s readWavRange: interleaved stereo
// float vc. mono→stereo zdvojeni; EOF (frame_off >= frames) = valid s 0
// frames; chyba = valid=false.

#include "io/wav_reader.h"
#include "sample/sample_types.h"

namespace ithaca {

WavData readSampleRange(const SampleFile& file, int64_t frame_off,
                        int64_t frame_count);

// Bajty na vzorek pro kSampleFmt* kod; 0 = neznamy format.
int sampleFormatBytes(uint16_t sample_format);

} // namespace ithaca
```

`engine/io/sample_read.cpp`:

```cpp
// engine/io/sample_read.cpp — viz sample_read.h.
#include "io/sample_read.h"

#include "io/file_handle.h"
#include "sample/ithaca_format.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ithaca {

int sampleFormatBytes(uint16_t f) {
    switch (f) {
        case kSampleFmtPcm16:   return 2;
        case kSampleFmtPcm24:   return 3;
        case kSampleFmtFloat32: return 4;
        case kSampleFmtPcm32:   return 4;
    }
    return 0;
}

namespace {
// kSampleFmt → (bits, audio_format) pro wavSampleToFloat.
void fmtToWav(uint16_t f, uint16_t& bits, uint16_t& audio_format) {
    switch (f) {
        case kSampleFmtPcm16:   bits = 16; audio_format = 1; break;
        case kSampleFmtPcm24:   bits = 24; audio_format = 1; break;
        case kSampleFmtFloat32: bits = 32; audio_format = 3; break;
        case kSampleFmtPcm32:   bits = 32; audio_format = 1; break;
        default:                bits = 0;  audio_format = 0; break;
    }
}
} // namespace

WavData readSampleRange(const SampleFile& file, int64_t frame_off,
                        int64_t frame_count) {
    if (!file.blob)
        return readWavRange(file.path, frame_off, frame_count);

    WavData out;
    if (frame_off < 0 || frame_count < 0) {
        return out;   // shodne s readWavRange (zaporne vstupy → invalid)
    }
    const int bps = sampleFormatBytes(file.sample_format);
    if (bps == 0 || (file.channels != 1 && file.channels != 2)) return out;

    out.sample_rate = file.sample_rate;
    const int64_t total = (int64_t)file.frames;
    if (frame_count == 0 || frame_off >= total) {
        out.valid = true;   // EOF/0-pozadavek → valid, 0 frames
        return out;
    }
    const int64_t n = std::min(frame_count, total - frame_off);
    const int     frame_bytes = bps * (int)file.channels;
    std::vector<uint8_t> raw((size_t)n * (size_t)frame_bytes);
    if (!file.blob->readAt(
            file.pcm_offset + (uint64_t)frame_off * (uint64_t)frame_bytes,
            raw.data(), raw.size()))
        return out;   // kratke cteni = poskozeny soubor → invalid

    uint16_t bits = 0, afmt = 0;
    fmtToWav(file.sample_format, bits, afmt);
    out.samples.resize((size_t)n * 2);
    for (int64_t i = 0; i < n; ++i) {
        const uint8_t* base = raw.data() + (size_t)i * (size_t)frame_bytes;
        float L = wavSampleToFloat(base, bits, afmt);
        float R = (file.channels >= 2) ? wavSampleToFloat(base + bps, bits, afmt)
                                       : L;   // mono → zdvoj (jako wav_reader)
        out.samples[(size_t)i * 2]     = L;
        out.samples[(size_t)i * 2 + 1] = R;
    }
    out.frames = (int)std::min<int64_t>(n, INT32_MAX);
    out.valid  = true;
    return out;
}

} // namespace ithaca
```

Do `CMakeLists.txt` přidej `engine/io/sample_read.cpp` (za `engine/io/file_handle.cpp`).

- [ ] **Step 5: Ověř pass (vč. celé sady — změna SampleFile nesmí nic rozbít)**

Run: `make test`
Expected: všechny testy PASS

- [ ] **Step 6: Commit**

```bash
git add engine/sample/sample_types.h engine/io/sample_read.h engine/io/sample_read.cpp tests/test_sample_read.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(io): readSampleRange dispatcher + SampleFile lokator pro packed banku"
```

---

### Task 7: Detekce PackedIthaca ve scanBank

**Files:**
- Modify: `engine/sample/bank_index.cpp` (scanBank, stupeň 0)
- Test: `tests/test_bank_index.cpp` (přidat TEST_CASE)

- [ ] **Step 1: Napiš failing test**

Na konec `tests/test_bank_index.cpp` přidej (soubor už používá doctest + `scanBank`; pokud chybí includes `<filesystem>`/`<fstream>`, doplň je):

```cpp
TEST_CASE("scanBank detekuje soundbank.ithaca s prioritou nade vsim") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_scan_packed";
    fs::remove_all(dir);
    fs::create_directories(dir + "/m060");           // i dynamic struktura...
    { std::ofstream f(dir + "/m060/aa.wav"); f << "x"; }
    { std::ofstream f(dir + "/soundbank.ithaca"); f << "x"; }   // ...ale packed vyhrava
    BankScan s = scanBank(dir);
    CHECK(s.format == BankFormat::PackedIthaca);
    CHECK(s.files.empty());     // zaznamy dodava ithaca index, ne sken
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Ověř fail**

Run: `cmake --build build --parallel 8 && ./build/tests/test_bank_index`
Expected: FAIL — nový TEST_CASE (format == DynamicVelocity místo PackedIthaca)

- [ ] **Step 3: Implementuj**

V `engine/sample/bank_index.cpp`:
- přidej `#include "sample/ithaca_format.h"` (kvůli `kIthacaFileName`),
- na začátek `scanBank` (hned za `std::error_code ec;`, před blok dynamic-velocity detekce) vlož:

```cpp
    // 0) Pakovana banka: soundbank.ithaca ma prednost nade vsim ostatnim
    //    obsahem adresare. Zaznamy dodava az ithaca index (loadBank),
    //    scan jen oznaci format.
    if (fs::is_regular_file(fs::path(dir) / kIthacaFileName, ec)) {
        scan.format = BankFormat::PackedIthaca;
        return scan;
    }
```

- [ ] **Step 4: Ověř pass**

Run: `cmake --build build --parallel 8 && ./build/tests/test_bank_index`
Expected: all passed

- [ ] **Step 5: Commit**

```bash
git add engine/sample/bank_index.cpp tests/test_bank_index.cpp
git commit -m "feat(sample): scanBank detekce pakovane banky (PackedIthaca)"
```

---

### Task 8: loadBank — packed větev (kostra z indexu, bez analýzy a sortu)

**Files:**
- Modify: `engine/sample/sample_store.cpp`
- Test: `tests/test_packed_bank_load.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Napiš failing test**

`tests/test_packed_bank_load.cpp`:

```cpp
// tests/test_packed_bank_load.cpp
// loadBank nad pakovanou bankou: kostra z indexu, baked RMS/attack
// autoritativni, poradi slotu = poradi indexu (predrazeno), preload heads
// ctene z blobu, mode FullyLoaded/Streamed dle runtime preload_ms.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "ithaca_test_blob.h"
#include "sample/sample_store.h"

using namespace ithaca;
using namespace ithaca_test;

TEST_CASE("loadBank packed: sloty, baked hodnoty, preload z blobu") {
    // 2 vrstvy noty 60 (tissi pred hlasitejsi — jak je radi bake) + nota 72.
    // 4096 frames @48k: pri preload_ms=150 je preload_frames=7200 →
    // frames <= 2*7200 → FullyLoaded (cely sampl v RAM).
    BuiltBlob b = buildTestIthaca("load_basic", {
        {60, 4096, 48000, -30.f, 100},
        {60, 4096, 48000, -20.f, 120},
        {72, 4096, 48000, -25.f, 90},
    });
    auto& L = log::Logger::default_();
    Bank bank = loadBank(b.dir, L);
    CHECK(bank.format == BankFormat::PackedIthaca);
    CHECK(bank.loaded_samples == 3);
    REQUIRE(bank.notes[60].recorded);
    REQUIRE(bank.notes[60].slots.size() == 2u);
    REQUIRE(bank.notes[72].slots.size() == 1u);
    // Baked RMS autoritativni (zadne mereni!) a poradi dle indexu.
    CHECK(bank.notes[60].slots[0].rms_db == doctest::Approx(-30.f));
    CHECK(bank.notes[60].slots[1].rms_db == doctest::Approx(-20.f));
    CHECK(bank.notes[60].slots[0].variants[0].attack_end_frame == 100);
    // Preload data bit-exact z blobu.
    const MicLayer& m = bank.notes[60].slots[0].variants[0].mics[0];
    CHECK(m.mode == MicLayerMode::FullyLoaded);
    REQUIRE(m.head_frames == 4096);
    for (size_t i = 0; i < m.preload_head.size(); ++i)
        REQUIRE(m.preload_head[i] == b.expected_samples[0][i]);
    // Lokator pro streaming pripraveny.
    CHECK(m.file.blob != nullptr);
    CHECK(m.file.sample_rate == 48000);
    removeBlob(b);
}

TEST_CASE("loadBank packed: dlouhy sampl je Streamed s baked resonance startem") {
    // 48000 frames @48k: preload_frames=7200, 48000 > 14400 → Streamed.
    BuiltBlob b = buildTestIthaca("load_stream", {{60, 48000, 48000, -25.f, 500}});
    auto& L = log::Logger::default_();
    Bank bank = loadBank(b.dir, L);
    REQUIRE(bank.notes[60].slots.size() == 1u);
    const MicLayer& m = bank.notes[60].slots[0].variants[0].mics[0];
    CHECK(m.mode == MicLayerMode::Streamed);
    CHECK(m.head_frames == 7200);
    CHECK(m.resonance_start_frame == 500);   // = baked attack_end
    removeBlob(b);
}

TEST_CASE("loadBank packed: midi filtr") {
    BuiltBlob b = buildTestIthaca("load_filter", {
        {60, 2048, 48000, -30.f, 10}, {72, 2048, 48000, -25.f, 10}});
    auto& L = log::Logger::default_();
    Bank bank = loadBank(b.dir, L, 0, /*midi_from=*/70, /*midi_to=*/80);
    CHECK_FALSE(bank.notes[60].recorded);
    CHECK(bank.notes[72].recorded);
    CHECK(bank.loaded_samples == 1);
    removeBlob(b);
}

TEST_CASE("loadBank packed: poskozeny soubor → prazdna banka, nepada") {
    BuiltBlob b = buildTestIthaca("load_corrupt", {{60, 2048, 48000, -30.f, 10}},
                                  /*corrupt_index_hash=*/true);
    auto& L = log::Logger::default_();
    Bank bank = loadBank(b.dir, L);
    CHECK(bank.format == BankFormat::PackedIthaca);
    CHECK(bank.loaded_samples == 0);
    removeBlob(b);
}

TEST_CASE("loadBank packed == loadBank adresar (bit-exact preload data)") {
    // Stejne 2 vrstvy noty 60 jako dynamic-velocity adresar vs. packed blob.
    // Poradi slotu v adresarove bance urcuje MERENA RMS (DSP detail — zde
    // nepredikujeme); sloty parujeme podle delky (4096 vs 2048 je
    // jednoznacne) a porovnavame preload data bit-exact.
    namespace fs = std::filesystem;
    BuiltBlob b = buildTestIthaca("load_equiv", {
        {60, 4096, 48000, -30.f, 0},
        {60, 2048, 48000, -20.f, 0},
    });
    std::string ddir = "/tmp/ithaca_equiv_dyn";
    fs::remove_all(ddir);
    fs::create_directories(ddir + "/m060");
    writeWavStereo16(ddir + "/m060/a.wav", rampSamples(4096), 48000);
    writeWavStereo16(ddir + "/m060/b.wav", rampSamples(2048), 48000);
    auto& L = log::Logger::default_();
    Bank pb = loadBank(b.dir, L);
    Bank db = loadBank(ddir, L);
    REQUIRE(pb.notes[60].slots.size() == 2u);
    REQUIRE(db.notes[60].slots.size() == 2u);
    for (int want = 0; want < 2; ++want) {
        const int frames = (want == 0) ? 4096 : 2048;
        const MicLayer* pm = nullptr; const MicLayer* dm = nullptr;
        for (const auto& s : pb.notes[60].slots)
            if (s.variants[0].mics[0].file.frames == frames)
                pm = &s.variants[0].mics[0];
        for (const auto& s : db.notes[60].slots)
            if (s.variants[0].mics[0].file.frames == frames)
                dm = &s.variants[0].mics[0];
        REQUIRE(pm != nullptr); REQUIRE(dm != nullptr);
        REQUIRE(pm->head_frames == dm->head_frames);
        REQUIRE(pm->preload_head.size() == dm->preload_head.size());
        for (size_t i = 0; i < pm->preload_head.size(); ++i)
            REQUIRE(pm->preload_head[i] == dm->preload_head[i]);   // bit-exact
    }
    fs::remove_all(ddir);
    removeBlob(b);
}
```

Registrace v `tests/CMakeLists.txt`:

```cmake
add_executable(test_packed_bank_load test_packed_bank_load.cpp)
target_link_libraries(test_packed_bank_load PRIVATE ithaca_core doctest)
add_test(NAME test_packed_bank_load COMMAND test_packed_bank_load)
```

- [ ] **Step 2: Ověř fail**

Run: `cmake --build build --parallel 8 && ./build/tests/test_packed_bank_load`
Expected: FAIL — packed banka se nenačte (`loaded_samples == 0`, Unknown chování)

- [ ] **Step 3: Implementuj packed větev v sample_store.cpp**

V `engine/sample/sample_store.cpp`:

1. Přidej includes: `#include "io/sample_read.h"`, `#include "sample/ithaca_bank.h"`.

2. Do anonymního namespace přidej (za `prepareSampleFile`):

```cpp
// Priprava JEDNOHO zaznamu pakovane banky: ZADNA analyza (RMS/attack jsou
// baked v indexu — autoritativni), jen preload head pres readSampleRange.
// Mode FullyLoaded/Streamed zustava runtime rozhodnuti (baked frames vs
// aktualni preload_ms).
PreparedSample preparePackedSample(const IthacaEntry& e,
                                   const std::shared_ptr<IFileHandle>& blob,
                                   const std::string& ithaca_path,
                                   log::Logger& logger, int preload_ms,
                                   size_t budget_bytes,
                                   std::atomic<size_t>& approx_bytes) {
    PreparedSample out;
    out.midi     = (int)e.midi;
    out.filename = ithaca_path;   // puvodni jmeno nedrzime (jen pro logy)

    MicLayer mic;
    mic.mic_name           = "stereo";
    mic.file.path          = ithaca_path;
    mic.file.frames        = (int)(std::min)(e.frames, (int64_t)INT32_MAX);
    mic.file.sample_rate   = (int)e.sample_rate;
    mic.file.valid         = true;
    mic.file.blob          = blob;
    mic.file.pcm_offset    = e.entry_offset + e.pcm_data_offset;
    mic.file.channels      = e.channels;
    mic.file.sample_format = e.sample_format;

    const int preload_frames =
        (int)((int64_t)preload_ms * mic.file.sample_rate / 1000);
    if (mic.file.frames <= preload_frames * 2) {
        mic.mode        = MicLayerMode::FullyLoaded;
        mic.head_frames = mic.file.frames;
    } else {
        mic.mode        = MicLayerMode::Streamed;
        mic.head_frames = preload_frames;
    }

    const size_t est = (size_t)mic.head_frames * 2 * sizeof(float);
    if (budget_bytes &&
        approx_bytes.fetch_add(est, std::memory_order_relaxed) + est
            > budget_bytes * 2) {
        return out;   // OOM guard (stejne jako prepareSampleFile)
    }

    WavData head = readSampleRange(mic.file, 0, mic.head_frames);
    if (!head.valid) {
        logger.log("bank", log::Severity::Warning,
                   "Packed: nelze nacist preload midi %d @ %llu",
                   out.midi, (unsigned long long)mic.file.pcm_offset);
        return out;
    }
    mic.preload_head = std::move(head.samples);

    // Baked analyza z indexu — zadne mereni.
    const float rms = e.rms_db;
    const int   ae  = (int)e.attack_end;
    if (mic.mode == MicLayerMode::Streamed)
        mic.resonance_start_frame = ae;

    out.bytes = mic.preload_head.size() * sizeof(float);
    SampleAsset asset;
    asset.peak_rms_db      = rms;
    asset.attack_end_frame = ae;
    asset.mics.push_back(std::move(mic));
    out.slot.rms_db = rms;
    out.slot.variants.push_back(std::move(asset));
    out.ok = true;
    return out;
}

// Nacteni pakovane banky: kostra primo z indexu (zadny directory scan, zadna
// RMS analyza, zadny sort — index je predrazeny dle (midi, rms)). Faze heads
// bezi paralelne jako u adresarove banky; merge ve scan poradi = poradi
// indexu, takze sloty prijdou vzestupne dle baked RMS.
void loadPackedBank(Bank& bank, const std::string& dir, log::Logger& logger,
                    int cache_budget_mb, int midi_from, int midi_to,
                    int preload_ms, BankLoadProgress* progress) {
    namespace fs = std::filesystem;
    const std::string ithaca_path = (fs::path(dir) / kIthacaFileName).string();
    IthacaBankFile pf = openIthacaBank(ithaca_path);
    if (!pf.ok) {
        logger.log("bank", log::Severity::Error,
                   "Banka '%s': soundbank.ithaca odmitnut — %s",
                   bank.name.c_str(), pf.error.c_str());
        return;   // prazdna banka (format zustava PackedIthaca)
    }
    logger.log("bank", log::Severity::Info,
               "Banka '%s': packed-ithaca, %zu zaznamu",
               bank.name.c_str(), pf.entries.size());

    const size_t budget_bytes = cache_budget_mb > 0
                              ? (size_t)cache_budget_mb * 1024 * 1024 : 0;
    std::vector<int> idx;
    idx.reserve(pf.entries.size());
    for (int i = 0; i < (int)pf.entries.size(); ++i) {
        const int midi = (int)pf.entries[(size_t)i].midi;
        if (midi < midi_from || midi > midi_to) continue;
        idx.push_back(i);
    }
    if (progress) {
        progress->phase.store(1);
        progress->total.store((int)idx.size());
        progress->done.store(0);
    }
    std::vector<PreparedSample> prepared(idx.size());
    std::atomic<size_t> approx_bytes{0};
    parallelFor((int)idx.size(), loaderWorkers(), [&](int i) {
        prepared[(size_t)i] = preparePackedSample(
            pf.entries[(size_t)idx[(size_t)i]], pf.handle, ithaca_path,
            logger, preload_ms, budget_bytes, approx_bytes);
        if (progress) {
            progress->done.fetch_add(1, std::memory_order_relaxed);
            progress->bytes_loaded.fetch_add(prepared[(size_t)i].bytes,
                                             std::memory_order_relaxed);
        }
    });
    for (auto& p : prepared) {
        if (!p.ok) continue;
        if (budget_bytes && bank.total_bytes >= budget_bytes) {
            logger.log("bank", log::Severity::Error,
                       "Banka '%s': RAM budget %d MB prekrocen (~%zu MB) — "
                       "nacitani PRERUSENO, banka NEUPLNA.",
                       bank.name.c_str(), cache_budget_mb,
                       bank.total_bytes / (1024 * 1024));
            if (progress) progress->truncated.store(true, std::memory_order_relaxed);
            break;
        }
        commitSample(bank, std::move(p));
    }
    if (progress)
        progress->bytes_loaded.store(bank.total_bytes, std::memory_order_relaxed);
    // ZADNY sortBankSlotsByRms: poradi indexu (midi, rms vzestupne) je
    // autoritativni; std::sort neni stabilni a u shodnych RMS by mohl
    // prohodit bake poradi.
    logBankSummary(bank, logger, cache_budget_mb);
}
```

3. V `loadBank()` přidej větev hned za `scan.format == BankFormat::Unknown` blok (před Extended):

```cpp
    if (scan.format == BankFormat::PackedIthaca) {
        loadPackedBank(bank, dir, logger, cache_budget_mb, midi_from, midi_to,
                       preload_ms, progress);
        return bank;
    }
```

4. V `buildResonanceCache()` nahraď volání `readWavRange(m.file.path, m.resonance_start_frame, rwin)` za `readSampleRange(m.file, m.resonance_start_frame, rwin)` — tím rezo cache čte z blobu i z WAV cest jednotně.

5. V `prepareSampleFile()` nahraď `readWavRange(full_path, 0, mic.head_frames)` za `readSampleRange(mic.file, 0, mic.head_frames)` (mic.file.blob je null → deleguje na readWavRange; sjednocení cesty).

- [ ] **Step 4: Ověř pass (vč. celé sady — adresářové banky beze změny chování)**

Run: `make test`
Expected: všechny testy PASS (zejména `test_sample_store`, `test_resonance_cache_build`, `test_render_regression` beze změny)

- [ ] **Step 5: Commit**

```bash
git add engine/sample/sample_store.cpp tests/test_packed_bank_load.cpp tests/CMakeLists.txt
git commit -m "feat(sample): loadBank packed vetev — kostra z indexu, bez analyzy a sortu"
```

---

### Task 9: Streaming z blobu (StreamRequest nese SampleFile)

**Files:**
- Modify: `engine/stream/stream_engine.h` (StreamRequest, requestRead), `engine/stream/stream_engine.cpp` (worker), `engine/stream/streamed_reader.h` + `.cpp` (begin/refill), `engine/voice/voice.cpp`, `engine/voice/resonance_voice.cpp`
- Test: `tests/test_packed_stream.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Napiš failing test**

`tests/test_packed_stream.cpp`:

```cpp
// tests/test_packed_stream.cpp
// Streaming worker cte z blobu pres readSampleRange: request s packed
// lokatorem naplni ring stejnymi daty jako WAV soubor.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "ithaca_test_blob.h"
#include "io/sample_read.h"
#include "stream/stream_engine.h"

#include <chrono>
#include <thread>

using namespace ithaca;
using namespace ithaca_test;

namespace {
// Pocka az ring ma aspon n frames (timeout 2 s).
bool waitAvail(RingHandle* r, int n) {
    for (int i = 0; i < 200; ++i) {
        if (r->available() >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

TEST_CASE("stream worker plni ring z packed blobu") {
    BuiltBlob b = buildTestIthaca("stream_ring", {{60, 16384, 48000, -25.f, 10}});
    auto h = openFileHandle(b.ithaca_path);
    REQUIRE(h != nullptr);
    SampleFile f;
    f.path          = b.ithaca_path;
    f.frames        = 16384; f.sample_rate = 48000; f.valid = true;
    f.blob          = h;
    f.pcm_offset    = b.entries[0].entry_offset + b.entries[0].pcm_data_offset;
    f.channels      = 2;
    f.sample_format = kSampleFmtPcm16;

    StreamEngine se(/*n_rings=*/2, /*ring_capacity_frames=*/4096, /*n_workers=*/1);
    se.start();
    RingHandle* ring = se.acquireRing();
    REQUIRE(ring != nullptr);
    // Pozadej 1024 frames od offsetu 8000.
    REQUIRE(se.requestRead(ring, f, 8000, 1024, /*eof_when_done=*/false));
    REQUIRE(waitAvail(ring, 1024));
    for (int i = 0; i < 1024; ++i) {
        float L, R;
        REQUIRE(ring->popFrame(L, R));
        REQUIRE(L == b.expected_samples[0][(size_t)(8000 + i) * 2]);
        REQUIRE(R == b.expected_samples[0][(size_t)(8000 + i) * 2 + 1]);
    }
    se.releaseRing(ring);
    se.stop();
    removeBlob(b);
}

TEST_CASE("stream worker EOF na konci packed samplu") {
    BuiltBlob b = buildTestIthaca("stream_eof", {{60, 4096, 48000, -25.f, 10}});
    auto h = openFileHandle(b.ithaca_path);
    SampleFile f;
    f.path = b.ithaca_path; f.frames = 4096; f.sample_rate = 48000; f.valid = true;
    f.blob = h;
    f.pcm_offset    = b.entries[0].entry_offset + b.entries[0].pcm_data_offset;
    f.channels      = 2;
    f.sample_format = kSampleFmtPcm16;

    StreamEngine se(2, 8192, 1);
    se.start();
    RingHandle* ring = se.acquireRing();
    REQUIRE(ring != nullptr);
    REQUIRE(se.requestRead(ring, f, 0, 4096, /*eof_when_done=*/true));
    REQUIRE(waitAvail(ring, 4096));
    // Po dokonceni requestu s eof_when_done nastavi worker eof_.
    for (int i = 0; i < 200 && !ring->eof_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(ring->eof_.load());
    se.releaseRing(ring);
    se.stop();
    removeBlob(b);
}
```

Registrace v `tests/CMakeLists.txt`:

```cmake
add_executable(test_packed_stream test_packed_stream.cpp)
target_link_libraries(test_packed_stream PRIVATE ithaca_core doctest)
add_test(NAME test_packed_stream COMMAND test_packed_stream)
```

- [ ] **Step 2: Ověř fail**

Run: `cmake --build build --parallel 8`
Expected: FAIL — `requestRead` nebere `SampleFile` (no matching function)

- [ ] **Step 3: Implementuj**

**`engine/stream/stream_engine.h`:**

1. Přidej `#include "sample/sample_types.h"` do include bloku.
2. `StreamRequest` — nahraď member `std::string path;` (a jeho komentář, řádky 90–99) za:

```cpp
    // Zdroj dat: SampleFile lokator (WAV cesta NEBO packed blob). Kopiruje se
    // pri push: string copy (SBO pro kratke cesty) + shared_ptr refcount
    // (atomic inc, bez zamku/alokace) — stejna RT uvaha jako drive u path.
    // shared_ptr drzi blob handle nazivu i kdyz se banka mezitim vymeni
    // (worker docte ze stareho handle; gen guard data zahodi).
    SampleFile  file;
```

3. `requestRead` — změň signaturu (deklarace v class StreamEngine):

```cpp
    bool requestRead(RingHandle* ring, const SampleFile& file,
                     int64_t frame_off, int64_t n_frames,
                     bool eof_when_done = false) noexcept;
```

**`engine/stream/stream_engine.cpp`:**

1. Přidej `#include "io/sample_read.h"`.
2. V `requestRead` nahraď parametr `const std::string& path` za `const SampleFile& file` a `req.path = path;` za `req.file = file;` (komentář „kopie…" ponech/uprav).
3. Ve `workerLoop` nahraď:

```cpp
            WavData data = readWavRange(req.path, off, chunk);
```

za:

```cpp
            WavData data = readSampleRange(req.file, off, chunk);
```

a v logu o chybě `req.path.c_str()` za `req.file.path.c_str()`. Pokud soubor includuje `io/wav_reader.h` jen kvůli readWavRange, include nech (WavData type) — jen ověř kompilaci.

**`engine/stream/streamed_reader.h`:**

1. Nahraď `#include <string>` za `#include "sample/sample_types.h"` (SampleFile už nese path).
2. Změň signatury:

```cpp
    bool begin(StreamEngine* se, const SampleFile& file,
               int64_t start_frame, int64_t total_frames) noexcept;
```

```cpp
    void refill(StreamEngine* se, const SampleFile& file) noexcept;
```

(komentáře u obou metod zůstávají platné — jen `path` → `file`).

**`engine/stream/streamed_reader.cpp`:** v `begin` a `refill` nahraď parametr `const std::string& path` za `const SampleFile& file` a obě volání `se->requestRead(ring_, path, …)` za `se->requestRead(ring_, file, …)`.

**`engine/voice/voice.cpp`:** řádek 126 `reader_.begin(stream_, mic_->file.path, …)` → `reader_.begin(stream_, mic_->file, …)`; řádek 305 `reader_.refill(stream_, mic_->file.path)` → `reader_.refill(stream_, mic_->file)`.

**`engine/voice/resonance_voice.cpp`:** řádek 79 `reader_.begin(stream_, mic_->file.path, …)` → `reader_.begin(stream_, mic_->file, …)`; řádek 321 `reader_.refill(stream_, mic_->file.path)` → `reader_.refill(stream_, mic_->file)`.

Pozn.: žádné jiné call-sites `requestRead`/`begin`/`refill` nejsou (ověř `grep -rn "requestRead\|reader_.begin\|reader_.refill" engine/ tests/`); testy `test_stream_engine.cpp` a `test_long_sample_stream.cpp` možná volají `requestRead` s cestou — pokud ano, uprav je: postav `SampleFile f; f.path = <cesta>; f.frames = <frames>; f.sample_rate = <sr>; f.valid = true;` a předej `f`.

- [ ] **Step 4: Ověř pass (celá sada — streaming regrese jsou kritické)**

Run: `make test`
Expected: všechny testy PASS, zejména `test_stream_engine`, `test_long_sample_stream`, `test_streamed_interp`, `test_render_regression`, `test_resonance_stream_sr` (bit-exact chování WAV cesty se nemění — dispatcher na ni jen deleguje)

- [ ] **Step 5: Commit**

```bash
git add engine/stream/stream_engine.h engine/stream/stream_engine.cpp engine/stream/streamed_reader.h engine/stream/streamed_reader.cpp engine/voice/voice.cpp engine/voice/resonance_voice.cpp tests/test_packed_stream.cpp tests/CMakeLists.txt
git commit -m "feat(stream): streaming z packed blobu — StreamRequest nese SampleFile"
```

---

### Task 10: CLI `--dump-bank-index`

**Files:**
- Modify: `app/cli/main.cpp`

JSON s analýzou per soubor jde do souboru přes `--out` (ne na stdout — logy loaderu by výstup znečistily). Formát:

```json
{ "bank_format": "dynamic-velocity", "preload_ms": 150,
  "files": [ { "path": "...", "midi": 21, "frames": 480000,
               "sample_rate": 48000, "rms_db": -23.4, "attack_end": 1234 } ] }
```

Pořadí `files` = pořadí (midi vzestupně, sloty vzestupně dle RMS) — tj. přesně pořadí, ve kterém je bake zapíše do indexu.

- [ ] **Step 1: Implementuj (CLI nemá unit testy — ověření ručně v kroku 2)**

V `app/cli/main.cpp`:

1. Přidej proměnné k ostatním deklaracím v `main` (výstupní cestu sdílí s `--render` proměnná `render_out`):

```cpp
    std::string dump_dir;
    int         dump_preload_ms = 150;
```

2. Do parsování argumentů (za větev `--inspect`):

```cpp
        } else if (a == "--dump-bank-index" && i + 1 < argc) {
            dump_dir = argv[++i];
        } else if (a == "--preload-ms" && i + 1 < argc) {
            dump_preload_ms = std::atoi(argv[++i]);
            if (dump_preload_ms < 0) dump_preload_ms = 0;
```

a uprav `--out` komentář v usage (sdílí ho `--render` a `--dump-bank-index`). Do `printUsage` přidej řádky:

```
  --dump-bank-index <dir>  nacti banku, vypis JSON analyzy do --out <json>
  --preload-ms N           preload okno analyzy pro --dump-bank-index (default 150)
```

3. Před blok `if (!inspect_dir.empty())` přidej:

```cpp
    if (!dump_dir.empty()) {
        if (render_out.empty()) {
            LOG_ERROR("dump", "--dump-bank-index vyzaduje --out <json>");
            return 1;
        }
        Bank bank = loadBank(dump_dir, L, /*cache_budget_mb=*/0,
                             /*midi_from=*/0, /*midi_to=*/127,
                             dump_preload_ms);
        if (bank.loaded_samples == 0) {
            LOG_ERROR("dump", "Zadne samply nenacteny z %s", dump_dir.c_str());
            return 1;
        }
        std::FILE* f = std::fopen(render_out.c_str(), "wb");
        if (!f) {
            LOG_ERROR("dump", "Nelze otevrit vystup: %s", render_out.c_str());
            return 1;
        }
        // Minimalni JSON escape pro cesty (backslash + uvozovky staci — jine
        // ridici znaky se v cestach bank nevyskytuji).
        auto esc = [](const std::string& s) {
            std::string o; o.reserve(s.size());
            for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
            return o;
        };
        std::fprintf(f, "{\"bank_format\":\"%s\",\"preload_ms\":%d,\"files\":[",
                     bankFormatName(bank.format), dump_preload_ms);
        bool first = true;
        for (int n = 0; n < 128; ++n) {
            for (const auto& slot : bank.notes[n].slots) {
                const auto& asset = slot.variants[0];
                const auto& mic   = asset.mics[0];
                // %.9g: presny float→text→float round-trip (bake uklada
                // rms_db zpet jako f32 — bit-exact shoda s adresarovym dump).
                std::fprintf(f, "%s\n  {\"path\":\"%s\",\"midi\":%d,"
                             "\"frames\":%d,\"sample_rate\":%d,"
                             "\"rms_db\":%.9g,\"attack_end\":%d}",
                             first ? "" : ",", esc(mic.file.path).c_str(), n,
                             mic.file.frames, mic.file.sample_rate,
                             (double)slot.rms_db, asset.attack_end_frame);
                first = false;
            }
        }
        std::fprintf(f, "\n]}\n");
        std::fclose(f);
        LOG_INFO("dump", "Index zapsan: %s (%d samplu)",
                 render_out.c_str(), bank.loaded_samples);
        return 0;
    }
```

- [ ] **Step 2: Ověř ručně na fixture bance**

```bash
cmake --build build --parallel 8
mkdir -p /tmp/dump_fixture/m060 /tmp/dump_fixture/m061
python3 - <<'EOF'
import wave, struct, math
for path, amp in [("/tmp/dump_fixture/m060/a.wav", 8000),
                  ("/tmp/dump_fixture/m060/b.wav", 20000),
                  ("/tmp/dump_fixture/m061/c.wav", 12000)]:
    f = wave.open(path, "wb"); f.setnchannels(2); f.setsampwidth(2); f.setframerate(48000)
    f.writeframes(b"".join(struct.pack("<hh", int(amp*math.sin(i*0.05)),
                  int(amp*math.sin(i*0.05))) for i in range(24000)))
    f.close()
EOF
./build/ithaca-cli --dump-bank-index /tmp/dump_fixture --out /tmp/dump.json
python3 -m json.tool /tmp/dump.json
```

Expected: exit 0; JSON má 3 záznamy; nota 60 má 2 záznamy seřazené vzestupně dle `rms_db` (a.wav s amp 8000 první); `python3 -m json.tool` projde (validní JSON). Pak `rm -rf /tmp/dump_fixture /tmp/dump.json`.

- [ ] **Step 3: Commit**

```bash
git add app/cli/main.cpp
git commit -m "feat(cli): --dump-bank-index — JSON analyzy banky pro bake nastroj"
```

---

### Task 11: tools/bake_soundbank.py

**Files:**
- Create: `tools/bake_soundbank.py`
- Test: `tests/test_bake_soundbank.py` (čistý python, bez CLI — analýza se injektuje)

Jen stdlib. Funkce oddělené od CLI wrapperu kvůli testovatelnosti: `parse_riff()`, `compute_layout()`, `write_ithaca()`, `read_ithaca_header()` (pro verify), `verify_ithaca()`.

- [ ] **Step 1: Napiš failing test**

`tests/test_bake_soundbank.py`:

```python
#!/usr/bin/env python3
# tests/test_bake_soundbank.py
# Unit testy bake nastroje BEZ zavislosti na C++ CLI: analyza (rms/attack)
# se injektuje, testuje se RIFF parse, layout, zapis a verify.
import os
import struct
import sys
import tempfile
import unittest
import wave

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import bake_soundbank as bake


def make_wav(path, frames=1000, sr=48000, amp=8000):
    f = wave.open(path, "wb")
    f.setnchannels(2)
    f.setsampwidth(2)
    f.setframerate(sr)
    f.writeframes(b"".join(struct.pack("<hh", (i * amp) % 32767, -((i * amp) % 32767))
                           for i in range(frames)))
    f.close()


class TestParseRiff(unittest.TestCase):
    def test_parse_pcm16(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "a.wav")
            make_wav(p, frames=1000, sr=44100)
            info = bake.parse_riff(p)
            self.assertEqual(info["channels"], 2)
            self.assertEqual(info["sample_rate"], 44100)
            self.assertEqual(info["sample_format"], bake.FMT_PCM16)
            self.assertEqual(info["frames"], 1000)
            self.assertGreaterEqual(info["pcm_data_offset"], 44)
            self.assertEqual(info["entry_size"], os.path.getsize(p))

    def test_reject_non_wav(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.wav")
            with open(p, "wb") as f:
                f.write(b"not a wav at all")
            with self.assertRaises(bake.BakeError):
                bake.parse_riff(p)


class TestBakeRoundtrip(unittest.TestCase):
    def _bake_minimal(self, d):
        # 2 vrstvy noty 60 + 1 vrstva noty 72; analyza injektovana.
        src = os.path.join(d, "src")
        os.makedirs(os.path.join(src, "m060"))
        os.makedirs(os.path.join(src, "m072"))
        wavs = [("m060/a.wav", 60, -30.0, 100, 3000),
                ("m060/b.wav", 60, -20.0, 120, 3000),
                ("m072/c.wav", 72, -25.0, 90, 2000)]
        analysis = []
        for rel, midi, rms, ae, frames in wavs:
            p = os.path.join(src, rel)
            make_wav(p, frames=frames)
            analysis.append({"path": p, "midi": midi, "frames": frames,
                             "sample_rate": 48000, "rms_db": rms,
                             "attack_end": ae})
        out = os.path.join(d, "soundbank.ithaca")
        bake.write_ithaca(out, analysis, bank_name="test",
                          analysis_preload_ms=150)
        return src, out, analysis

    def test_write_and_header(self):
        with tempfile.TemporaryDirectory() as d:
            _, out, analysis = self._bake_minimal(d)
            hdr = bake.read_ithaca_header(out)
            self.assertEqual(hdr["version"], 1)
            self.assertEqual(hdr["flags"], 0)
            self.assertEqual(hdr["entry_count"], 3)
            self.assertEqual(hdr["blob_offset"] % bake.BLOB_ALIGN, 0)
            # Index predrazeny dle (midi, rms): a (-30) pred b (-20).
            self.assertEqual([e["midi"] for e in hdr["entries"]], [60, 60, 72])
            self.assertAlmostEqual(hdr["entries"][0]["rms_db"], -30.0, places=4)

    def test_verify_ok_and_extraction(self):
        with tempfile.TemporaryDirectory() as d:
            src, out, analysis = self._bake_minimal(d)
            # verify_ithaca kontroluje oba hashe + bit-exact extrakci.
            bake.verify_ithaca(out, analysis)   # nesmi vyhodit

    def test_verify_detects_corruption(self):
        with tempfile.TemporaryDirectory() as d:
            _, out, analysis = self._bake_minimal(d)
            with open(out, "r+b") as f:
                f.seek(os.path.getsize(out) - 1)
                f.write(b"\xff")            # poskozeni blobu
            with self.assertRaises(bake.BakeError):
                bake.verify_ithaca(out, analysis)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Ověř fail**

Run: `python3 tests/test_bake_soundbank.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'bake_soundbank'`

- [ ] **Step 3: Implementuj**

`tools/bake_soundbank.py`:

```python
#!/usr/bin/env python3
"""bake_soundbank.py — zabali dynamickou banku do soundbank.ithaca (v1).

Format (LE): [hlavicka 408 B][metadata JSON][index 64 B/zaznam][names]
[blob: doslovne WAVy zarovnane na 4096 B]. Layout viz
docs/bank-format-packed.md. Analyzu (rms_db, attack_end) NEpocitame —
prebira se z `ithaca-cli --dump-bank-index` (jediny zdroj pravdy, shodne
poradi velocity vrstev s adresarovym nactenim).

Pouziti:
  python3 tools/bake_soundbank.py \
      --source-soundbank-dir <dynamicka banka> \
      --destination-soundbank-dir <cil> \
      --engine-cli build/ithaca-cli [--verify] [--force]
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile

MAGIC = b"ITHACABK"
VERSION = 1
HEADER_SIZE = 408
ENTRY_SIZE = 64
BLOB_ALIGN = 4096
NO_NAME = 0xFFFFFFFF
FMT_PCM16, FMT_PCM24, FMT_FLOAT32, FMT_PCM32 = 1, 2, 3, 4
TOOL_VERSION = "1.0"


class BakeError(Exception):
    pass


def parse_riff(path):
    """Vrati dict: channels, sample_rate, sample_format, frames,
    pcm_data_offset, entry_size. BakeError pri nepodporovanem formatu."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        if f.read(4) != b"RIFF":
            raise BakeError(f"{path}: neni RIFF")
        f.seek(4, 1)
        if f.read(4) != b"WAVE":
            raise BakeError(f"{path}: neni WAVE")
        fmt = None
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                raise BakeError(f"{path}: chybi data chunk")
            cid, csz = hdr[:4], struct.unpack("<I", hdr[4:])[0]
            if cid == b"fmt ":
                raw = f.read(min(csz, 16))
                audio_format, channels, sample_rate = struct.unpack(
                    "<HHI", raw[:8])
                bits = struct.unpack("<H", raw[14:16])[0]
                fmt = (audio_format, channels, sample_rate, bits)
                if csz > 16:
                    f.seek(csz - 16, 1)
            elif cid == b"data":
                if fmt is None:
                    raise BakeError(f"{path}: data pred fmt chunkem")
                audio_format, channels, sample_rate, bits = fmt
                if channels not in (1, 2):
                    raise BakeError(f"{path}: channels={channels}")
                if (audio_format, bits) == (1, 16):
                    sample_format = FMT_PCM16
                elif (audio_format, bits) == (1, 24):
                    sample_format = FMT_PCM24
                elif (audio_format, bits) == (3, 32):
                    sample_format = FMT_FLOAT32
                elif (audio_format, bits) == (1, 32):
                    sample_format = FMT_PCM32
                else:
                    raise BakeError(
                        f"{path}: nepodporovany format ({audio_format}/{bits})")
                data_off = f.tell()
                avail = max(0, size - data_off)
                data_sz = min(csz, avail)   # tolerantni k oriznutemu souboru
                frame_bytes = channels * (bits // 8)
                return {"channels": channels, "sample_rate": sample_rate,
                        "sample_format": sample_format,
                        "frames": data_sz // frame_bytes,
                        "pcm_data_offset": data_off, "entry_size": size}
            else:
                f.seek(csz + (csz & 1), 1)


def compute_layout(analysis, metadata_bytes):
    """Spocita offsety sekci + zaznamy. analysis: list dictu z dump JSONu,
    obohacene o RIFF pole (parse_riff). Vraci (header_fields, entries)."""
    # Predrazeni dle (midi, rms, path) — autoritativni poradi vrstev.
    entries = sorted(analysis, key=lambda a: (a["midi"], a["rms_db"], a["path"]))
    names_blob = b""
    for e in entries:
        name = os.path.basename(e["path"]).encode("utf-8")
        e["name_offset"] = len(names_blob)
        names_blob += struct.pack("<H", len(name)) + name
    metadata_offset = HEADER_SIZE
    index_offset = metadata_offset + len(metadata_bytes)
    index_size = len(entries) * ENTRY_SIZE
    names_offset = index_offset + index_size
    names_size = len(names_blob)
    blob_offset = names_offset + names_size
    blob_offset = (blob_offset + BLOB_ALIGN - 1) // BLOB_ALIGN * BLOB_ALIGN
    off = blob_offset
    for e in entries:
        e["entry_offset"] = off
        off += e["entry_size"]
        off = (off + BLOB_ALIGN - 1) // BLOB_ALIGN * BLOB_ALIGN
    # Posledni zaznam bez koncoveho paddingu (blob konci daty).
    blob_size = (entries[-1]["entry_offset"] + entries[-1]["entry_size"]
                 - blob_offset) if entries else 0
    hdr = {"metadata_offset": metadata_offset,
           "metadata_size": len(metadata_bytes),
           "index_offset": index_offset, "index_size": index_size,
           "names_offset": names_offset, "names_size": names_size,
           "blob_offset": blob_offset, "blob_size": blob_size,
           "entry_count": len(entries)}
    return hdr, entries, names_blob


def pack_entry(e):
    # 64 B dle spec: ...sample_format u16, reserved u16 (=0), frames i64...
    return struct.pack("<HHIQQIHHqfII12x",
                       e["midi"], e["channels"], e["sample_rate"],
                       e["entry_offset"], e["entry_size"],
                       e["pcm_data_offset"], e["sample_format"], 0,
                       e["frames"], e["rms_db"], e["attack_end"],
                       e["name_offset"])


def write_ithaca(out_path, analysis, bank_name, analysis_preload_ms,
                 created_at="", progress=None):
    """Zapise soundbank.ithaca. analysis: list dictu {path, midi, frames,
    sample_rate, rms_db, attack_end} (z dump JSONu). RIFF pole doplni sam.
    Velke banky se kopiruji streamem (bez nacteni do RAM)."""
    if not analysis:
        raise BakeError("prazdna analyza (zadne soubory)")
    for a in analysis:
        riff = parse_riff(a["path"])
        if riff["frames"] != a["frames"] or riff["sample_rate"] != a["sample_rate"]:
            raise BakeError(
                f"{a['path']}: neshoda CLI vs RIFF "
                f"(frames {a['frames']}/{riff['frames']}, "
                f"sr {a['sample_rate']}/{riff['sample_rate']})")
        a.update(riff)
    metadata = json.dumps({
        "bank_name": bank_name, "created_at": created_at,
        "bake_tool_version": TOOL_VERSION,
        "analysis_preload_ms": analysis_preload_ms,
        "source_format": "dynamic"}).encode("utf-8")
    hdr, entries, names_blob = compute_layout(analysis, metadata)
    index_bytes = b"".join(pack_entry(e) for e in entries)
    sha_index = hashlib.sha256(metadata + index_bytes + names_blob).digest()

    with open(out_path, "wb") as f:
        # Hlavicka s nulovym payload hashem — dopatchuje se po blobu.
        header = struct.pack("<8sII", MAGIC, VERSION, 0)
        header += struct.pack("<8Q", hdr["metadata_offset"], hdr["metadata_size"],
                              hdr["index_offset"], hdr["index_size"],
                              hdr["names_offset"], hdr["names_size"],
                              hdr["blob_offset"], hdr["blob_size"])
        header += struct.pack("<II", hdr["entry_count"], 0)
        header += sha_index + b"\x00" * 32 + b"\x00" * 256
        assert len(header) == HEADER_SIZE
        f.write(header + metadata + index_bytes + names_blob)
        f.write(b"\x00" * (hdr["blob_offset"] - f.tell()))
        sha_payload = hashlib.sha256()
        for i, e in enumerate(entries):
            assert f.tell() == e["entry_offset"]
            with open(e["path"], "rb") as src:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
                    sha_payload.update(chunk)
            pad = -(f.tell() - hdr["blob_offset"]) % BLOB_ALIGN
            if i == len(entries) - 1:
                pad = 0                      # posledni bez paddingu
            f.write(b"\x00" * pad)
            sha_payload.update(b"\x00" * pad)
            if progress:
                progress(i + 1, len(entries))
        f.seek(120)
        f.write(sha_payload.digest())


def read_ithaca_header(path):
    """Precte a zvaliduje hlavicku + index (pro verify/testy)."""
    with open(path, "rb") as f:
        hb = f.read(HEADER_SIZE)
        if len(hb) < HEADER_SIZE or hb[:8] != MAGIC:
            raise BakeError(f"{path}: neni soundbank.ithaca")
        version, flags = struct.unpack("<II", hb[8:16])
        (metadata_offset, metadata_size, index_offset, index_size,
         names_offset, names_size, blob_offset, blob_size) = struct.unpack(
            "<8Q", hb[16:80])
        entry_count = struct.unpack("<I", hb[80:84])[0]
        sha_index, sha_payload = hb[88:120], hb[120:152]
        f.seek(metadata_offset); metadata = f.read(metadata_size)
        f.seek(index_offset);    index_bytes = f.read(index_size)
        f.seek(names_offset);    names = f.read(names_size)
        if hashlib.sha256(metadata + index_bytes + names).digest() != sha_index:
            raise BakeError(f"{path}: hash indexu nesouhlasi")
        entries = []
        for i in range(entry_count):
            p = index_bytes[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]
            (midi, channels, sample_rate, entry_offset, entry_size,
             pcm_data_offset, sample_format, _rsv, frames, rms_db,
             attack_end, name_offset) = struct.unpack("<HHIQQIHHqfII", p[:52])
            entries.append({"midi": midi, "channels": channels,
                            "sample_rate": sample_rate,
                            "entry_offset": entry_offset,
                            "entry_size": entry_size,
                            "pcm_data_offset": pcm_data_offset,
                            "sample_format": sample_format, "frames": frames,
                            "rms_db": rms_db, "attack_end": attack_end,
                            "name_offset": name_offset})
        return {"version": version, "flags": flags,
                "entry_count": entry_count, "blob_offset": blob_offset,
                "blob_size": blob_size, "sha_payload": sha_payload,
                "metadata": json.loads(metadata.decode("utf-8")),
                "entries": entries}


def verify_ithaca(path, analysis):
    """Overi oba hashe + bit-exact extrakci kazdeho zaznamu proti zdrojum."""
    hdr = read_ithaca_header(path)
    # Payload hash pres cely blob.
    sha = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(hdr["blob_offset"])
        remain = hdr["blob_size"]
        while remain > 0:
            chunk = f.read(min(1 << 20, remain))
            if not chunk:
                raise BakeError(f"{path}: blob kratsi nez blob_size")
            sha.update(chunk)
            remain -= len(chunk)
    if sha.digest() != hdr["sha_payload"]:
        raise BakeError(f"{path}: hash blobu nesouhlasi")
    # Extrakce: bajty zaznamu == bajty zdrojoveho souboru.
    by_size = {}
    for a in analysis:
        by_size.setdefault((a["midi"], os.path.getsize(a["path"])), []).append(a)
    with open(path, "rb") as f:
        for e in hdr["entries"]:
            cands = by_size.get((e["midi"], e["entry_size"]), [])
            if not cands:
                raise BakeError(f"verify: zaznam midi {e['midi']} bez zdroje")
            f.seek(e["entry_offset"])
            packed = f.read(e["entry_size"])
            if not any(packed == open(c["path"], "rb").read() for c in cands):
                raise BakeError(f"verify: data midi {e['midi']} nesedi se zdrojem")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-soundbank-dir", required=True)
    ap.add_argument("--destination-soundbank-dir", required=True)
    ap.add_argument("--engine-cli", required=True)
    ap.add_argument("--preload-ms", type=int, default=150)
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    src = args.source_soundbank_dir
    has_note_dirs = any(
        n.lower().startswith("m") and n[1:].isdigit() and os.path.isdir(
            os.path.join(src, n)) for n in os.listdir(src))
    if not has_note_dirs:
        sys.exit(f"CHYBA: {src} neni dynamicka banka (zadne m<NNN>/ slozky); "
                 "fixni banku preved tools/make_dynamic_bank.sh")
    os.makedirs(args.destination_soundbank_dir, exist_ok=True)
    out = os.path.join(args.destination_soundbank_dir, "soundbank.ithaca")
    if os.path.exists(out) and not args.force:
        sys.exit(f"CHYBA: {out} existuje (pouzij --force)")

    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
        dump_path = tf.name
    try:
        r = subprocess.run(
            [args.engine_cli, "--dump-bank-index", src, "--out", dump_path,
             "--preload-ms", str(args.preload_ms)],
            capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"CHYBA: --dump-bank-index selhal:\n{r.stdout}{r.stderr}")
        with open(dump_path) as f:
            dump = json.load(f)
    finally:
        os.unlink(dump_path)
    if dump.get("bank_format") != "dynamic-velocity":
        sys.exit(f"CHYBA: zdroj neni dynamic-velocity ({dump.get('bank_format')})")

    analysis = dump["files"]
    print(f"Analyza: {len(analysis)} souboru (preload {dump['preload_ms']} ms)")
    write_ithaca(out, analysis,
                 bank_name=os.path.basename(os.path.normpath(src)),
                 analysis_preload_ms=dump["preload_ms"],
                 progress=lambda d, t: print(f"\r  pack {d}/{t}", end="",
                                             flush=True))
    print(f"\nZapsano: {out} ({os.path.getsize(out)} B)")
    if args.verify:
        verify_ithaca(out, analysis)
        print("Verify OK (hash indexu, hash blobu, bit-exact extrakce)")


if __name__ == "__main__":
    main()
```

Pozn. k `created_at`: prázdný string v testech; CLI wrapper může doplnit `time.strftime("%Y-%m-%d")` v `main()` — udělej to (v `main()` předej `created_at=time.strftime("%Y-%m-%dT%H:%M:%S")` a přidej `import time`).

- [ ] **Step 4: Ověř pass**

Run: `python3 tests/test_bake_soundbank.py -v`
Expected: všechny testy OK

- [ ] **Step 5: Commit**

```bash
chmod +x tools/bake_soundbank.py
git add tools/bake_soundbank.py tests/test_bake_soundbank.py
git commit -m "feat(tools): bake_soundbank.py — packing dynamicke banky do soundbank.ithaca"
```

---

### Task 12: End-to-end round-trip skript + registrace v ctest

**Files:**
- Create: `tests/roundtrip_packed_bank.sh`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Napiš skript**

`tests/roundtrip_packed_bank.sh`:

```bash
#!/usr/bin/env bash
# tests/roundtrip_packed_bank.sh <cesta-k-ithaca-cli> <repo-root>
# End-to-end: vygeneruj dynamickou banku → bake → dump obou variant →
# porovnej (midi, frames, sample_rate, rms_db, attack_end) v poradi.
set -euo pipefail
CLI="$1"; ROOT="$2"
WORK="$(mktemp -d /tmp/ithaca_roundtrip.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

SRC="$WORK/src"; DST="$WORK/dst"
mkdir -p "$SRC/m060" "$SRC/m064" "$DST"

python3 - "$SRC" <<'EOF'
import math, struct, sys, wave
src = sys.argv[1]
# 2 vrstvy noty 60 (ruzna amplituda → ruzna RMS) + 1 vrstva noty 64.
for path, amp, frames in [(f"{src}/m060/aa11.wav",  6000, 30000),
                          (f"{src}/m060/bb22.wav", 24000, 30000),
                          (f"{src}/m064/cc33.wav", 12000, 20000)]:
    f = wave.open(path, "wb")
    f.setnchannels(2); f.setsampwidth(2); f.setframerate(48000)
    f.writeframes(b"".join(
        struct.pack("<hh", int(amp * math.sin(i * 0.03)),
                    int(amp * math.sin(i * 0.031)))
        for i in range(frames)))
    f.close()
EOF

"$CLI" --dump-bank-index "$SRC" --out "$WORK/src.json" --log-level warn
python3 "$ROOT/tools/bake_soundbank.py" \
    --source-soundbank-dir "$SRC" --destination-soundbank-dir "$DST" \
    --engine-cli "$CLI" --verify
"$CLI" --dump-bank-index "$DST" --out "$WORK/dst.json" --log-level warn

python3 - "$WORK/src.json" "$WORK/dst.json" <<'EOF'
import json, sys
a = json.load(open(sys.argv[1]))
b = json.load(open(sys.argv[2]))
assert b["bank_format"] == "packed-ithaca", b["bank_format"]
ta = [(f["midi"], f["frames"], f["sample_rate"], f["attack_end"], f["rms_db"])
      for f in a["files"]]
tb = [(f["midi"], f["frames"], f["sample_rate"], f["attack_end"], f["rms_db"])
      for f in b["files"]]
assert len(ta) == len(tb) == 3, (len(ta), len(tb))
for x, y in zip(ta, tb):
    assert x[:4] == y[:4], (x, y)
    # rms: f32 round-trip pres index — dump pouziva %.9g, mel by sedet presne;
    # tolerance pro jistotu.
    assert abs(x[4] - y[4]) < 1e-4, (x, y)
print("ROUNDTRIP OK")
EOF
echo "roundtrip_packed_bank: OK"
```

- [ ] **Step 2: Registruj v ctest**

Do `tests/CMakeLists.txt` na konec:

```cmake
# End-to-end round-trip pakovane banky (bake python + CLI dump obou variant).
add_test(NAME roundtrip_packed_bank
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/roundtrip_packed_bank.sh
                 $<TARGET_FILE:ithaca-cli> ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 3: Ověř pass**

```bash
chmod +x tests/roundtrip_packed_bank.sh
cmake --build build --parallel 8
ctest --test-dir build -R roundtrip_packed_bank --output-on-failure
```

Expected: `roundtrip_packed_bank ... Passed` (výstup obsahuje `ROUNDTRIP OK` a `Verify OK`)

- [ ] **Step 4: Commit**

```bash
git add tests/roundtrip_packed_bank.sh tests/CMakeLists.txt
git commit -m "test: end-to-end roundtrip pakovane banky (bake + dump compare)"
```

---

### Task 13: Dokumentace

**Files:**
- Create: `docs/bank-format-packed.md`
- Modify: `docs/reference/F-loader.md`, `docs/config-file.md`, `docs/superpowers/specs/2026-06-10-packed-soundbank-design.md`

- [ ] **Step 1: Napiš `docs/bank-format-packed.md`**

Obsah: zkopíruj sekci „1. Formát souboru" ze specu (hlavička 408 B, index 64 B, tabulky polí, hash sémantika, names tabulka, blob 4096 align) a doplň:
- jak banku vyrobit (`tools/bake_soundbank.py` — přesný příkaz s argumenty),
- jak ji aplikace detekuje (`soundbank.ithaca` v adresáři banky, priorita nade vším),
- `sample_format` kódy 1=PCM16, 2=PCM24, 3=float32, **4=PCM32**,
- omezení v1: flags musí být 0 (šifra/podpis = v2), vstup bake jen dynamický formát,
- chování při poškození (ERROR log + prázdná banka).

- [ ] **Step 2: Aktualizuj spec (drobné odchylky implementace)**

V `docs/superpowers/specs/2026-06-10-packed-soundbank-design.md`:
- sekce 1, index: `sample_format` doplň kód `4=PCM32` (wav_reader podporuje i 32-bit PCM),
- sekce 4 (CLI): JSON jde do souboru přes `--out <json>` (ne na stdout — logy by výstup znečistily); `rms_db` se tiskne `%.9g` (přesný float round-trip),
- hlavičku „Stav:" změň na `implementováno`.

- [ ] **Step 3: Aktualizuj `docs/reference/F-loader.md`**

Přidej sekci „Pakovaná banka (packed-ithaca)" popisující: detekci (scanBank stupeň 0, `kIthacaFileName`), load path (`loadPackedBank` — kostra z indexu, žádná analýza/sort, baked hodnoty autoritativní), čtecí vrstvu (`IFileHandle`/pread, `readSampleRange` dispatcher, `StreamRequest.file`) a chybové stavy (openIthacaBank validace → ERROR + prázdná banka). Odkazy na `engine/sample/ithaca_bank.cpp`, `engine/io/sample_read.cpp`, `docs/bank-format-packed.md`.

- [ ] **Step 4: Aktualizuj `docs/config-file.md`**

U `bank_path`/`bank_search_dir` doplň větu: adresář banky může místo WAV souborů obsahovat `soundbank.ithaca` (pakovaná banka, `docs/bank-format-packed.md`) — výběr v GUI i CLI funguje beze změny.

- [ ] **Step 5: Commit**

```bash
git add docs/bank-format-packed.md docs/reference/F-loader.md docs/config-file.md docs/superpowers/specs/2026-06-10-packed-soundbank-design.md
git commit -m "docs: format pakovane banky + aktualizace F-loader/config-file"
```

---

### Task 14: Finální verifikace

- [ ] **Step 1: Celá testovací sada + smoke**

```bash
make test
make smoke
```

Expected: všechny testy PASS, smoke OK.

- [ ] **Step 2: Orientační perf měření (manuální, je-li k dispozici reálná banka)**

```bash
# adresarova vs pakovana varianta teze banky:
time ./build/ithaca-cli --inspect <dir-banka>      --log-level warn
python3 tools/bake_soundbank.py --source-soundbank-dir <dir-banka> \
    --destination-soundbank-dir /tmp/packed_bank --engine-cli build/ithaca-cli
time ./build/ithaca-cli --inspect /tmp/packed_bank --log-level warn
```

Výsledek (časy obou variant) zapiš do popisu PR / poznámky v plánu. Očekávání: packed rychlejší (1 fd, žádný scan, žádná analýza).

- [ ] **Step 3: Odškrtej checkboxy plánu, commit**

```bash
git add docs/superpowers/plans/2026-06-11-packed-soundbank.md
git commit -m "docs(plan): packed soundbank — plan dokoncen"
```

Následuje skill `superpowers:finishing-a-development-branch` (merge/PR rozhodnutí je na uživateli).

---

## Poznámky pro implementátora

- **Bit-exact zásada:** dispatcher (`readSampleRange`) musí pro WAV cesty jen delegovat — žádná změna chování stávajících bank (hlídá `test_render_regression`).
- **Žádný sort u packed:** pořadí indexu je autoritativní; `std::sort` není stabilní a u shodných RMS by rozbil bake pořadí.
- **`StreamRequest` s `SampleFile`:** shared_ptr copy na audio threadu = atomic refcount (bez zámku/alokace) — stejná RT úvaha jako stávající `std::string path` (viz komentář v stream_engine.h).
- **Index `rms_db` je f32**; CLI dump tiskne `%.9g` (přesný round-trip float→text→float). V Tasku 10 použij `%.9g`, ne `%.6f`.
- Komentáře v kódu česky bez diakritiky (konvence enginu).




