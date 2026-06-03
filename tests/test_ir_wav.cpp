#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "dsp/ir_wav.h"
#include <vector>
using namespace ithaca::dsp;
TEST_CASE("loadIrWavMono: chybna cesta → false") {
    std::vector<float> ir;
    CHECK_FALSE(loadIrWavMono("/tmp/ithaca_nope_xyz.wav", 48000.f, 8192, ir));
}
