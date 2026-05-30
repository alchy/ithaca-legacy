// tests/test_engine.cpp
// Engine fasada: nacti malou banku (fixture), noteOn pres frontu, processBlock
// vyrobi zvuk. Bez audio device — voláme processBlock primo.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine.h"

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
    int frames=sr/2; uint32_t ds=(uint32_t)frames*4u;
    std::fwrite("RIFF",1,4,f); wU32(f,36u+ds); std::fwrite("WAVE",1,4,f);
    std::fwrite("fmt ",1,4,f); wU32(f,16u); wU16(f,1); wU16(f,2);
    wU32(f,(uint32_t)sr); wU32(f,(uint32_t)sr*4u); wU16(f,4); wU16(f,16);
    std::fwrite("data",1,4,f); wU32(f,ds);
    int16_t v=(int16_t)std::lround(amp*32767.f);
    for(int i=0;i<frames;i++){std::fwrite(&v,2,1,f);std::fwrite(&v,2,1,f);} std::fclose(f);
}
double energy(const std::vector<float>& b){double s=0;for(float v:b)s+=std::fabs((double)v);return s;}
} // namespace

TEST_CASE("Engine: load bank, noteOn pres frontu, processBlock vyrobi zvuk") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_engine_fixture";
    fs::remove_all(dir); fs::create_directories(dir);
    writeConstWav(dir + "/m060-vel4-f48.wav", 0.5f);

    Engine eng;
    EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 256; cfg.max_voices = 32;
    REQUIRE(eng.init(cfg));
    REQUIRE(eng.loadBank(dir));
    fs::remove_all(dir);

    eng.noteOn(60, 100);                      // vlozi do fronty
    std::vector<float> L(256, 0.f), R(256, 0.f);
    eng.processBlock(L.data(), R.data(), 256); // drainuje frontu + renderuje
    CHECK(energy(L) > 0.0);
}

TEST_CASE("Engine: prazdna banka → processBlock je ticho, ne crash") {
    Engine eng;
    EngineConfig cfg;
    REQUIRE(eng.init(cfg));
    eng.noteOn(60, 100);
    std::vector<float> L(256, 0.f), R(256, 0.f);
    eng.processBlock(L.data(), R.data(), 256);
    CHECK(energy(L) == doctest::Approx(0.0));
}
