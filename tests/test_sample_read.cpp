// tests/test_sample_read.cpp
// readSampleRange: dispatcher WAV cesta vs. blob. Blob cteni musi byt
// bit-exact shodne s readWavRange nad puvodnim WAVem (stejna konverze
// wavSampleToFloat, stejna EOF semantika).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "io/file_handle.h"
#include "io/sample_read.h"
#include "ithaca_test_blob.h"

using namespace ithaca;
using namespace ithaca_test;

namespace {
// SampleFile lokator pro i-ty zaznam testovaciho blobu.
SampleFile locatorFor(const BuiltBlob& b, std::shared_ptr<IFileHandle> h, int i) {
    const IthacaEntry& e = b.entries[(size_t)i];
    SampleFile f;
    f.path          = b.ithaca_path;
    f.frames        = (int)e.frames;
    f.sample_rate   = (int)e.sample_rate;
    f.valid         = true;
    f.blob          = std::move(h);
    f.pcm_offset    = e.entry_offset + e.pcm_data_offset;
    f.channels      = e.channels;
    f.sample_format = e.sample_format;
    return f;
}
} // namespace

TEST_CASE("readSampleRange blob == ocekavana ramp data (cely + vyrez)") {
    BuiltBlob b = buildTestIthaca("sr_basic", {{60, 4096, 48000, -30.f, 10}});
    auto h = openFileHandle(b.ithaca_path);
    REQUIRE(h != nullptr);
    SampleFile f = locatorFor(b, h, 0);

    WavData whole = readSampleRange(f, 0, 4096);
    REQUIRE(whole.valid);
    CHECK(whole.frames == 4096);
    CHECK(whole.sample_rate == 48000);
    REQUIRE(whole.samples.size() == b.expected_samples[0].size());
    for (size_t i = 0; i < whole.samples.size(); ++i)
        REQUIRE(whole.samples[i] == b.expected_samples[0][i]);   // bit-exact

    WavData mid = readSampleRange(f, 1000, 256);
    REQUIRE(mid.valid);
    CHECK(mid.frames == 256);
    CHECK(mid.samples[0] == b.expected_samples[0][1000 * 2]);
    CHECK(mid.samples[1] == b.expected_samples[0][1000 * 2 + 1]);
    removeBlob(b);
}

TEST_CASE("readSampleRange blob EOF semantika jako readWavRange") {
    BuiltBlob b = buildTestIthaca("sr_eof", {{60, 4096, 48000, -30.f, 10}});
    auto h = openFileHandle(b.ithaca_path);
    SampleFile f = locatorFor(b, h, 0);

    WavData clip = readSampleRange(f, 4000, 1000);   // orizne na 96
    REQUIRE(clip.valid);
    CHECK(clip.frames == 96);

    WavData past = readSampleRange(f, 4096, 100);    // za koncem → 0, valid
    REQUIRE(past.valid);
    CHECK(past.frames == 0);
    CHECK(past.samples.empty());

    WavData neg = readSampleRange(f, -1, 100);       // chyba
    CHECK_FALSE(neg.valid);
    removeBlob(b);
}

TEST_CASE("readSampleRange bez blobu deleguje na readWavRange") {
    // Bezny WAV na disku — dispatcher musi vratit totez co readWavRange.
    std::string p = "/tmp/ithaca_sr_plain.wav";
    auto samples = rampSamples(2048);
    REQUIRE(writeWavStereo16(p, samples, 48000));
    SampleFile f;
    f.path = p; f.frames = 2048; f.sample_rate = 48000; f.valid = true;
    WavData a = readSampleRange(f, 500, 100);
    WavData b2 = readWavRange(p, 500, 100);
    REQUIRE(a.valid); REQUIRE(b2.valid);
    REQUIRE(a.samples.size() == b2.samples.size());
    for (size_t i = 0; i < a.samples.size(); ++i)
        REQUIRE(a.samples[i] == b2.samples[i]);
    std::remove(p.c_str());
}

TEST_CASE("readSampleRange odmitne neznamy sample_format") {
    BuiltBlob b = buildTestIthaca("sr_fmt", {{60, 1024, 48000, -30.f, 10}});
    auto h = openFileHandle(b.ithaca_path);
    SampleFile f = locatorFor(b, h, 0);
    f.sample_format = 99;
    CHECK_FALSE(readSampleRange(f, 0, 10).valid);
    removeBlob(b);
}

TEST_CASE("readSampleRange blob: mono PCM16 (mono->stereo double)") {
    BuiltBlob b = buildTestIthaca("sr_mono", {
        {60, 2048, 48000, -30.f, 10, /*channels=*/1, kSampleFmtPcm16}});
    auto h = openFileHandle(b.ithaca_path);
    REQUIRE(h != nullptr);
    SampleFile f = locatorFor(b, h, 0);
    WavData w = readSampleRange(f, 0, 2048);
    REQUIRE(w.valid);
    CHECK(w.frames == 2048);
    REQUIRE(w.samples.size() == b.expected_samples[0].size());
    for (size_t i = 0; i < w.samples.size(); ++i)
        REQUIRE(w.samples[i] == b.expected_samples[0][i]);   // bit-exact, L==R
    removeBlob(b);
}

TEST_CASE("readSampleRange blob: stereo PCM24 (bps=3 offset aritmetika)") {
    BuiltBlob b = buildTestIthaca("sr_pcm24", {
        {60, 2048, 48000, -30.f, 10, /*channels=*/2, kSampleFmtPcm24}});
    auto h = openFileHandle(b.ithaca_path);
    REQUIRE(h != nullptr);
    SampleFile f = locatorFor(b, h, 0);
    WavData whole = readSampleRange(f, 0, 2048);
    REQUIRE(whole.valid);
    REQUIRE(whole.samples.size() == b.expected_samples[0].size());
    for (size_t i = 0; i < whole.samples.size(); ++i)
        REQUIRE(whole.samples[i] == b.expected_samples[0][i]);   // bit-exact
    WavData mid = readSampleRange(f, 1000, 200);   // vyrez — overit offset
    REQUIRE(mid.valid);
    CHECK(mid.frames == 200);
    CHECK(mid.samples[0] == b.expected_samples[0][1000 * 2]);
    CHECK(mid.samples[1] == b.expected_samples[0][1000 * 2 + 1]);
    removeBlob(b);
}
