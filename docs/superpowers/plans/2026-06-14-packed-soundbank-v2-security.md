# Pakovaná soundbanka v2 — zabezpečení — implementační plán

> ## ✅ DOKONČENO (2026-06-15)
>
> Všech 12 tasků hotovo a zrevidováno (spec + quality per task + finální
> holistická revize → ready to merge). 44/44 ctest + 18/18 python testů + smoke
> zelené. Cross-language parita ověřena byte-exact (RFC 4231 vektory v obou
> jazycích) i end-to-end přes reálnou `ithaca-cli` (`roundtrip_packed_bank`:
> python bake se secretem → engine dešifruje → tamper odmítnut). Master secret
> potvrzeně **není v gitu**. Threat model: odrazení + dohledatelnost (ne DRM).
> Větev `feat/packed-soundbank-v2-security` připravena k mergi/PR.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Šifrování pakované banky `soundbank.ithaca` + licenční soubor s identitou kupce kryptograficky vázaný do banky (HMAC-SHA256 + SHA-256 CTR keystream, nula nových závislostí), s `LicenseInvalid` overlay v GUI.

**Architecture:** Krypto primitiva (HMAC, KDF, keystream) postavená nad stávající `Sha256`, zrcadlená v pythonu (stdlib `hmac`). Master secret = jeden gitignored soubor idempotentně generovaný buildem, zkompilovaný do appky i čtený python bakem. Šifrování je zapouzdřené v `DecryptingFileHandle` pod `IFileHandle::readAt`, takže loader/streaming/voice/audio vlákno se nemění. `openIthacaBank` u `flags bit0` načte license, odvodí klíče, ověří MAC, obalí handle; selhání = `LicenseInvalid` → anglický overlay s „Click to continue".

**Tech Stack:** C++20 (CMake, doctest), python3 stdlib (`hmac`, `hashlib`, `secrets`) + numpy (už je). Spec: `docs/superpowers/specs/2026-06-14-packed-soundbank-v2-security-design.md`. Větev: `feat/packed-soundbank-v2-security`.

**Build/test:**
- build: `cmake --build build --parallel 8`
- jeden C++ test: `./build/tests/<nazev>`
- celá sada: `ctest --test-dir build -j 8 --output-on-failure`
- python: `python3 tests/test_ithaca_crypto.py` / `python3 tests/test_bake_soundbank.py`

**Konvence:** komentáře v C++/pythonu česky bez diakritiky; commity malými písmeny bez diakritiky.

---

## Struktura souborů

| Soubor | Akce | Zodpovědnost |
|---|---|---|
| `engine/util/ithaca_crypto.{h,cpp}` | nový | C++ HMAC-SHA256, deriveKey (KDF), keystreamXor (CTR) — nad `Sha256` |
| `tools/ithaca_crypto.py` | nový | python zrcadlo téhož (hmac/hashlib); sdílí bake i testy |
| `tools/gen-bank-secret.py` | nový | idempotentní generace `secret/bank_secret.key` |
| `engine/io/file_handle.{h,cpp}` | úprava | `DecryptingFileHandle` (obal nad IFileHandle, dešifruje blob rozsah) |
| `engine/sample/ithaca_bank.{h,cpp}` | úprava | `openIthacaBank` overload se secretem; license load + MAC + handle wrap; `LicenseInvalid` |
| `engine/sample/sample_store.{h,cpp}` | úprava | `loadPackedBank` propaguje license error; `BankLoadProgress.license_invalid` |
| `engine/engine.{h,cpp}` | úprava | předání kompilovaného secretu do loaderu (nebo loader čte global) |
| `tools/bake_soundbank.py` | úprava | `--license` režim: license.ithaca + šifrování + tag |
| `app/gui/main.cpp` (overlay) | úprava | `LicenseInvalid` stav: anglický text + click-to-continue |
| `CMakeLists.txt`, `Makefile` | úprava | generace + kompilace `bank_secret_generated.h` |
| `.gitignore` | úprava | `secret/`, `*bank_secret_generated.h` |
| `tests/test_ithaca_crypto.{cpp,py}` | nový | krypto vektory (RFC 4231) + parita |
| `tests/test_decrypting_handle.cpp` | nový | DecryptingFileHandle |
| `tests/ithaca_test_blob.h` | úprava | volitelné šifrování fixture (C++ test secret) |
| `tests/test_ithaca_bank.cpp` | úprava | licensed happy path + tamper |
| `tests/roundtrip_packed_bank.sh` | úprava | licensed varianta |

**Testovací secret:** krypto funkce berou secret jako parametr (pure). `openIthacaBank(path)` čte kompilovaný `kBankSecret`; přidá se overload `openIthacaBank(path, secret32)` pro testy s pevným secretem. C++ testy si šifrovaný fixture postaví v C++ (rozšířený `buildTestIthaca`) → self-contained, bez pythonu. Cross-language parita (Task 11) je zvlášť.

---

### Task 1: C++ krypto primitiva (HMAC-SHA256, KDF, keystream)

**Files:**
- Create: `engine/util/ithaca_crypto.h`, `engine/util/ithaca_crypto.cpp`
- Test: `tests/test_ithaca_crypto.cpp`
- Modify: `CMakeLists.txt` (přidat `engine/util/ithaca_crypto.cpp` za `engine/util/sha256.cpp`), `tests/CMakeLists.txt`

- [ ] **Step 1: Napiš failing test** — `tests/test_ithaca_crypto.cpp`:

```cpp
// tests/test_ithaca_crypto.cpp
// HMAC-SHA256 proti RFC 4231 + keystream determinismus. Parita s pythonem
// (tools/ithaca_crypto.py) je zajistena shodnymi vektory v obou jazycich.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "util/ithaca_crypto.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace ithaca;

namespace {
std::string hex(const std::array<uint8_t, 32>& d) {
    char o[65];
    for (int i = 0; i < 32; ++i) std::snprintf(o + i * 2, 3, "%02x", d[i]);
    return std::string(o, 64);
}
} // namespace

TEST_CASE("hmacSha256 RFC 4231 test case 2") {
    // key="Jefe", data="what do ya want for nothing?"
    auto m = hmacSha256((const uint8_t*)"Jefe", 4,
                        "what do ya want for nothing?", 28);
    CHECK(hex(m) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("deriveKey = HMAC(secret, domain || msg)") {
    uint8_t secret[32]; std::memset(secret, 0x42, 32);
    // Rucni reference: HMAC(secret, "enc" + "LIC")
    std::string m = std::string("enc") + "LIC";
    auto ref = hmacSha256(secret, 32, m.data(), m.size());
    auto got = deriveKey(secret, "enc", (const uint8_t*)"LIC", 3);
    CHECK(hex(got) == hex(ref));
}

TEST_CASE("keystreamXor je involuce (XOR dvakrat = original) a offset-aware") {
    uint8_t key[32]; std::memset(key, 0x11, 32);
    uint8_t nonce[32]; std::memset(nonce, 0x22, 32);
    std::vector<uint8_t> data(200);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)(i * 7 + 3);
    std::vector<uint8_t> orig = data;
    // Zasifruj cely blok od blob-relativni pozice 0.
    keystreamXor(key, nonce, /*p0=*/0, data.data(), data.size());
    CHECK(data != orig);                       // neco se zmenilo
    keystreamXor(key, nonce, /*p0=*/0, data.data(), data.size());
    CHECK(data == orig);                        // XOR dvakrat = original

    // Vyrez [50, 50+60) sifrovany samostatne == tentyz vyrez z plneho sifrovani.
    std::vector<uint8_t> full = orig;
    keystreamXor(key, nonce, 0, full.data(), full.size());
    std::vector<uint8_t> part(orig.begin() + 50, orig.begin() + 110);
    keystreamXor(key, nonce, /*p0=*/50, part.data(), part.size());
    for (size_t i = 0; i < part.size(); ++i)
        CHECK(part[i] == full[50 + i]);         // random-access shoda
}
```

Registrace v `tests/CMakeLists.txt` (na konec):
```cmake
add_executable(test_ithaca_crypto test_ithaca_crypto.cpp)
target_link_libraries(test_ithaca_crypto PRIVATE ithaca_core doctest)
add_test(NAME test_ithaca_crypto COMMAND test_ithaca_crypto)
```

