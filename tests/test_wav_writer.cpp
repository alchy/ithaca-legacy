// tests/test_wav_writer.cpp
// Round-trip: zapis buffer pres writeWav, precti pres readWav, over shodu.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "io/wav_writer.h"
#include "io/wav_reader.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace ithaca;

TEST_CASE("writeWav + readWav round-trip zachova hodnoty a SR") {
    std::vector<float> in = {0.5f, -0.5f, 0.25f, -0.25f, 0.f, 1.f, -1.f, 0.75f};  // 4 stereo frames
    std::string p = "/tmp/ithaca_wavwriter_rt.wav";
    REQUIRE(writeWavStereo16(p, in, 48000));
    WavData w = readWav(p);
    std::remove(p.c_str());
    REQUIRE(w.valid);
    CHECK(w.sample_rate == 48000);
    CHECK(w.frames == 4);
    REQUIRE(w.samples.size() == 8u);
    for (size_t i = 0; i < in.size(); ++i)
        CHECK(w.samples[i] == doctest::Approx(in[i]).epsilon(0.001));
}

TEST_CASE("writeWav clampuje mimo rozsah a vrati false pro nezapisovatelnou cestu") {
    std::vector<float> in = {2.0f, -2.0f};   // bude clampnuto na +1/-1
    std::string p = "/tmp/ithaca_wavwriter_clamp.wav";
    REQUIRE(writeWavStereo16(p, in, 44100));
    WavData w = readWav(p);
    std::remove(p.c_str());
    REQUIRE(w.valid);
    CHECK(w.samples[0] == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(w.samples[1] == doctest::Approx(-1.0f).epsilon(0.001));
    // Nezapisovatelna cesta (neexistujici adresar) → false.
    CHECK_FALSE(writeWavStereo16("/tmp/ithaca_nope_dir_xyz/out.wav", in, 48000));
}
