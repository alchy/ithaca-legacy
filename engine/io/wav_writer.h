#pragma once
// engine/io/wav_writer.h
// ----------------------
// Zapis interleaved stereo float bufferu do 16-bit PCM stereo WAV. Pouziva
// batch renderer (offline render bez audio device) a testy. Float [-1,1] se
// clampuje a kvantuje na int16.

#include <string>
#include <vector>

namespace ithaca {

// Zapise `samples` (interleaved stereo [L,R,...]) jako 16-bit PCM stereo WAV.
// Vrati false kdyz nelze otevrit soubor pro zapis.
bool writeWavStereo16(const std::string& path, const std::vector<float>& samples,
                      int sample_rate);

} // namespace ithaca