- [ ] **Step 2: Build → FAIL** (`util/ithaca_crypto.h` chybí). Run: `cmake --build build --parallel 8`.

- [ ] **Step 3: Implementuj** `engine/util/ithaca_crypto.h`:

```cpp
#pragma once
// engine/util/ithaca_crypto.h
// Symetricka krypto pro pakovanou banku v2 — postavena VYHRADNE nad nasi
// Sha256 (zadna externi zavislost). HMAC-SHA256 (RFC 2104), KDF a CTR
// keystream. Python zrcadlo: tools/ithaca_crypto.py (stejne vektory v testech).

#include <array>
#include <cstddef>
#include <cstdint>

namespace ithaca {

// HMAC-SHA256 (RFC 2104). key_len libovolne; pro nase 32B klice trivialni.
std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t key_len,
                                   const void* msg, size_t msg_len);

// Odvozeni klice: HMAC(secret32, domain || msg). domain je C-string (bez \0).
std::array<uint8_t, 32> deriveKey(const uint8_t secret[32], const char* domain,
                                  const uint8_t* msg, size_t msg_len);

// CTR keystream XOR in-place. p0 = blob-relativni pozice prvniho bajtu v `buf`.
// keystream_block(i) = HMAC(key, nonce32 || u64_le(i)); blok i = p/32.
// Dvoji volani se stejnymi parametry vrati original (involuce).
void keystreamXor(const uint8_t key[32], const uint8_t nonce[32],
                  uint64_t p0, uint8_t* buf, size_t n);

} // namespace ithaca
```

`engine/util/ithaca_crypto.cpp`:

```cpp
// engine/util/ithaca_crypto.cpp — viz ithaca_crypto.h.
#include "util/ithaca_crypto.h"

#include "util/sha256.h"

#include <cstring>

namespace ithaca {

namespace {
constexpr size_t kBlock = 64;   // SHA-256 block size pro HMAC padding
} // namespace

std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t key_len,
                                   const void* msg, size_t msg_len) {
    uint8_t k[kBlock];
    std::memset(k, 0, kBlock);
    if (key_len > kBlock) {
        auto kh = Sha256::hash(key, key_len);   // dlouhy klic → hash
        std::memcpy(k, kh.data(), 32);
    } else {
        std::memcpy(k, key, key_len);
    }
    uint8_t ipad[kBlock], opad[kBlock];
    for (size_t i = 0; i < kBlock; ++i) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    Sha256 inner;
    inner.update(ipad, kBlock);
    inner.update(msg, msg_len);
    auto ih = inner.finish();
    Sha256 outer;
    outer.update(opad, kBlock);
    outer.update(ih.data(), ih.size());
    return outer.finish();
}

std::array<uint8_t, 32> deriveKey(const uint8_t secret[32], const char* domain,
                                  const uint8_t* msg, size_t msg_len) {
    // message = domain || msg ; key = secret. Zretez bez kopirovani do jednoho
    // bufferu: HMAC potrebuje souvisly msg, takze slozime.
    const size_t dlen = std::strlen(domain);
    std::vector<uint8_t> m(dlen + msg_len);
    std::memcpy(m.data(), domain, dlen);
    if (msg_len) std::memcpy(m.data() + dlen, msg, msg_len);
    return hmacSha256(secret, 32, m.data(), m.size());
}

void keystreamXor(const uint8_t key[32], const uint8_t nonce[32],
                  uint64_t p0, uint8_t* buf, size_t n) {
    if (n == 0) return;
    uint64_t p = p0;
    size_t done = 0;
    while (done < n) {
        const uint64_t block_i = p / 32;
        // msg = nonce(32) || u64_le(block_i)
        uint8_t m[40];
        std::memcpy(m, nonce, 32);
        for (int b = 0; b < 8; ++b) m[32 + b] = (uint8_t)(block_i >> (8 * b));
        auto ks = hmacSha256(key, 32, m, sizeof(m));
        size_t off_in_block = (size_t)(p % 32);
        while (off_in_block < 32 && done < n) {
            buf[done] = (uint8_t)(buf[done] ^ ks[off_in_block]);
            ++off_in_block; ++done; ++p;
        }
    }
}
```

(Přidej `#include <vector>` do `.cpp` kvůli `std::vector` v `deriveKey`.)

Do `CMakeLists.txt` přidej `engine/util/ithaca_crypto.cpp` za `engine/util/sha256.cpp`.

- [ ] **Step 4: Build + test** — Run: `cmake --build build --parallel 8 && ./build/tests/test_ithaca_crypto`. Expected: 3/3 passed.

- [ ] **Step 5: Commit**
```bash
git add engine/util/ithaca_crypto.h engine/util/ithaca_crypto.cpp tests/test_ithaca_crypto.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(crypto): HMAC-SHA256 + KDF + CTR keystream nad nasi sha256"
```

---

### Task 2: Python krypto zrcadlo + parita

**Files:**
- Create: `tools/ithaca_crypto.py`, `tests/test_ithaca_crypto.py`

- [ ] **Step 1: Napiš failing test** — `tests/test_ithaca_crypto.py`:

```python
#!/usr/bin/env python3
# tests/test_ithaca_crypto.py
# Python krypto zrcadlo: HMAC/KDF/keystream proti TYMZ vektorum jako C++
# (tests/test_ithaca_crypto.cpp) → garantuje cross-language paritu.
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import ithaca_crypto as ic


class TestCrypto(unittest.TestCase):
    def test_hmac_rfc4231_tc2(self):
        m = ic.hmac_sha256(b"Jefe", b"what do ya want for nothing?")
        self.assertEqual(
            m.hex(),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")

    def test_derive_key(self):
        secret = b"\x42" * 32
        ref = ic.hmac_sha256(secret, b"enc" + b"LIC")
        got = ic.derive_key(secret, "enc", b"LIC")
        self.assertEqual(got, ref)

    def test_keystream_involution_and_offset(self):
        key = b"\x11" * 32
        nonce = b"\x22" * 32
        orig = bytes((i * 7 + 3) & 0xFF for i in range(200))
        enc = ic.keystream_xor(key, nonce, 0, orig)
        self.assertNotEqual(enc, orig)
        self.assertEqual(ic.keystream_xor(key, nonce, 0, enc), orig)
        full = ic.keystream_xor(key, nonce, 0, orig)
        part = ic.keystream_xor(key, nonce, 50, orig[50:110])
        self.assertEqual(part, full[50:110])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run → FAIL** (`ModuleNotFoundError: ithaca_crypto`). Run: `python3 tests/test_ithaca_crypto.py`.

- [ ] **Step 3: Implementuj** `tools/ithaca_crypto.py`:

```python
#!/usr/bin/env python3
"""ithaca_crypto.py — symetricka krypto pro pakovanou banku v2.

Zrcadli engine/util/ithaca_crypto.cpp (stejne vektory v testech garantuji
cross-language paritu). HMAC-SHA256 ze stdlib, CTR keystream, KDF.
"""
import hashlib
import hmac
import struct

NONCE_LEN = 32
KEY_LEN = 32


def hmac_sha256(key: bytes, msg: bytes) -> bytes:
    return hmac.new(key, msg, hashlib.sha256).digest()


def derive_key(secret: bytes, domain: str, msg: bytes) -> bytes:
    # HMAC(secret, domain || msg) — zrcadlo C++ deriveKey.
    return hmac_sha256(secret, domain.encode("ascii") + msg)


def keystream_xor(key: bytes, nonce: bytes, p0: int, data: bytes) -> bytes:
    # CTR keystream XOR. p0 = blob-relativni pozice prvniho bajtu data.
    out = bytearray(len(data))
    p = p0
    done = 0
    n = len(data)
    while done < n:
        block_i = p // 32
        ks = hmac_sha256(key, nonce + struct.pack("<Q", block_i))
        off = p % 32
        while off < 32 and done < n:
            out[done] = data[done] ^ ks[off]
            off += 1
            done += 1
            p += 1
    return bytes(out)
