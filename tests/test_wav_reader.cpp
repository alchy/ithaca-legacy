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
