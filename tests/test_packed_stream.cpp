// tests/test_packed_stream.cpp
// Streaming worker cte z blobu pres readSampleRange: request s packed
// lokatorem naplni ring stejnymi daty jako WAV soubor.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "ithaca_test_blob.h"
#include "io/file_handle.h"
#include "stream/stream_engine.h"

#include <chrono>
#include <thread>

using namespace ithaca;
using namespace ithaca_test;

namespace {
// Pocka az ring ma aspon n frames (timeout 2 s).
bool waitAvail(RingHandle* r, int n) {
    for (int i = 0; i < 200; ++i) {
        if (r->available() >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

TEST_CASE("stream worker plni ring z packed blobu") {
    BuiltBlob b = buildTestIthaca("stream_ring", {{60, 16384, 48000, -25.f, 10}});
    auto h = openFileHandle(b.ithaca_path);
    REQUIRE(h != nullptr);
    SampleFile f;
    f.path          = b.ithaca_path;
    f.frames        = 16384; f.sample_rate = 48000; f.valid = true;
    f.blob          = h;
    f.pcm_offset    = b.entries[0].entry_offset + b.entries[0].pcm_data_offset;
    f.channels      = 2;
    f.sample_format = kSampleFmtPcm16;

    StreamEngine se(/*n_rings=*/2, /*ring_capacity_frames=*/4096, /*n_workers=*/1);
    se.start();
    RingHandle* ring = se.acquireRing();
    REQUIRE(ring != nullptr);
    // Pozadej 1024 frames od offsetu 8000.
    REQUIRE(se.requestRead(ring, f, 8000, 1024, /*eof_when_done=*/false));
    REQUIRE(waitAvail(ring, 1024));
    for (int i = 0; i < 1024; ++i) {
        float L, R;
        REQUIRE(ring->popFrame(L, R));
        REQUIRE(L == b.expected_samples[0][(size_t)(8000 + i) * 2]);
        REQUIRE(R == b.expected_samples[0][(size_t)(8000 + i) * 2 + 1]);
    }
    se.releaseRing(ring);
    se.stop();
    removeBlob(b);
}

TEST_CASE("stream worker EOF na konci packed samplu") {
    BuiltBlob b = buildTestIthaca("stream_eof", {{60, 4096, 48000, -25.f, 10}});
    auto h = openFileHandle(b.ithaca_path);
    REQUIRE(h != nullptr);
    SampleFile f;
    f.path = b.ithaca_path; f.frames = 4096; f.sample_rate = 48000; f.valid = true;
    f.blob = h;
    f.pcm_offset    = b.entries[0].entry_offset + b.entries[0].pcm_data_offset;
    f.channels      = 2;
    f.sample_format = kSampleFmtPcm16;

    StreamEngine se(2, 8192, 1);
    se.start();
    RingHandle* ring = se.acquireRing();
    REQUIRE(ring != nullptr);
    REQUIRE(se.requestRead(ring, f, 0, 4096, /*eof_when_done=*/true));
    REQUIRE(waitAvail(ring, 4096));
    // Po dokonceni requestu s eof_when_done nastavi worker eof_.
    for (int i = 0; i < 200 && !ring->eof_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(ring->eof_.load());
    se.releaseRing(ring);
    se.stop();
    removeBlob(b);
}