```

- [ ] **Step 4: Run → PASS.** Run: `python3 tests/test_ithaca_crypto.py -v`. Expected: 3/3 OK.

> Parita: oba testy používají identické vektory (RFC 4231 TC2, `secret=0x42*32`,
> `key=0x11*32`/`nonce=0x22*32` ramp). Shoda v obou jazycích = C++ a python
> produkují bit-identický keystream i MAC.

- [ ] **Step 5: Commit**
```bash
chmod +x tools/ithaca_crypto.py
git add tools/ithaca_crypto.py tests/test_ithaca_crypto.py
git commit -m "feat(tools): python krypto zrcadlo + parita vektory s C++"
```

---

### Task 3: Generátor master secretu

**Files:**
- Create: `tools/gen-bank-secret.py`
- Modify: `.gitignore`

- [ ] **Step 1: Implementuj** `tools/gen-bank-secret.py` (testuje se ručně v kroku 2 — generátor je I/O utilita):

```python
#!/usr/bin/env python3
"""gen-bank-secret.py <out-path> — idempotentni generace master secretu.

Pokud soubor neexistuje, vytvori 32 nahodnych bajtu + vypise hlasite varovani.
Pokud existuje, NIKDY neprepise (idempotentni). Volaji Makefile i CMake; oba
buildy tak maji secret bez rucniho kroku, na vsech platformach (jen python3).
"""
import os
import secrets
import sys

SECRET_LEN = 32


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen-bank-secret.py <out-path>")
    out = sys.argv[1]
    if os.path.exists(out):
        return  # idempotentni — nikdy neprepisuj
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(secrets.token_bytes(SECRET_LEN))
    sys.stderr.write(
        "\n*** VYGENEROVAN NOVY bank secret: %s ***\n"
        "    ZAZALOHUJ HO. Drive vydane licensed banky s nim nepujdou otevrit;\n"
        "    produkcni secret je dlouhozijici aktivum (neztrat, nepregeneruj).\n\n"
        % out)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Ověř idempotenci ručně**
```bash
chmod +x tools/gen-bank-secret.py
rm -f /tmp/sk.key
python3 tools/gen-bank-secret.py /tmp/sk.key   # vypise varovani
wc -c /tmp/sk.key                              # 32
A=$(shasum /tmp/sk.key)
python3 tools/gen-bank-secret.py /tmp/sk.key   # ticho (no-op)
B=$(shasum /tmp/sk.key)
[ "$A" = "$B" ] && echo "IDEMPOTENT OK" || echo "FAIL prepsal"
rm -f /tmp/sk.key
```
Expected: první volání 32 B + varování na stderr; druhé ticho; `IDEMPOTENT OK`.

- [ ] **Step 3: .gitignore** — přidej řádky:
```
secret/
bank_secret_generated.h
```

- [ ] **Step 4: Commit**
```bash
git add tools/gen-bank-secret.py .gitignore
git commit -m "feat(tools): idempotentni generator master secretu + gitignore"
```

---

### Task 4: Build injekce secretu (CMake + Makefile)

**Files:**
- Modify: `CMakeLists.txt`, `Makefile`
- (generuje: `secret/bank_secret.key` gitignored, `build/.../bank_secret_generated.h`)

- [ ] **Step 1: CMake — generace secretu + headeru.** Do `CMakeLists.txt` PŘED `add_library(ithaca_core ...)` přidej:

```cmake
# -- Master secret pro pakovanou banku v2 (gitignored, build-time) --
# Idempotentni: gen-bank-secret.py vytvori jen kdyz chybi. Z nej se vygeneruje
# bank_secret_generated.h (32 B jako C pole) do build adresare.
set(ITHACA_BANK_SECRET_FILE "${CMAKE_SOURCE_DIR}/secret/bank_secret.key")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen-bank-secret.py
            ${ITHACA_BANK_SECRET_FILE}
    RESULT_VARIABLE _sk_rc)
if(NOT _sk_rc EQUAL 0)
    message(FATAL_ERROR "gen-bank-secret.py selhal (rc=${_sk_rc})")
endif()
file(READ "${ITHACA_BANK_SECRET_FILE}" _sk_hex HEX)
string(REGEX MATCHALL "([0-9a-f][0-9a-f])" _sk_bytes "${_sk_hex}")
string(JOIN ",0x" _sk_join ${_sk_bytes})
set(_sk_arr "0x${_sk_join}")
file(WRITE "${CMAKE_BINARY_DIR}/generated/bank_secret_generated.h"
"#pragma once\n// AUTO-GENEROVANO z secret/bank_secret.key — NEcommitovat.\n"
"#include <cstdint>\nnamespace ithaca {\n"
"inline constexpr uint8_t kBankSecret[32] = {${_sk_arr}};\n} // namespace ithaca\n")
```

A za `target_include_directories(ithaca_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)` přidej:
```cmake
target_include_directories(ithaca_core PUBLIC ${CMAKE_BINARY_DIR}/generated)
```

- [ ] **Step 2: Makefile — prerekvizita secretu.** V `Makefile` přidej pravidlo a navěs ho na `configure`:

```make
SECRET_FILE ?= secret/bank_secret.key

.PHONY: bank-secret
bank-secret:
	@python3 tools/gen-bank-secret.py $(SECRET_FILE)
```

A do receptu `configure` přidej jako PRVNÍ řádek (před `cmake -S`):
```make
	@python3 tools/gen-bank-secret.py $(SECRET_FILE)
```
(CMake to dělá taky — idempotence zajistí, že se negeneruje dvakrát.)

- [ ] **Step 3: Smoke test generace** — Run:
```bash
rm -rf build secret
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
test -f secret/bank_secret.key && echo "SECRET OK ($(wc -c < secret/bank_secret.key) B)"
test -f build/generated/bank_secret_generated.h && echo "HEADER OK"
grep -c "kBankSecret" build/generated/bank_secret_generated.h
```
Expected: `SECRET OK (32 B)`, `HEADER OK`, `1`. Pak `cmake --build build --parallel 8` projde (header se zatím nikde nepoužívá — jen se generuje).

- [ ] **Step 4: Commit**
```bash
git add CMakeLists.txt Makefile
git commit -m "build: generace + injekce master secretu (CMake header + Makefile prereq)"
```

---

### Task 5: DecryptingFileHandle

**Files:**
- Modify: `engine/io/file_handle.h`, `engine/io/file_handle.cpp`
- Test: `tests/test_decrypting_handle.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Napiš failing test** — `tests/test_decrypting_handle.cpp`:

```cpp
// tests/test_decrypting_handle.cpp
// DecryptingFileHandle: bajty v rozsahu blobu se desifruji keystreamem,
// bajty mimo (hlavicka/index) projdou beze zmeny (pass-through).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "io/file_handle.h"
#include "util/ithaca_crypto.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ithaca;

namespace {
struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { if (!path.empty()) std::remove(path.c_str()); }
};
} // namespace

