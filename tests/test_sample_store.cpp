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

// Zapis stereo 16-bit WAV s konstantni amplitudou `amp` (default 0.3 s, ale
// frames lze nastavit pro test streamingu dlouheho samplu).
void writeConstWav(const std::string& path, float amp,
                   int sr = 48000, int frames = -1) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    if (frames < 0) frames = sr * 3 / 10;             // default 0.3 s
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

TEST_CASE("loadBank postavi NoteMap z fixture banky") {
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
    Bank bank = loadBank(dir, L);
    fs::remove_all(dir);

    CHECK(bank.format == BankFormat::FixedVelocity);
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
    CHECK(bank.notes[60].slots[0].variants[0].mics[0].file.sample_rate == 48000);
    // 0.3 s sampl @ 48k = 14400 frames; pri default preload_ms=150 je threshold
    // 2*150ms*48 = 14400 → vejde se PRESNE → FullyLoaded, head_frames = file.frames.
    CHECK(bank.notes[60].slots[0].variants[0].mics[0].mode == MicLayerMode::FullyLoaded);
    CHECK(bank.notes[60].slots[0].variants[0].mics[0].head_frames ==
          bank.notes[60].slots[0].variants[0].mics[0].file.frames);
    // Nota 62 ma 1 slot.
    REQUIRE(bank.notes[62].recorded);
    CHECK(bank.notes[62].slots.size() == 1u);
    // Nota 61 nema sampl.
    CHECK_FALSE(bank.notes[61].recorded);
}

TEST_CASE("loadBank: dynamic-velocity folder format + variabilni pocet vrstev") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_dynfixture_bank";
    fs::remove_all(dir);
    fs::create_directories(dir + "/m060");
    fs::create_directories(dir + "/m072");
    // Nota 60: 4 vrstvy s hash-nazvy, amplitudy ZAMERNE neserazene dle nazvu.
    writeConstWav(dir + "/m060/aaaa1111aaaa1111.wav", 0.4f);
    writeConstWav(dir + "/m060/bbbb2222bbbb2222.wav", 0.1f);
    writeConstWav(dir + "/m060/cccc3333cccc3333.wav", 0.9f);
    writeConstWav(dir + "/m060/dddd4444dddd4444.wav", 0.2f);
    // Nota 72: jen 2 vrstvy → jiny pocet (overuje dynamicky pocet souboru).
    writeConstWav(dir + "/m072/eeee5555eeee5555.wav", 0.3f);
    writeConstWav(dir + "/m072/ffff6666ffff6666.wav", 0.7f);

    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);
    Bank bank = loadBank(dir, L);
    fs::remove_all(dir);

    CHECK(bank.format == BankFormat::DynamicVelocity);
    CHECK(bank.loaded_samples == 6);
    // Variabilni pocet velocity vrstev per nota (= pocet souboru ve slozce).
    REQUIRE(bank.notes[60].recorded);
    CHECK(bank.notes[60].slots.size() == 4u);
    REQUIRE(bank.notes[72].recorded);
    CHECK(bank.notes[72].slots.size() == 2u);
    // Sloty serazene VZESTUPNE dle mereneho RMS (nazev/poradi na disku ignorovano).
    for (size_t i = 1; i < bank.notes[60].slots.size(); ++i)
        CHECK(bank.notes[60].slots[i-1].rms_db < bank.notes[60].slots[i].rms_db);
    // Jeden variant + stereo mic, stejne jako fixed-velocity.
    REQUIRE(bank.notes[60].slots[0].variants.size() == 1u);
    CHECK(bank.notes[60].slots[0].variants[0].mics[0].mic_name == "stereo");
    CHECK_FALSE(bank.notes[61].recorded);
}

TEST_CASE("loadBank respektuje MIDI rozsah") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_fixture_range";
    fs::remove_all(dir);
    fs::create_directories(dir);
    writeConstWav(dir + "/m060-vel3-f48.wav", 0.5f);
    writeConstWav(dir + "/m072-vel3-f48.wav", 0.5f);
    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);
    // Nacti jen notu 60 (rozsah 60..60).
    Bank bank = loadBank(dir, L, /*cache_budget_mb=*/0, /*midi_from=*/60, /*midi_to=*/60);
    fs::remove_all(dir);
    CHECK(bank.loaded_samples == 1);
    CHECK(bank.notes[60].recorded);
    CHECK_FALSE(bank.notes[72].recorded);
}

