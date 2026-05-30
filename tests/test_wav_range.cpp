// tests/test_wav_range.cpp
// readWavRange: cte vyrez WAV. Round-trip s nasim wav_writerem.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "io/wav_reader.h"
#include "io/wav_writer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace ithaca;

namespace {
// RAII guard pro temp WAV: smaze soubor v destruktoru, takze REQUIRE-fail
// uprostred testu nezustavi za sebou bordel v /tmp.
struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { if (!path.empty()) std::remove(path.c_str()); }
};

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
    TempFile p{ makeRampWav("mid") };
    WavData w = readWavRange(p.path, /*frame_off=*/1000, /*frame_count=*/256);
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

TEST_CASE("readWavRange orizne pozadavek presahujici konec souboru") {
    TempFile p{ makeRampWav("end") };
    // Soubor ma 4096 frames; pozadame o 1000 frames od offsetu 4000 → vrati 96.
    WavData w = readWavRange(p.path, 4000, 1000);
    REQUIRE(w.valid);
    CHECK(w.frames == 96);
    CHECK(w.samples.size() == 96u * 2u);
}

TEST_CASE("readWavRange offset >= konec souboru → 0 frames, stale valid") {
    TempFile p{ makeRampWav("past") };
    WavData w = readWavRange(p.path, 4096, 100);
    REQUIRE(w.valid);
    CHECK(w.frames == 0);
    CHECK(w.samples.empty());
}

TEST_CASE("readWavRange na neexistujicim souboru → invalid") {
    WavData w = readWavRange("/tmp/ithaca_no_such_file_xyz.wav", 0, 100);
    CHECK_FALSE(w.valid);
}