TEST_CASE("DecryptingFileHandle desifruje blob, mimo blob pass-through") {
    const uint64_t blob_off = 100;
    const uint64_t blob_size = 300;
    uint8_t key[32];   std::memset(key, 0x11, 32);
    uint8_t nonce[32]; std::memset(nonce, 0x22, 32);

    // Plaintext soubor: [0,100) hlavicka, [100,400) "blob".
    std::vector<uint8_t> plain(400);
    for (size_t i = 0; i < plain.size(); ++i) plain[i] = (uint8_t)(i & 0xFF);
    // Na disk zapis: hlavicka plaintext + zasifrovany blob.
    std::vector<uint8_t> disk = plain;
    keystreamXor(key, nonce, /*p0=*/0, disk.data() + blob_off, (size_t)blob_size);

    std::string p = "/tmp/ithaca_dec_handle.bin";
    TempFile guard{p};
    { std::FILE* f = std::fopen(p.c_str(), "wb");
      REQUIRE(f); std::fwrite(disk.data(), 1, disk.size(), f); std::fclose(f); }

    auto raw = openFileHandle(p);
    REQUIRE(raw != nullptr);
    auto dec = makeDecryptingFileHandle(raw, key, nonce, blob_off, blob_size);
    REQUIRE(dec != nullptr);

    // Cteni hlavicky (mimo blob) → plaintext.
    uint8_t hb[16];
    REQUIRE(dec->readAt(0, hb, 16));
    for (int i = 0; i < 16; ++i) CHECK(hb[i] == (uint8_t)(i & 0xFF));

    // Cteni vyrezu blobu [150,150+80) → desifrovano == puvodni plain.
    uint8_t bb[80];
    REQUIRE(dec->readAt(150, bb, 80));
    for (int i = 0; i < 80; ++i) CHECK(bb[i] == plain[150 + i]);

    // Cteni pres hranici blobu [90,90+30) → cast plaintext, cast desifrovana.
    uint8_t cb[30];
    REQUIRE(dec->readAt(90, cb, 30));
    for (int i = 0; i < 30; ++i) CHECK(cb[i] == plain[90 + i]);
}
```

Registrace v `tests/CMakeLists.txt`:
```cmake
add_executable(test_decrypting_handle test_decrypting_handle.cpp)
target_link_libraries(test_decrypting_handle PRIVATE ithaca_core doctest)
add_test(NAME test_decrypting_handle COMMAND test_decrypting_handle)
```

- [ ] **Step 2: Build → FAIL** (`makeDecryptingFileHandle` neexistuje).

- [ ] **Step 3: Implementuj.** Do `engine/io/file_handle.h` před `} // namespace ithaca` přidej:

```cpp
// Obal nad IFileHandle: bajty v rozsahu blobu [blob_off, blob_off+blob_size)
// desifruje CTR keystreamem (key+nonce), bajty mimo projdou beze zmeny
// (plaintext hlavicka/index/names). Pouziva pakovana banka v2; nad readAt se
// nic nemeni (loader/streaming ctou plaintext).
std::shared_ptr<IFileHandle> makeDecryptingFileHandle(
    std::shared_ptr<IFileHandle> inner,
    const uint8_t key[32], const uint8_t nonce[32],
    uint64_t blob_off, uint64_t blob_size);
```

Do `engine/io/file_handle.cpp` přidej `#include "util/ithaca_crypto.h"`, `#include <array>` a v anonymním namespace třídu + na konec (před `} // namespace ithaca`) factory:

```cpp
namespace {
class DecryptingFileHandle : public IFileHandle {
public:
    DecryptingFileHandle(std::shared_ptr<IFileHandle> inner,
                         const uint8_t key[32], const uint8_t nonce[32],
                         uint64_t blob_off, uint64_t blob_size)
        : inner_(std::move(inner)), blob_off_(blob_off), blob_size_(blob_size) {
        std::memcpy(key_.data(), key, 32);
        std::memcpy(nonce_.data(), nonce, 32);
    }
    bool readAt(uint64_t off, void* buf, size_t n) const override {
        if (!inner_->readAt(off, buf, n)) return false;
        // Prekryv [off, off+n) s blobem [blob_off_, blob_off_+blob_size_)?
        const uint64_t bend = blob_off_ + blob_size_;
        const uint64_t s = (off > blob_off_) ? off : blob_off_;
        const uint64_t e = (off + n < bend) ? (off + n) : bend;
        if (s < e) {   // desifruj jen prunik
            uint8_t* p = static_cast<uint8_t*>(buf) + (s - off);
            keystreamXor(key_.data(), nonce_.data(), s - blob_off_, p,
                         (size_t)(e - s));
        }
        return true;
    }
    uint64_t size() const override { return inner_->size(); }
private:
    std::shared_ptr<IFileHandle> inner_;
    std::array<uint8_t, 32> key_, nonce_;
    uint64_t blob_off_, blob_size_;
};
} // namespace

std::shared_ptr<IFileHandle> makeDecryptingFileHandle(
    std::shared_ptr<IFileHandle> inner,
    const uint8_t key[32], const uint8_t nonce[32],
    uint64_t blob_off, uint64_t blob_size) {
    if (!inner) return nullptr;
    return std::make_shared<DecryptingFileHandle>(std::move(inner), key, nonce,
                                                  blob_off, blob_size);
}
```

(Přidej `#include <cstring>` a `#include <array>` do `.cpp`, pokud chybí.)

- [ ] **Step 4: Build + test** — `cmake --build build --parallel 8 && ./build/tests/test_decrypting_handle`. Expected: passed. Pak `ctest --test-dir build -j 8 | tail -3` (nic se nerozbilo).

- [ ] **Step 5: Commit**
```bash
git add engine/io/file_handle.h engine/io/file_handle.cpp tests/test_decrypting_handle.cpp tests/CMakeLists.txt
git commit -m "feat(io): DecryptingFileHandle — desifrovani blob rozsahu pod readAt"
```

---

### Task 6: license.ithaca + licensed bake (python)

**Files:**
- Modify: `tools/bake_soundbank.py`, `tests/test_bake_soundbank.py`

Licensed bake = zašifruj blob CTR keystreamem + zapiš `license.ithaca` (deterministický JSON) + `flags bit0`/`cipher_id`/`nonce`/`hmac_tag` do hlavičky. Konstanty hlavičky (offset 152 cipher_id, 154 nonce, 186 hmac_tag) dle specu §4.

- [ ] **Step 1: Napiš failing test** — přidej do `tests/test_bake_soundbank.py` novou třídu:

```python
class TestLicensedBake(unittest.TestCase):
    SECRET = b"\x07" * 32

    def _src(self, d):
        src = os.path.join(d, "src")
        os.makedirs(os.path.join(src, "m060"))
        make_const_wav(os.path.join(src, "m060", "a.wav"), 4096, 48000, 8000)
        make_const_wav(os.path.join(src, "m060", "b.wav"), 4096, 48000, 20000)
        return src

    def test_licensed_bake_header_and_license_file(self):
        with tempfile.TemporaryDirectory() as d:
            src = self._src(d)
            out_dir = os.path.join(d, "out")
            os.makedirs(out_dir)
            info = {"bank_name": "t", "owner_email": "a@b.cz",
                    "owner_name": "A B", "transaction_id": "TX1",
                    "issued_at": "2026-06-14T00:00:00"}
            bake.bake_licensed(src, out_dir, self.SECRET, info, preload_ms=150)
            out = os.path.join(out_dir, "soundbank.ithaca")
            lic = os.path.join(out_dir, "license.ithaca")
            self.assertTrue(os.path.exists(lic))
            hdr = bake.read_ithaca_header(out)
            self.assertEqual(hdr["flags"] & 1, 1)          # encrypted bit
            self.assertEqual(hdr["cipher_id"], 1)
            self.assertEqual(len(hdr["nonce"]), 32)
            self.assertEqual(len(hdr["hmac_tag"]), 32)

    def test_licensed_roundtrip_decrypt(self):
        with tempfile.TemporaryDirectory() as d:
            src = self._src(d)
            out_dir = os.path.join(d, "out"); os.makedirs(out_dir)
            info = {"bank_name": "t", "owner_email": "a@b.cz",
                    "owner_name": "A B", "transaction_id": "TX1",
                    "issued_at": "2026-06-14T00:00:00"}
            bake.bake_licensed(src, out_dir, self.SECRET, info, preload_ms=150)
            # verify_licensed: overi hmac_tag + desifruje + bit-exact se zdroji.
            bake.verify_licensed(out_dir, self.SECRET)   # nesmi vyhodit

    def test_edited_license_breaks_verify(self):
        with tempfile.TemporaryDirectory() as d:
            src = self._src(d)
            out_dir = os.path.join(d, "out"); os.makedirs(out_dir)
            info = {"bank_name": "t", "owner_email": "a@b.cz",
                    "owner_name": "A B", "transaction_id": "TX1",
                    "issued_at": "2026-06-14T00:00:00"}
            bake.bake_licensed(src, out_dir, self.SECRET, info, preload_ms=150)
            lic = os.path.join(out_dir, "license.ithaca")
            with open(lic, "r+b") as f:
                f.seek(0); f.write(b"X")    # poskoz prvni bajt
            with self.assertRaises(bake.BakeError):
                bake.verify_licensed(out_dir, self.SECRET)
```

