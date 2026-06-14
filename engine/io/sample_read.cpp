// engine/io/sample_read.cpp — viz sample_read.h.
#include "io/sample_read.h"

#include "io/file_handle.h"
#include "sample/ithaca_format.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ithaca {

namespace {
// kSampleFmt → (bits, audio_format) pro wavSampleToFloat.
void fmtToWav(uint16_t f, uint16_t& bits, uint16_t& audio_format) {
    switch (f) {
        case kSampleFmtPcm16:   bits = 16; audio_format = 1; break;
        case kSampleFmtPcm24:   bits = 24; audio_format = 1; break;
        case kSampleFmtFloat32: bits = 32; audio_format = 3; break;
        case kSampleFmtPcm32:   bits = 32; audio_format = 1; break;
        default:                bits = 0;  audio_format = 0; break;
    }
}
} // namespace

WavData readSampleRange(const SampleFile& file, int64_t frame_off,
                        int64_t frame_count) {
    if (!file.blob)
        return readWavRange(file.path, frame_off, frame_count);

    WavData out;
    if (frame_off < 0 || frame_count < 0) {
        return out;   // shodne s readWavRange (zaporne vstupy → invalid)
    }
    const int bps = sampleFormatBytes(file.sample_format);
    if (bps == 0 || (file.channels != 1 && file.channels != 2)) return out;

    out.sample_rate = file.sample_rate;
    const int64_t total = (int64_t)file.frames;
    if (frame_count == 0 || frame_off >= total) {
        out.valid = true;   // EOF/0-pozadavek → valid, 0 frames
        return out;
    }
    const int64_t n = std::min(frame_count, total - frame_off);
    const int     frame_bytes = bps * (int)file.channels;
    std::vector<uint8_t> raw((size_t)n * (size_t)frame_bytes);
    if (!file.blob->readAt(
            file.pcm_offset + (uint64_t)frame_off * (uint64_t)frame_bytes,
            raw.data(), raw.size()))
        return out;   // kratke cteni = poskozeny soubor → invalid

    uint16_t bits = 0, afmt = 0;
    fmtToWav(file.sample_format, bits, afmt);
    out.samples.resize((size_t)n * 2);
    for (int64_t i = 0; i < n; ++i) {
        const uint8_t* base = raw.data() + (size_t)i * (size_t)frame_bytes;
        float L = wavSampleToFloat(base, bits, afmt);
        float R = (file.channels >= 2) ? wavSampleToFloat(base + bps, bits, afmt)
                                       : L;   // mono → zdvoj (jako wav_reader)
        out.samples[(size_t)i * 2]     = L;
        out.samples[(size_t)i * 2 + 1] = R;
    }
    out.frames = (int)std::min<int64_t>(n, INT32_MAX);
    out.valid  = true;
    return out;
}

} // namespace ithaca
