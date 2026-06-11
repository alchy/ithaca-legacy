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