- [ ] **Step 2: Run → FAIL** (`bake.bake_licensed` neexistuje). Run: `python3 tests/test_bake_soundbank.py`.

- [ ] **Step 3: Implementuj** v `tools/bake_soundbank.py`. Přidej importy nahoru:
```python
import ithaca_crypto as ic   # tools/ je na sys.path pri spousteni z repo rootu
```
(Pozn.: aby `import ithaca_crypto` fungoval i z testu, který přidává `tools/` do
`sys.path`, je to OK. V `bake_soundbank.py` samotném přidej fallback: `import os,
sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))` před import.)

Přidej konstanty (vedle stávajících header offsetů):
```python
FLAG_ENCRYPTED = 1
CIPHER_SHA256_CTR = 1
HDR_CIPHER_ID = 152
HDR_NONCE = 154
HDR_HMAC_TAG = 186
ENC_DOMAIN = "ithaca-enc-v2"
MAC_DOMAIN = "ithaca-mac-v2"
```

Funkce pro deterministický license JSON + licensed write. `bake_licensed` staví
na stávajícím `write_ithaca`, ale: (a) zapíše license, (b) odvodí klíče, (c)
šifruje blob při zápisu, (d) dopíše flags/cipher_id/nonce/tag. Nejčistší je
refaktor `write_ithaca` tak, aby přijal volitelný `enc` parametr:

```python
def license_bytes(info: dict) -> bytes:
    # Deterministicky JSON (tridene klice, kompaktni) — TYTEZ bajty do KDF i MAC.
    import json
    return json.dumps(info, sort_keys=True, separators=(",", ":")).encode("utf-8")


def bake_licensed(src_dir, dst_dir, secret: bytes, info: dict,
                  preload_ms=150, force=False):
    lic = license_bytes(info)
    bank_key = ic.derive_key(secret, ENC_DOMAIN, lic)
    mac_key = ic.derive_key(secret, MAC_DOMAIN, lic)
    import secrets as _secrets
    nonce = _secrets.token_bytes(32)
    analysis = analyze_bank(src_dir, preload_ms)
    os.makedirs(dst_dir, exist_ok=True)
    out = os.path.join(dst_dir, "soundbank.ithaca")
    if os.path.exists(out) and not force:
        raise BakeError(f"{out} existuje (force=False)")
    # Zapis license.ithaca PRESNE temito bajty (KDF/MAC je nad nimi).
    with open(os.path.join(dst_dir, "license.ithaca"), "wb") as f:
        f.write(lic)
    write_ithaca(out, analysis, bank_name=info.get("bank_name", "bank"),
                 analysis_preload_ms=preload_ms,
                 enc={"bank_key": bank_key, "mac_key": mac_key, "nonce": nonce,
                      "license": lic})
```

V `write_ithaca` přidej parametr `enc=None`. Když `enc` je nastaven:
- při zápisu blobu každý WAV po zkopírování **zašifruj** `keystream_xor(bank_key,
  nonce, p0=blob_relativni_offset, data)` — tj. místo `f.write(chunk)` zapisuj
  zašifrované bajty; `sha_payload` počítej nad **zašifrovanými** bajty; sleduj
  blob-relativní pozici pro p0,
- po sestavení `metadata+index_bytes+names` spočítej `hmac_tag = ic.hmac_sha256(
  enc["mac_key"], metadata + index_bytes + names_blob + enc["license"])`,
- v hlavičce nastav `flags = FLAG_ENCRYPTED`, zapiš `cipher_id`@152, `nonce`@154,
  `hmac_tag`@186. (Stávající nešifrovaná cesta: `enc=None` → flags 0, vše jako v1.)

Implementační detail šifrování při streamovém zápisu: drž `blob_pos` (relativní
od `blob_offset`); pro každý chunk `enc_chunk = ic.keystream_xor(bank_key, nonce,
blob_pos, chunk)`, `f.write(enc_chunk)`, `sha_payload.update(enc_chunk)`,
`blob_pos += len(chunk)`. Padding mezi záznamy se NEšifruje (jsou to nuly mimo
WAV data, ale uvnitř blobu — POZOR: padding JE v blobu, takže ho šifrovat MUSÍŠ
taky, aby `blob_pos` a desifrovani sedely; jednodussi: pos, ktery roste i o pad,
a pad bajty take projdou keystreamem). Tj. **vše v blobu vč. paddingu jde přes
keystream** a `blob_pos` roste o pad.

`read_ithaca_header` rozšiř o čtení `cipher_id`/`nonce`/`hmac_tag` z hlavičky:
```python
        cipher_id = hb[HDR_CIPHER_ID]
        nonce = hb[HDR_NONCE:HDR_NONCE+32]
        hmac_tag = hb[HDR_HMAC_TAG:HDR_HMAC_TAG+32]
```
a vrať je v dictu.

`verify_licensed(dst_dir, secret)`:
```python
def verify_licensed(dst_dir, secret: bytes):
    out = os.path.join(dst_dir, "soundbank.ithaca")
    with open(os.path.join(dst_dir, "license.ithaca"), "rb") as f:
        lic = f.read()
    hdr = read_ithaca_header(out)          # overi sha256_index
    mac_key = ic.derive_key(secret, MAC_DOMAIN, lic)
    # Nacti sekce metadata+index+names a over hmac_tag.
    with open(out, "rb") as f:
        f.seek(hdr["metadata_offset"]); md = f.read(hdr["metadata_size"])
        f.seek(hdr["index_offset"]); ix = f.read(hdr["index_size"])
        f.seek(hdr["names_offset"]); nm = f.read(hdr["names_size"])
    tag = ic.hmac_sha256(mac_key, md + ix + nm + lic)
    if tag != hdr["hmac_tag"]:
        raise BakeError("hmac_tag nesouhlasi (license/index zmenen)")
    # Desifruj kazdy zaznam a porovnej se zdrojem.
    bank_key = ic.derive_key(secret, ENC_DOMAIN, lic)
    with open(out, "rb") as f:
        for e in hdr["entries"]:
            f.seek(e["entry_offset"])
            enc = f.read(e["entry_size"])
            p0 = e["entry_offset"] - hdr["blob_offset"]
            dec = ic.keystream_xor(bank_key, hdr["nonce"], p0, enc)
            # dec jsou puvodni WAV bajty; staci sanity: zacina "RIFF".
            if dec[:4] != b"RIFF":
                raise BakeError(f"desifrovany zaznam midi {e['midi']} neni RIFF")
```

Rozšiř `main()`: `--license` (interaktivní prompt na pole) NEBO `--license-json
<path>`; pokud zadáno → načti secret z `secret/bank_secret.key` (`--secret-file`
override) a volej `bake_licensed`; jinak stávající plaintext cesta. Pole promptu:
bank_name (default = jméno zdroje), owner_email, owner_name, transaction_id,
issued_at (default = teď).

- [ ] **Step 4: Run → PASS.** Run: `python3 tests/test_bake_soundbank.py -v`. Expected: všechny (vč. 3 nových licensed) OK.

- [ ] **Step 5: Commit**
```bash
git add tools/bake_soundbank.py tests/test_bake_soundbank.py
git commit -m "feat(tools): licensed bake — license.ithaca + sifrovany blob + hmac_tag"
```

---

### Task 7: ithaca_format/ithaca_bank — čtení v2 polí hlavičky

**Files:**
- Modify: `engine/sample/ithaca_format.h`, `engine/sample/ithaca_format.cpp`
- Test: `tests/test_ithaca_format.cpp`

`IthacaHeader` parser musí umět vyčíst `cipher_id`/`nonce`/`hmac_tag` z rezervy
(zatím je v1 nečte). Konstanty + pole.

- [ ] **Step 1: Failing test** — přidej do `tests/test_ithaca_format.cpp`:

