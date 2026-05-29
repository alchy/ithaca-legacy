// tests/test_sample_loader.cpp
// Testuje peak RMS a detekci konce attacku na syntetickych bufferech.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "sample/sample_loader.h"

#include <cmath>
#include <vector>

using namespace ithaca;

// Pomocna: vytvor interleaved stereo buffer z mono amplitud (L==R).
static std::vector<float> stereoFromMono(const std::vector<float>& mono) {
    std::vector<float> s(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) { s[i*2] = mono[i]; s[i*2+1] = mono[i]; }
    return s;
}

TEST_CASE("measurePeakRmsDb: plny rozsah (0 dBFS) pro konstantni 1.0") {
    std::vector<float> mono(48000, 1.0f);            // 1 s, plna amplituda
    auto s = stereoFromMono(mono);
    float db = measurePeakRmsDb(s.data(), (int)mono.size(), 48000);
    CHECK(db == doctest::Approx(0.0f).epsilon(0.05));   // RMS(1.0) = 1.0 → 0 dBFS
}

TEST_CASE("measurePeakRmsDb: -6 dBFS pro amplitudu 0.5") {
    std::vector<float> mono(48000, 0.5f);
    auto s = stereoFromMono(mono);
    float db = measurePeakRmsDb(s.data(), (int)mono.size(), 48000);
    CHECK(db == doctest::Approx(-6.02f).epsilon(0.1));
}

TEST_CASE("measurePeakRmsDb: ticho vrati podlahu") {
    std::vector<float> mono(48000, 0.0f);
    auto s = stereoFromMono(mono);
    float db = measurePeakRmsDb(s.data(), (int)mono.size(), 48000);
    CHECK(db <= -100.0f);
}

TEST_CASE("measurePeakRmsDb: chyti PEAK, ne prumer") {
    // 1 s ticha + kratky hlasity zaver → peak RMS musi byt blizko 0 dBFS,
    // ne prumer (ten by byl mnohem nizsi).
    std::vector<float> mono(48000, 0.0f);
    for (int i = 40000; i < 48000; ++i) mono[i] = 1.0f;   // hlasity konec
    auto s = stereoFromMono(mono);
    float db = measurePeakRmsDb(s.data(), (int)mono.size(), 48000);
    CHECK(db == doctest::Approx(0.0f).epsilon(0.5));
}

TEST_CASE("findAttackEnd: ramp nahoru → konec attacku je u vrcholu") {
    // Linearni ramp 0→1 pres 0.5 s, pak drzi 1.0 dalsich 0.5 s.
    std::vector<float> mono(48000, 1.0f);
    for (int i = 0; i < 24000; ++i) mono[i] = (float)i / 24000.f;
    auto s = stereoFromMono(mono);
    int ae = findAttackEnd(s.data(), (int)mono.size(), 48000);
    // Vrchol okenni RMS nastane az kdyz je okno plne v hlasite casti (~24000+).
    CHECK(ae >= 20000);
    CHECK(ae <= 30000);
}
