#pragma once
// engine/dsp/ir_wav.h — nacti mono IR z WAV (float32/int16), downmix→mono,
// resample na engine_sr (linearne), orizni na max_len. Pro Convolver.
#include <string>
#include <vector>
namespace ithaca::dsp {
// Vrati true + naplni `out` (mono float, delka <= max_len). false pri chybe.
bool loadIrWavMono(const std::string& path, float engine_sr, int max_len, std::vector<float>& out);
}