```cpp
TEST_CASE("parseIthacaHeader cte v2 pole (cipher_id, nonce, hmac_tag)") {
    auto b = makeHeaderBytes();
    b[12] = 1;                       // flags bit0 = encrypted
    b[152] = 1;                      // cipher_id
    for (int i = 0; i < 32; ++i) b[(size_t)(154 + i)] = (uint8_t)(0xC0 + i);
    for (int i = 0; i < 32; ++i) b[(size_t)(186 + i)] = (uint8_t)(0xD0 + i);
    IthacaHeader h;
    REQUIRE(parseIthacaHeader(b.data(), b.size(), h));
    CHECK((h.flags & 1u) == 1u);
    CHECK(h.cipher_id == 1);
    for (int i = 0; i < 32; ++i) {
        CHECK(h.nonce[(size_t)i]    == (uint8_t)(0xC0 + i));
        CHECK(h.hmac_tag[(size_t)i] == (uint8_t)(0xD0 + i));
    }
}
```

- [ ] **Step 2: Build → FAIL** (`h.cipher_id`/`nonce`/`hmac_tag` neexistují).

- [ ] **Step 3: Implementuj.** Do `IthacaHeader` (ithaca_format.h) přidej pole:
```cpp
    uint8_t  cipher_id = 0;                  // 0 = plaintext, 1 = sha256-ctr-v2
    std::array<uint8_t, 32> nonce{};
    std::array<uint8_t, 32> hmac_tag{};
```
Přidej konstanty:
```cpp
inline constexpr size_t kIthacaHdrCipherId = 152;
inline constexpr size_t kIthacaHdrNonce    = 154;
inline constexpr size_t kIthacaHdrHmacTag  = 186;
inline constexpr uint16_t kCipherSha256Ctr = 1;
```
V `parseIthacaHeader` (ithaca_format.cpp) za stávající čtení doplň:
```cpp
    out.cipher_id = buf[kIthacaHdrCipherId];
    std::memcpy(out.nonce.data(),    buf + kIthacaHdrNonce,   32);
    std::memcpy(out.hmac_tag.data(), buf + kIthacaHdrHmacTag, 32);
```

- [ ] **Step 4: Build + test** — `cmake --build build --parallel 8 && ./build/tests/test_ithaca_format`. Expected: passed.

- [ ] **Step 5: Commit**
```bash
git add engine/sample/ithaca_format.h engine/sample/ithaca_format.cpp tests/test_ithaca_format.cpp
git commit -m "feat(sample): parser cte v2 pole hlavicky (cipher_id, nonce, hmac_tag)"
```

---

### Task 8: openIthacaBank — license load, MAC verify, decrypt handle

**Files:**
- Modify: `engine/sample/ithaca_bank.h`, `engine/sample/ithaca_bank.cpp`, `tests/ithaca_test_blob.h`
- Test: `tests/test_ithaca_bank.cpp`

- [ ] **Step 1: Rozšiř test helper** `tests/ithaca_test_blob.h` o volitelné šifrování. Přidej parametr a logiku (po sestavení `index_bytes`, `names_blob`, `blob` a PŘED výpočtem hashů):

Signatura `buildTestIthaca` dostane na konec:
```cpp
                                 const uint8_t* enc_secret = nullptr,
                                 const char* license_json = nullptr) {
```
Za výpočet `blob` (a před hlavičkou) vlož:
```cpp
    // -- v2 licensed varianta (enc_secret != null): zasifruj blob + license + tag.
    uint8_t flags_b0 = 0, cipher_id = 0;
    std::array<uint8_t, 32> nonce{}, hmac_tag{};
    std::string lic = license_json ? license_json : "";
    if (enc_secret) {
        flags_b0 = 1; cipher_id = ithaca::kCipherSha256Ctr;
        for (int i = 0; i < 32; ++i) nonce[(size_t)i] = (uint8_t)(0x30 + i);
        auto bank_key = ithaca::deriveKey(enc_secret, "ithaca-enc-v2",
                            (const uint8_t*)lic.data(), lic.size());
        auto mac_key = ithaca::deriveKey(enc_secret, "ithaca-mac-v2",
                            (const uint8_t*)lic.data(), lic.size());
        ithaca::keystreamXor(bank_key.data(), nonce.data(), 0,
                             blob.data(), blob.size());   // sifruj cely blob
        // tag pres metadata+index+names+license
        std::vector<uint8_t> macmsg;
        macmsg.insert(macmsg.end(), (uint8_t*)metadata.data(),
                      (uint8_t*)metadata.data() + metadata.size());
        macmsg.insert(macmsg.end(), index_bytes.begin(), index_bytes.end());
        // names_blob: v helperu names_size=0, takze nic
        macmsg.insert(macmsg.end(), lic.begin(), lic.end());
        hmac_tag = ithaca::hmacSha256(mac_key.data(), 32, macmsg.data(), macmsg.size());
        // zapis license.ithaca vedle banky
        std::ofstream lf(out.dir + "/license.ithaca", std::ios::binary);
        lf.write(lic.data(), (std::streamsize)lic.size());
    }
```
A v zápisu hlavičky nastav `header[12] = flags_b0; header[152] = cipher_id;`
a zkopíruj `nonce`→`header+154`, `hmac_tag`→`header+186`. (sha_index/​payload se
počítají jako dosud — payload nad případně zašifrovaným blobem.)

Přidej include `#include "util/ithaca_crypto.h"` do helperu.

- [ ] **Step 2: Failing test** — přidej do `tests/test_ithaca_bank.cpp`:

```cpp
TEST_CASE("openIthacaBank licensed happy path (spravny secret)") {
    uint8_t secret[32]; std::memset(secret, 0x07, 32);
    const char* lic = "{\"owner\":\"A\"}";
    BuiltBlob b = buildTestIthaca("lic_ok", {{60, 2000, 48000, -30.f, 100}},
                                  false, kIthacaVersion, 0, false,
                                  secret, lic);
    IthacaBankFile f = openIthacaBank(b.ithaca_path, secret);
    REQUIRE(f.ok);
    CHECK(f.entries.size() == 1u);
    CHECK(f.handle != nullptr);
    removeBlob(b);
}

TEST_CASE("openIthacaBank licensed: spatny secret → LicenseInvalid") {
    uint8_t secret[32]; std::memset(secret, 0x07, 32);
    uint8_t wrong[32];  std::memset(wrong, 0x08, 32);
    const char* lic = "{\"owner\":\"A\"}";
    BuiltBlob b = buildTestIthaca("lic_badsecret", {{60, 2000, 48000, -30.f, 100}},
                                  false, kIthacaVersion, 0, false, secret, lic);
    IthacaBankFile f = openIthacaBank(b.ithaca_path, wrong);
    CHECK_FALSE(f.ok);
    CHECK(f.license_invalid);
    removeBlob(b);
}

TEST_CASE("openIthacaBank licensed: editovana license → LicenseInvalid") {
    uint8_t secret[32]; std::memset(secret, 0x07, 32);
    const char* lic = "{\"owner\":\"A\"}";
    BuiltBlob b = buildTestIthaca("lic_edited", {{60, 2000, 48000, -30.f, 100}},
                                  false, kIthacaVersion, 0, false, secret, lic);
    { std::ofstream lf(b.dir + "/license.ithaca", std::ios::binary);
      lf << "{\"owner\":\"B\"}"; }                 // prepis license
    IthacaBankFile f = openIthacaBank(b.ithaca_path, secret);
    CHECK_FALSE(f.ok);
    CHECK(f.license_invalid);
    removeBlob(b);
}

TEST_CASE("openIthacaBank licensed: chybi license → LicenseInvalid") {
    uint8_t secret[32]; std::memset(secret, 0x07, 32);
    const char* lic = "{\"owner\":\"A\"}";
    BuiltBlob b = buildTestIthaca("lic_missing", {{60, 2000, 48000, -30.f, 100}},
                                  false, kIthacaVersion, 0, false, secret, lic);
    std::remove((b.dir + "/license.ithaca").c_str());
    IthacaBankFile f = openIthacaBank(b.ithaca_path, secret);
    CHECK_FALSE(f.ok);
    CHECK(f.license_invalid);
    removeBlob(b);
}
```

(Pozn.: stávající plaintext testy volají `openIthacaBank(path)` — ten zůstává a
interně použije kompilovaný `kBankSecret`; nové testy volají overload se secretem.)

- [ ] **Step 3: Build → FAIL** (overload + `license_invalid` neexistují).

