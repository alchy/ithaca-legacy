// engine/io/wav_writer.cpp — viz wav_writer.h.
#include "io/wav_writer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace ithaca {

namespace {
void wU32(std::FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void wU16(std::FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }
} // namespace

bool writeWavStereo16(const std::string& path, const std::vector<float>& samples,
                      int sample_rate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    const uint32_t n_samp    = (uint32_t)samples.size();      // celkem L+R hodnot
    const uint32_t data_size = n_samp * 2u;                   // 2 bajty / hodnota (16-bit)
    const uint16_t channels  = 2;
    const uint32_t byte_rate = (uint32_t)sample_rate * channels * 2u;

    std::fwrite("RIFF", 1, 4, f); wU32(f, 36u + data_size);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); wU32(f, 16u);
    wU16(f, 1);                                               // PCM
    wU16(f, channels);
    wU32(f, (uint32_t)sample_rate);
    wU32(f, byte_rate);
    wU16(f, (uint16_t)(channels * 2));                        // block align
    wU16(f, 16);                                              // bits per sample
    std::fwrite("data", 1, 4, f); wU32(f, data_size);

    for (float s : samples) {
        if (s > 1.f) s = 1.f;
        if (s < -1.f) s = -1.f;
        int16_t v = (int16_t)std::lround(s * 32767.f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

} // namespace ithaca
