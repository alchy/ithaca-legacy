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

TEST_CASE("processBlock inkrementuje block epoch (podklad reload/recache handshake)") {
    Engine e;
    EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 64;
    cfg.midi_from = 59; cfg.midi_to = 61;
    REQUIRE(e.init(cfg));
    const uint64_t e0 = e.blockEpoch();
    std::vector<float> L(64, 0.f), R(64, 0.f);
    e.processBlock(L.data(), R.data(), 64);
    e.processBlock(L.data(), R.data(), 64);
    CHECK(e.blockEpoch() == e0 + 2);
}

TEST_CASE("noteOn/noteOff s out-of-range parametry jsou bezpecne (clamp/zahozeni)") {
    Engine e;
    EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 64;
    cfg.midi_from = 59; cfg.midi_to = 61;
    REQUIRE(e.init(cfg));
    e.noteOn(200, 100);    // midi mimo rozsah → zahodit ((uint8_t)200 by hral jinou notu)
    e.noteOn(-3, 100);
    e.noteOn(60, 300);     // velocity > 127 → clamp ((uint8_t)300==44, 256==0 → falesny NoteOff)
    e.noteOff(-5);
    std::vector<float> L(64, 0.f), R(64, 0.f);
    e.processBlock(L.data(), R.data(), 64);   // nesmi spadnout / UB (overi i ASan)
    CHECK(true);
}

// -- EngineDiag snapshot (R2 z ARCHITECTURE_REVIEW) --------------------------
// diag() je kanonicka ctecí cesta pro GUI — jedno volani misto tuctu getteru.
// Testuje se proti zbyvajicim getterum (kde existuji) a proti ocekavanemu
// stavu tam, kde getter zanikl (bank fakta, note ages).

TEST_CASE("diag: cerstvy engine — nuly, kNever stari, Unknown banka") {
    Engine eng;
    EngineConfig cfg;
    REQUIRE(eng.init(cfg));
    const EngineDiag d = eng.diag();

    CHECK(d.active_voices == 0);
    CHECK(d.resonance_voices == 0);
    CHECK(d.pedal_cc == 0);
    CHECK(d.bank_type == BankFormat::Unknown);
    CHECK(d.loaded_samples == 0);
    CHECK(d.recorded_notes == 0);
    // Zadna udalost jeste nenastala → stari je "nikdy", ne nula. Na tom stoji
    // GUI lampy: age < okno nesmi pri startu bliknout.
    CHECK(d.note_on_age_ms  > 1e9f);
    CHECK(d.note_off_age_ms > 1e9f);
    CHECK(d.overload_age_ms > 1e9f);
    CHECK(d.main_underrun_age_ms > 1e9f);
    CHECK(d.reso_underrun_age_ms > 1e9f);
    // Ring pooly existuji hned po init a souhlasi s gettery drzenymi testy.
    CHECK(d.main_rings_total == eng.mainRingsTotal());
    CHECK(d.reso_rings_total == eng.resonanceRingsTotal());
    CHECK(d.main_rings_total > 0);
}

TEST_CASE("diag: zrcadli zive gettery a bank fakta po nacteni") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_diag_fixture";
    fs::remove_all(dir); fs::create_directories(dir);
    writeConstWav(dir + "/m060-vel4-f48.wav", 0.5f);

    Engine eng;
    EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 256;
    REQUIRE(eng.init(cfg));
    REQUIRE(eng.loadBank(dir));
    fs::remove_all(dir);

    eng.noteOn(60, 100);
    eng.sustainPedal(64);
    std::vector<float> L(256, 0.f), R(256, 0.f);
    eng.processBlock(L.data(), R.data(), 256);

    const EngineDiag d = eng.diag();
    CHECK(d.active_voices == eng.activeVoices());
    CHECK(d.active_voices >= 1);
    CHECK(d.pedal_cc == eng.pedalCC());
    CHECK(d.pedal_cc == 64);
    CHECK(d.master_peak_l == doctest::Approx(eng.masterPeakL()));
    CHECK(d.dsp_load_peak == doctest::Approx(eng.dspLoadPeak()).epsilon(0.5));
    // Note-on prave probehl → stari je male; note-off zadny nebyl.
    CHECK(d.note_on_age_ms < 60000.f);
    CHECK(d.note_off_age_ms > 1e9f);
    // Fixture banka: 1 nota, 1 sample, format detekovan (ne Unknown).
    CHECK(d.bank_type != BankFormat::Unknown);
    CHECK(d.recorded_notes == 1);
    CHECK(d.loaded_samples >= 1);
}