- [ ] **Step 4: Implementuj.** Do `IthacaBankFile` (ithaca_bank.h) přidej:
```cpp
    bool license_invalid = false;   // flags bit0 a license/MAC selhala
```
Přidej overload deklaraci:
```cpp
// Overload s explicitnim secretem (testy / engine predava kompilovany kBankSecret).
IthacaBankFile openIthacaBank(const std::string& path, const uint8_t secret[32]);
```

V `ithaca_bank.cpp` přidej `#include "util/ithaca_crypto.h"`, `#include "bank_secret_generated.h"`, `#include <fstream>`. Stávající `openIthacaBank(path)` přesměruj:
```cpp
IthacaBankFile openIthacaBank(const std::string& path) {
    return openIthacaBank(path, kBankSecret);
}
```
A do `openIthacaBank(path, secret)` (přejmenovaná stávající implementace) za
úspěšnou validaci v1 (po `parseIthacaIndex` + per-entry kontrolách, před
`f.handle = std::move(handle); f.ok = true;`) vlož v2 větev:

```cpp
    // -- v2: sifrovana + licencovana banka (flags bit0) --
    if (h.flags & kIthacaFlagEncrypted) {
        if (h.cipher_id != kCipherSha256Ctr) {
            f.error = "neznamy cipher_id"; f.license_invalid = true; return f;
        }
        // license.ithaca vedle banky (stejny adresar jako path).
        std::string lic_path =
            std::filesystem::path(path).parent_path().string() + "/license.ithaca";
        std::ifstream lf(lic_path, std::ios::binary);
        if (!lf) { f.error = "chybi license.ithaca"; f.license_invalid = true; return f; }
        std::vector<uint8_t> lic((std::istreambuf_iterator<char>(lf)),
                                  std::istreambuf_iterator<char>());
        auto mac_key = deriveKey(secret, "ithaca-mac-v2", lic.data(), lic.size());
        // hmac nad metadata+index+names+license (sekce uz nactene vyse).
        std::vector<uint8_t> macmsg;
        macmsg.insert(macmsg.end(), metadata.begin(), metadata.end());
        macmsg.insert(macmsg.end(), index_bytes.begin(), index_bytes.end());
        macmsg.insert(macmsg.end(), names.begin(), names.end());
        macmsg.insert(macmsg.end(), lic.begin(), lic.end());
        auto tag = hmacSha256(mac_key.data(), 32, macmsg.data(), macmsg.size());
        if (tag != h.hmac_tag) {
            f.error = "hmac_tag nesouhlasi"; f.license_invalid = true; return f;
        }
        auto bank_key = deriveKey(secret, "ithaca-enc-v2", lic.data(), lic.size());
        handle = makeDecryptingFileHandle(handle, bank_key.data(),
                     h.nonce.data(), h.blob_offset, h.blob_size);
    }
    f.handle = std::move(handle);
    f.ok = true;
    return f;
```

POZOR: aby `metadata`, `index_bytes`, `names` byly v scope, ujisti se, že
existují jako proměnné z v1 validace (v1 už je čte pro `sha256_index`). Pokud
mají jiná jména, použij je.

`std::filesystem` include přidej, pokud chybí (`#include <filesystem>`).

- [ ] **Step 5: Build + test** — `cmake --build build --parallel 8 && ./build/tests/test_ithaca_bank`. Expected: všechny (vč. 4 nových licensed) passed. Pak celá `ctest --test-dir build -j 8 | tail -3`.

- [ ] **Step 6: Commit**
```bash
git add engine/sample/ithaca_bank.h engine/sample/ithaca_bank.cpp tests/ithaca_test_blob.h tests/test_ithaca_bank.cpp
git commit -m "feat(sample): openIthacaBank v2 — license load, hmac verify, decrypt handle"
```

---

### Task 9: loadPackedBank → propagace LicenseInvalid

**Files:**
- Modify: `engine/sample/sample_store.h`, `engine/sample/sample_store.cpp`
- Test: `tests/test_packed_bank_load.cpp`

- [ ] **Step 1: Failing test** — přidej do `tests/test_packed_bank_load.cpp`:

```cpp
TEST_CASE("loadBank packed licensed: spravny secret nacte (pres kBankSecret)") {
    // Pozn.: loadBank pouziva kompilovany kBankSecret. Test postavi banku TYMZ
    // kBankSecret (dostupny pres bank_secret_generated.h) → nacte se.
    extern const uint8_t* testBankSecret();   // viz helper nize
    const char* lic = "{\"o\":\"x\"}";
    BuiltBlob b = buildTestIthaca("load_lic", {{60, 4096, 48000, -30.f, 100}},
                                  false, kIthacaVersion, 0, false,
                                  testBankSecret(), lic);
    auto& L = log::Logger::default_();
    BankLoadProgress prog;
    Bank bank = loadBank(b.dir, L, 0, 0, 127, 150, 500, &prog);
    CHECK(bank.loaded_samples == 1);
    CHECK_FALSE(prog.license_invalid.load());
    removeBlob(b);
}

TEST_CASE("loadBank packed licensed: editovana license → license_invalid flag") {
    extern const uint8_t* testBankSecret();
    const char* lic = "{\"o\":\"x\"}";
    BuiltBlob b = buildTestIthaca("load_lic_bad", {{60, 4096, 48000, -30.f, 100}},
                                  false, kIthacaVersion, 0, false,
                                  testBankSecret(), lic);
    { std::ofstream lf(b.dir + "/license.ithaca", std::ios::binary); lf << "{\"o\":\"y\"}"; }
    auto& L = log::Logger::default_();
    BankLoadProgress prog;
    Bank bank = loadBank(b.dir, L, 0, 0, 127, 150, 500, &prog);
    CHECK(bank.loaded_samples == 0);
    CHECK(prog.license_invalid.load());
    removeBlob(b);
}
```

Na konec test souboru přidej helper, který zpřístupní kompilovaný secret:
```cpp
#include "bank_secret_generated.h"
const uint8_t* testBankSecret() { return ithaca::kBankSecret; }
```
(`#include <fstream>` přidej, pokud chybí.)

- [ ] **Step 2: Build → FAIL** (`prog.license_invalid` neexistuje).

- [ ] **Step 3: Implementuj.** Do `BankLoadProgress` (sample_store.h) přidej:
```cpp
    std::atomic<bool> license_invalid{false};   // packed v2: license/MAC selhala
```
V `loadPackedBank` (sample_store.cpp) uprav větev `if (!pf.ok)`:
```cpp
    if (!pf.ok) {
        logger.log("bank", log::Severity::Error,
                   "Banka '%s': soundbank.ithaca odmitnut — %s",
                   bank.name.c_str(), pf.error.c_str());
        if (progress && pf.license_invalid)
            progress->license_invalid.store(true, std::memory_order_relaxed);
        return;
    }
```

- [ ] **Step 4: Build + test** — `cmake --build build --parallel 8 && ./build/tests/test_packed_bank_load`. Expected: passed. Pak `ctest --test-dir build -j 8 | tail -3`.

- [ ] **Step 5: Commit**
```bash
git add engine/sample/sample_store.h engine/sample/sample_store.cpp tests/test_packed_bank_load.cpp
git commit -m "feat(sample): loadPackedBank propaguje LicenseInvalid do progress"
```

---

### Task 10: GUI overlay — LicenseInvalid (anglicky, click-to-continue)

**Files:**
- Modify: `app/gui/app_context.{h,cpp}` (přenos flagu), `app/gui/main.cpp` (overlay render)

Pozn.: konkrétní místa zjisti grep-em — `BankLoadProgress`, `bankLoadFraction`,
overlay render v `main.cpp`, completion handler v `app_context.cpp`. Vzor je
stávající `truncated` flag (NEUPLNA varování v `panel_bank.cpp`).

- [ ] **Step 1: Přenes flag do AppContext.** V `app_context.h` přidej člen
`std::atomic<bool> bank_license_invalid_{false};` (vedle `bank_truncated_`).
V completion handleru (`app_context.cpp`, kde se čte výsledek reloadu / progress)
nastav `bank_license_invalid_.store(load_progress_.license_invalid.load())`.
Reset na false na začátku `requestBankReload`.

