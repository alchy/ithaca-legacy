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
