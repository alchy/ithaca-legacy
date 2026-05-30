// tests/test_voice_pool.cpp
// Testuje render hlasu a chovani poolu na syntetickych SampleAssetech.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "voice/voice_pool.h"
#include "voice/patch_manager.h"
#include "sample/sample_types.h"

#include <cmath>
#include <vector>

using namespace ithaca;

namespace {
// SampleAsset s konstantni amplitudou, dany pocet frames, SR 48k.
SampleAsset makeAsset(float amp, int frames) {
    SampleAsset a;
    a.peak_rms_db = 0.f;
    MicLayer m;
    m.mic_name         = "stereo";
    m.file.frames      = frames;
    m.file.sample_rate = 48000;
    m.file.valid       = true;
    m.mode             = MicLayerMode::FullyLoaded;
    m.head_frames      = frames;
    m.preload_head.assign((size_t)frames * 2, amp);
    a.mics.push_back(std::move(m));
    return a;
}
// Soucet absolutnich hodnot bufferu (hruba "energie").
double energy(const std::vector<float>& b) {
    double s = 0; for (float v : b) s += std::fabs((double)v); return s;
}
} // namespace

TEST_CASE("Voice prehraje nenulovy zvuk a pak utichne (konec samplu)") {
    SampleAsset a = makeAsset(0.5f, 256);
    VoicePool pool(8);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 1.0; vs.vel_gain = 1.0f;
    pool.noteOn(60, vs, /*engine_sr=*/48000.f);

    std::vector<float> L(512, 0.f), R(512, 0.f);
    bool any = pool.processBlock(L.data(), R.data(), 512, 48000.f);
    CHECK(any);
    CHECK(energy(L) > 0.0);                  // neco zaznelo
    // Po dohrani 256-frame samplu uz dalsi blok nic neprida.
    std::vector<float> L2(512, 0.f), R2(512, 0.f);
    pool.processBlock(L2.data(), R2.data(), 512, 48000.f);
    CHECK(energy(L2) == doctest::Approx(0.0));
}

TEST_CASE("noteOff spusti release a hlas dozni") {
    SampleAsset a = makeAsset(0.5f, 48000);  // 1 s, dost dlouhy
    VoicePool pool(8);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 1.0; vs.vel_gain = 1.0f;
    pool.noteOn(60, vs, 48000.f);
    std::vector<float> L(256, 0.f), R(256, 0.f);
    pool.processBlock(L.data(), R.data(), 256, 48000.f);
    CHECK(pool.activeCount() == 1);
    pool.noteOff(60, /*release_ms=*/10.f, 48000.f);
    // Po dostatku bloku (>10 ms = 480 frames) hlas zmizi.
    for (int i = 0; i < 10; ++i) {
        std::vector<float> b(256, 0.f), b2(256, 0.f);
        pool.processBlock(b.data(), b2.data(), 256, 48000.f);
    }
    CHECK(pool.activeCount() == 0);
}

TEST_CASE("pitch_ratio 2.0 prehraje sampl 2x rychleji (drive utichne)") {
    SampleAsset a = makeAsset(0.5f, 1000);
    VoicePool pool(8);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 2.0; vs.vel_gain = 1.0f;
    pool.noteOn(60, vs, 48000.f);
    // Pri 2x rychlosti je 1000-frame sampl prehrany za ~500 frames.
    std::vector<float> L(600, 0.f), R(600, 0.f);
    pool.processBlock(L.data(), R.data(), 600, 48000.f);
    std::vector<float> L2(100, 0.f), R2(100, 0.f);
    bool any2 = pool.processBlock(L2.data(), R2.data(), 100, 48000.f);
    CHECK_FALSE(any2);                        // uz dohrano
}

TEST_CASE("retrigger tehoz tonu neztrati hlas (porad 1 aktivni)") {
    SampleAsset a = makeAsset(0.5f, 48000);
    VoicePool pool(8);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 1.0; vs.vel_gain = 1.0f;
    pool.noteOn(60, vs, 48000.f);
    std::vector<float> b(256, 0.f), b2(256, 0.f);
    pool.processBlock(b.data(), b2.data(), 256, 48000.f);
    pool.noteOn(60, vs, 48000.f);            // retrigger
    pool.processBlock(b.data(), b2.data(), 256, 48000.f);
    // Po retriggeru hraje (nove) tlo + pripadny damping; aktivni aspon 1.
    CHECK(pool.activeCount() >= 1);
}