- [ ] **Step 2: Overlay render.** V `app/gui/main.cpp` v overlay bloku (kde se
kreslí progress bar během loadu) přidej: pokud `ctx.bank_license_invalid_`, místo
progress baru vykresli chybový stav:
```cpp
// LicenseInvalid: anglicky overlay + cekani na klik (zadny timeout).
ImGui::TextUnformatted("Soundbank is corrupted or license file is invalid.");
ImGui::TextUnformatted("Sampler is unable to load the bank.");
ImGui::Dummy({0, 12});
if (ImGui::Button("Click to continue",
                  ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
    ctx.bank_license_invalid_.store(false);   // zavri overlay
}
```
Overlay se zobrazuje dokud `bank_license_invalid_` je true (nezávisle na
`reloadInProgress`). Po kliknutí se vypne → GUI v plné palbě, banka nenačtená.

POZOR: zajisti, že overlay-visible podmínka v main.cpp je
`ctx.reloadInProgress() || ctx.bank_license_invalid_` a že v license stavu se
NEkreslí progress bar (jen text + tlačítko), aby šlo kliknout.

- [ ] **Step 3: Build + ruční ověření.** `cmake --build build --parallel 8`.
Pokud je GUI sestavitelné, ověř ručně: zabal licensed banku s jiným než
kompilovaným secretem (nebo edituj license) → vyber v dropdownu → overlay ukáže
anglické varování + „Click to continue"; klik → GUI, banka nenačtená, lze vybrat
jinou. (Pokud GUI nelze v tomto prostředí spustit, ověř aspoň kompilaci.)

- [ ] **Step 4: Commit**
```bash
git add app/gui/app_context.h app/gui/app_context.cpp app/gui/main.cpp
git commit -m "feat(gui): LicenseInvalid overlay — anglicky text + click to continue"
```

---

### Task 11: Cross-language parita + round-trip licensed

**Files:**
- Modify: `tests/roundtrip_packed_bank.sh`

End-to-end: python zabalí licensed banku **se stejným secretem**, jaký je
zkompilovaný v `ithaca-cli` (přečte `secret/bank_secret.key`), engine ji načte
přes `--inspect` (dešifruje) → správný počet samplů. Pak edituj license → engine
ji odmítne (`--inspect` selže / loaded 0).

- [ ] **Step 1: Rozšiř** `tests/roundtrip_packed_bank.sh` — za stávající plaintext
round-trip přidej licensed sekci:

```bash
# -- licensed varianta: bake se secretem z secret/bank_secret.key (tyz, ktery
#    je zkompilovan v ithaca-cli) → engine ho desifruje. --
LIC_DST="$WORK/lic"; mkdir -p "$LIC_DST"
python3 "$ROOT/tools/bake_soundbank.py" \
    --source-soundbank-dir "$SRC" --destination-soundbank-dir "$LIC_DST" \
    --license-json <(printf '{"bank_name":"rt","owner_email":"a@b.cz","owner_name":"A","transaction_id":"TX","issued_at":"2026-06-14T00:00:00"}') \
    --secret-file "$ROOT/secret/bank_secret.key" --verify

"$CLI" --inspect "$LIC_DST" >"$WORK/lic.txt" 2>&1 \
    || { echo "CHYBA: inspect licensed selhal"; cat "$WORK/lic.txt"; exit 1; }
grep -q "Format: packed-ithaca" "$WORK/lic.txt" \
    || { echo "CHYBA: licensed neni packed-ithaca"; cat "$WORK/lic.txt"; exit 1; }
python3 - "$WORK/lic.txt" <<'PYEOF'
import re, sys
s = open(sys.argv[1]).read()
m = re.search(r"Celkem samplu: (\d+)", s)
assert m and int(m.group(1)) == 3, "licensed: spatny pocet samplu\n" + s
print("LICENSED LOAD OK")
PYEOF

# Tamper: edit license → engine odmitne (loaded 0 → --inspect exit 1).
printf '{"tampered":true}' > "$LIC_DST/license.ithaca"
if "$CLI" --inspect "$LIC_DST" >/dev/null 2>&1; then
    echo "CHYBA: editovana license se nacetla (mela selhat)"; exit 1
fi
echo "LICENSED TAMPER REJECTED OK"
```

(Pozn.: `--license-json` čte cestu; process-substitution `<(...)` funguje v bash.
Pokud `--secret-file` nebo `--license-json` nemáš v `main()`, doplň je v Tasku 6.)

- [ ] **Step 2: Ověř** — Run:
```bash
cmake --build build --parallel 8
ctest --test-dir build -R roundtrip_packed_bank --output-on-failure 2>&1 | tail -8
```
Expected: Passed; výstup obsahuje `LICENSED LOAD OK` a `LICENSED TAMPER REJECTED OK`.

- [ ] **Step 3: Commit**
```bash
git add tests/roundtrip_packed_bank.sh
git commit -m "test: cross-language licensed roundtrip (python bake + engine desifruje + tamper)"
```

---

### Task 12: Dokumentace + finální verifikace

**Files:**
- Modify: `docs/bank-format-packed.md` (§7 → implementováno), `docs/superpowers/specs/2026-06-14-packed-soundbank-v2-security-design.md` (Stav)

- [ ] **Step 1: Aktualizuj `docs/bank-format-packed.md` §7** — změň nadpis z
„(v2 — planned, NOT implemented)" na „(v2 — IMPLEMENTOVÁNO)", a doplň do §2 (How
to bake) licensed příkaz:
```sh
python3 tools/bake_soundbank.py \
    --source-soundbank-dir   path/to/dynamic-bank \
    --destination-soundbank-dir path/to/output-dir \
    --license --verify     # interaktivni prompt na udaje vlastnika
```
+ věta: vznikne `soundbank.ithaca` (šifrovaný) + `license.ithaca` (plaintext);
banku otevře jen build appky se shodným `secret/bank_secret.key`. Edit license =
overlay „Soundbank is corrupted or license file is invalid".

- [ ] **Step 2: Spec Stav** — v `docs/superpowers/specs/2026-06-14-packed-soundbank-v2-security-design.md`
změň `Stav: schválený návrh (před implementací)` na `Stav: implementováno`.

- [ ] **Step 3: Finální verifikace**
```bash
ctest --test-dir build -j 8 --output-on-failure | tail -5
python3 tests/test_ithaca_crypto.py && python3 tests/test_bake_soundbank.py
make smoke
```
Expected: vše zelené, smoke OK.

- [ ] **Step 4: Commit**
```bash
git add docs/bank-format-packed.md docs/superpowers/specs/2026-06-14-packed-soundbank-v2-security-design.md
git commit -m "docs: v2 zabezpeceni implementovano — bake --license workflow"
```

---

## Poznámky pro implementátora

- **Bit-exact WAV cesta:** v1 plaintext banky (`flags bit0 == 0`) se NESMÍ změnit
  — `DecryptingFileHandle` se na ně nikdy nenasadí (jen u flags bit0). Hlídá
  `test_render_regression` + `test_packed_bank_load` plaintext testy.
- **Padding v blobu:** šifruje se VŠE v `[blob_offset, blob_offset+blob_size)`
  včetně 4 KB paddingu mezi WAVy (jinak by `p0` aritmetika nesouhlasila). Helper
  i bake i handle musí počítat `p0` relativně k `blob_offset`.
- **license_bytes = přesné bajty souboru** (ne reparsovaný JSON). Python píše
  `json.dumps(sort_keys=True, separators=(",",":"))` a tytéž bajty dává do KDF/MAC
  a zapisuje do `license.ithaca`. C++ čte soubor 1:1.
- **Secret v testech:** krypto je pure (secret param). C++ unit testy staví
  fixture vlastním secretem; `loadBank` test používá kompilovaný `kBankSecret`
  (přes `bank_secret_generated.h`). Round-trip používá `secret/bank_secret.key`.
- **Doménová separace:** `"ithaca-enc-v2"` / `"ithaca-mac-v2"` musí být IDENTICKÉ
  v C++ (ithaca_crypto/ithaca_bank/helper) i pythonu (ithaca_crypto.py/bake).
- Komentáře česky bez diakritiky.
