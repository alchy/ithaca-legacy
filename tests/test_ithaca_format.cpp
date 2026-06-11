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
