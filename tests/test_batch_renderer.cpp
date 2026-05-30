// tests/test_batch_renderer.cpp
// Render par not z fixture banky do WAV, over ze WAV je nenulovy.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "render/batch_renderer.h"
#include "engine.h"
#include "io/wav_reader.h"

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
void writeConstWav(const std::string& p, float amp, int sr=48000) {
    std::FILE* f=std::fopen(p.c_str(),"wb"); REQUIRE(f);
    int frames=sr; uint32_t ds=(uint32_t)frames*4u;       // 1 s
    std::fwrite("RIFF",1,4,f); wU32(f,36u+ds); std::fwrite("WAVE",1,4,f);
    std::fwrite("fmt ",1,4,f); wU32(f,16u); wU16(f,1); wU16(f,2);
    wU32(f,(uint32_t)sr); wU32(f,(uint32_t)sr*4u); wU16(f,4); wU16(f,16);
    std::fwrite("data",1,4,f); wU32(f,ds);
    int16_t v=(int16_t)std::lround(amp*32767.f);
    for(int i=0;i<frames;i++){std::fwrite(&v,2,1,f);std::fwrite(&v,2,1,f);} std::fclose(f);
}
} // namespace

TEST_CASE("renderNotes vyrobi nenulovy WAV z fixture banky") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_render_fixture";
    fs::remove_all(dir); fs::create_directories(dir);
    writeConstWav(dir + "/m060-vel4-f48.wav", 0.5f);

    Engine eng;
    EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 256;
    REQUIRE(eng.init(cfg));
    REQUIRE(eng.loadBank(dir));

    std::string out = "/tmp/ithaca_render_out.wav";
    std::vector<BatchNote> notes = { {60, 100, 0.2f} };   // 1 nota, 0.2 s
    int rendered = renderNotes(eng, notes, out, /*tail_s=*/0.1f);
    fs::remove_all(dir);

    CHECK(rendered == 1);
    WavData w = readWav(out);
    std::remove(out.c_str());
    REQUIRE(w.valid);
    CHECK(w.frames > 0);
    double e = 0; for (float s : w.samples) e += std::fabs((double)s);
    CHECK(e > 0.0);                                        // neco zaznelo
}