TEST_CASE("loadBank: dlouhy sampl je Streamed, nacita jen preload head") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_fixture_streamed";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // 60_000 frames @ 48k = 1.25 s — vetsi nez 2 * 150 ms = 14400 frames threshold
    // → mode Streamed, head_frames = preload_ms*sr/1000 = 7200.
    writeConstWav(dir + "/m060-vel4-f48.wav", 0.4f, /*sr=*/48000, /*frames=*/60000);

    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);
    // resonance_window_ms=0 -> preload_resonance zustane prazdny (faze 4 semantika).
    Bank bank = loadBank(dir, L, /*cache_budget_mb=*/0,
                               /*midi_from=*/0, /*midi_to=*/127,
                               /*preload_ms=*/150,
                               /*resonance_window_ms=*/0);
    fs::remove_all(dir);

    REQUIRE(bank.loaded_samples == 1);
    REQUIRE(bank.notes[60].recorded);
    const MicLayer& mic = bank.notes[60].slots[0].variants[0].mics[0];
    CHECK(mic.mode == MicLayerMode::Streamed);
    CHECK(mic.head_frames == 7200);       // 150 ms * 48000 / 1000
    CHECK(mic.file.frames == 60000);
    CHECK(mic.file.sample_rate == 48000);
    // V RAM lezi jen preload_head; preload_resonance pri rwin=0 prazdny.
    CHECK(mic.preload_head.size() == 7200u * 2u);
    CHECK(mic.preload_resonance.empty());
    // total_bytes pocita jen rezidentni preload (NE cely soubor).
    CHECK(bank.total_bytes == 7200u * 2u * sizeof(float));
}

TEST_CASE("buildResonanceCache: Streamed cilova vrstva nacte preload_resonance") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_fixture_resonance";
    fs::remove_all(dir); fs::create_directories(dir);
    // 60 000 frames @48k = 1.25 s -> Streamed (>2*preload_ms=14400 pro 150 ms).
    writeConstWav(dir + "/m060-vel4-f48.wav", 0.5f, 48000, 60000);

    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);
    Bank bank = loadBank(dir, L, /*cache_budget_mb=*/0,
                               /*midi_from=*/0, /*midi_to=*/127,
                               /*preload_ms=*/150,
                               /*resonance_window_ms=*/200);
    // loadBank uz resonance NEplni eagerne — buffer naplni az buildResonanceCache
    // pro cilovou vrstvu (jediny slot noty 60 = cil pro libovolny target_db).
    buildResonanceCache(bank, /*target_db=*/-20.f, /*window_ms=*/200, L);
    fs::remove_all(dir);

    REQUIRE(bank.notes[60].recorded);
    const SampleAsset& a = bank.notes[60].slots[0].variants[0];
    const MicLayer& mic  = a.mics[0];
    CHECK(mic.mode == MicLayerMode::Streamed);
    CHECK(mic.head_frames == 7200);
    CHECK(mic.resonance_frames > 0);
    CHECK((int)mic.preload_resonance.size() == mic.resonance_frames * 2);
    CHECK(mic.resonance_start_frame == a.attack_end_frame);
    // attack_end_frame na konstantnim signalu by mel byt brzy — over ze se vejde do preloadu.
    CHECK(mic.resonance_start_frame >= 0);
    CHECK(mic.resonance_start_frame < mic.file.frames);
}

TEST_CASE("loadBank vrati prazdnou banku pro neexistujici adresar") {
    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);
    Bank bank = loadBank("/tmp/ithaca_neexistuje_zzz", L);
    CHECK(bank.loaded_samples == 0);
    CHECK_FALSE(bank.notes[60].recorded);
}

TEST_CASE("loadBank: RAM budget (OOM guard) preruseni nacitani bez padu") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_budget_bank";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // 24 not, kazda ~112 KB rezidentne (FullyLoaded 0.3 s stereo float) → ~2.7 MB.
    const int kNotes = 24;
    char name[80];
    for (int n = 41; n < 41 + kNotes; ++n) {
        std::snprintf(name, sizeof(name), "%s/m%03d-vel3-f48.wav", dir.c_str(), n);
        writeConstWav(name, 0.3f);
    }
    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);

    // Budget 0 = vypnuto → nactou se vsechny noty.
    Bank full = loadBank(dir, L, /*cache_budget_mb=*/0);
    CHECK(full.loaded_samples == kNotes);
    const size_t full_bytes = full.total_bytes;
    REQUIRE(full_bytes > 1u * 1024 * 1024);   // fixture > 1 MB (jinak by 1MB budget netriggernul)

    // Budget 1 MB → nacitani se PRERUSI driv; neuplna banka, ZADNY pad/vyjimka.
    Bank capped = loadBank(dir, L, /*cache_budget_mb=*/1);
    CHECK(capped.loaded_samples > 0);          // neco se nacetlo
    CHECK(capped.loaded_samples < kNotes);     // ale ne vse → budget zabral (graceful abort)
    CHECK(capped.total_bytes <= full_bytes);

    fs::remove_all(dir);
}
