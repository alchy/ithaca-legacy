// tests/test_file_handle.cpp
// IFileHandle: pozicovane cteni (pread) bez sdileneho kurzoru.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "io/file_handle.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
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

TEST_CASE("readAt edge: n=0 → true; offset zcela za EOF → false") {
    TempFile p{ makePatternFile() };
    auto h = openFileHandle(p.path);
    REQUIRE(h != nullptr);
    uint8_t b;
    CHECK(h->readAt(0, &b, 0));
    CHECK(h->readAt(512, &b, 0));
    uint8_t buf[16];
    CHECK_FALSE(h->readAt(2000, buf, 16));
}

TEST_CASE("readAt soubezne ze 4 vlaken (lock-free kontrakt)") {
    // Cely smysl IFileHandle: paralelni loader workery + stream worker ctou
    // TYZ handle bez zamku. 4 vlakna ctou prokladane offsety a overuji vzor.
    TempFile p{ makePatternFile() };
    auto h = openFileHandle(p.path);
    REQUIRE(h != nullptr);
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&, t] {
            uint8_t buf[32];
            for (int it = 0; it < 500; ++it) {
                uint64_t off = (uint64_t)(((t * 131 + it * 37) % 992));
                if (!h->readAt(off, buf, 32)) { errors++; continue; }
                for (int i = 0; i < 32; ++i)
                    if (buf[i] != (uint8_t)((off + (uint64_t)i) & 0xFF)) { errors++; break; }
            }
        });
    }
    for (auto& th : ts) th.join();
    CHECK(errors.load() == 0);
}
