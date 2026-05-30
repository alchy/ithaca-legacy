// tests/test_stream_engine.cpp
// ----------------------------
// Unit testy StreamEngine: ring acquire/release recyklace + worker plni ring
// z WAV souboru. Veci ohledne integrace s Voice/Bank pokryvaji
// test_long_sample_stream.cpp (integ).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "stream/stream_engine.h"
#include "io/wav_writer.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ithaca;

namespace {
struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { if (!path.empty()) std::remove(path.c_str()); }
};

// Ramp WAV: L[i] = i/N, R[i] = -i/N (stereo 48k).
std::string makeRampWav(const char* tag, int frames, int sr = 48000) {
    std::vector<float> s((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        s[(size_t)i * 2]     =  (float)i / (float)frames;
        s[(size_t)i * 2 + 1] = -(float)i / (float)frames;
    }
    std::string p = std::string("/tmp/ithaca_stream_") + tag + ".wav";
    REQUIRE(writeWavStereo16(p, s, sr));
    return p;
}
} // namespace

TEST_CASE("StreamEngine acquire/release recykluje sloty") {
    StreamEngine se(/*n_rings=*/4, /*ring_capacity_frames=*/1024);
    se.start();

    RingHandle* r1 = se.acquireRing(); REQUIRE(r1 != nullptr);
    RingHandle* r2 = se.acquireRing(); REQUIRE(r2 != nullptr);
    RingHandle* r3 = se.acquireRing(); REQUIRE(r3 != nullptr);
    RingHandle* r4 = se.acquireRing(); REQUIRE(r4 != nullptr);
    RingHandle* r5 = se.acquireRing(); CHECK(r5 == nullptr);   // pool plny

    se.releaseRing(r1);
    RingHandle* r6 = se.acquireRing(); REQUIRE(r6 != nullptr); // recykluje r1
    CHECK(r6 == r1);

    se.releaseRing(r2); se.releaseRing(r3);
    se.releaseRing(r4); se.releaseRing(r6);
    se.stop();
}

TEST_CASE("StreamEngine worker naplni ring z WAV souboru") {
    TempFile p{ makeRampWav("worker", 8000) };
    StreamEngine se(2, 2048);
    se.start();

    RingHandle* r = se.acquireRing();
    REQUIRE(r != nullptr);
    se.requestRead(r, p.path, /*frame_off=*/100, /*n_frames=*/512);

    // Pockej max ~200 ms na worker (1 ms sleep + I/O).
    for (int i = 0; i < 100 && r->available() < 512; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(r->available() >= 512);

    // Prvni frame v ringu = puvodni frame 100.
    float L = 0.f, R = 0.f;
    REQUIRE(r->popFrame(L, R));
    CHECK(L == doctest::Approx(100.f / 8000.f).epsilon(0.001));
    CHECK(R == doctest::Approx(-100.f / 8000.f).epsilon(0.001));

    // Spotrebuj jeste par a over hodnoty po sobe (monotone roste).
    float prevL = L;
    for (int k = 1; k < 10; ++k) {
        REQUIRE(r->popFrame(L, R));
        CHECK(L > prevL);
        prevL = L;
    }

    se.releaseRing(r);
    se.stop();
}

TEST_CASE("StreamEngine eof_when_done nastavi ring->eof_ po konci souboru") {
    TempFile p{ makeRampWav("eof", 1000) };
    StreamEngine se(1, 2048);
    se.start();

    RingHandle* r = se.acquireRing();
    REQUIRE(r != nullptr);
    // Pozadame o 2000 frames od offsetu 500 → file ma 1000, vrati 500 + EOF.
    se.requestRead(r, p.path, /*off=*/500, /*n=*/2000, /*eof_when_done=*/true);

    // Pockej, az worker dotahne praci.
    for (int i = 0; i < 100 && !r->eof_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(r->eof_.load());
    CHECK(r->available() == 500);   // dostali jsme jen zbyle frames

    se.releaseRing(r);
    se.stop();
}

TEST_CASE("RingHandle push/pop wrap pres konec bufferu") {
    // Direct test ringu bez workeru.
    RingHandle r;
    r.capacity_frames = 8;
    r.buf.assign(16, 0.f);

    std::vector<float> in1(12, 0.f);   // 6 frames stereo
    for (int i = 0; i < 6; ++i) { in1[(size_t)i*2] = (float)i; in1[(size_t)i*2+1] = -(float)i; }
    CHECK(r.push(in1.data(), 6) == 6);

    // Spotrebuj 4 → r=4, w=6.
    float L, R;
    for (int i = 0; i < 4; ++i) REQUIRE(r.popFrame(L, R));

    // Push dalsich 6 frames → musi se wrappovat (free = 6).
    std::vector<float> in2(12, 0.f);
    for (int i = 0; i < 6; ++i) { in2[(size_t)i*2] = (float)(100+i); in2[(size_t)i*2+1] = -(float)(100+i); }
    CHECK(r.push(in2.data(), 6) == 6);

    // Cti zbyle 8 frames: 4 stare (4,5) + 6 novych (100..105) = 8 (kapacita).
    REQUIRE(r.popFrame(L, R)); CHECK(L == 4.f);
    REQUIRE(r.popFrame(L, R)); CHECK(L == 5.f);
    REQUIRE(r.popFrame(L, R)); CHECK(L == 100.f);
    REQUIRE(r.popFrame(L, R)); CHECK(L == 101.f);
    REQUIRE(r.popFrame(L, R)); CHECK(L == 102.f);
    REQUIRE(r.popFrame(L, R)); CHECK(L == 103.f);
    REQUIRE(r.popFrame(L, R)); CHECK(L == 104.f);
    REQUIRE(r.popFrame(L, R)); CHECK(L == 105.f);
    CHECK_FALSE(r.popFrame(L, R));
}
