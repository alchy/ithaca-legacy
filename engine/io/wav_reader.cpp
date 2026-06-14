// engine/io/wav_reader.cpp — viz wav_reader.h.
#include "io/wav_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ithaca {

namespace {

// Spolecny vysledek parsovani fmt chunku.
struct FmtInfo {
    uint16_t audio_format = 0;   // 1 = PCM, 3 = IEEE float
    uint16_t channels     = 0;
    uint32_t sample_rate  = 0;
    uint16_t bits         = 0;
    bool     have         = false;
};

// Najde fmt chunk a (volitelne) pozici+velikost data chunku. Soubor je po
// navratu pozicovan na zacatek data chunku (kdyz found_data=true).
bool parseHeader(std::FILE* f, FmtInfo& fmt,
                 uint32_t& data_size, bool& found_data) {
    found_data = false;
    char riff[4];
    if (std::fread(riff, 1, 4, f) != 4 || std::strncmp(riff, "RIFF", 4) != 0)
        return false;
    std::fseek(f, 4, SEEK_CUR);  // preskoc chunk size
    char wave[4];
    if (std::fread(wave, 1, 4, f) != 4 || std::strncmp(wave, "WAVE", 4) != 0)
        return false;

    while (true) {
        char chunk_id[4];
        if (std::fread(chunk_id, 1, 4, f) != 4) break;
        uint32_t chunk_size = 0;
        if (std::fread(&chunk_size, 4, 1, f) != 1) break;

        if (std::strncmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t raw[8] = {0};
            uint32_t to_read = chunk_size < 16u ? chunk_size : 16u;
            std::fread(raw, 1, to_read, f);
            fmt.audio_format = raw[0];
            fmt.channels     = raw[1];
            // Pozn.: WAV je little-endian a vsechny cilove platformy (x86/ARM
            // vc. Raspberry Pi) jsou tez LE, takze cteme primo. Pripadny
            // big-endian port by zde musel byte-swapovat.
            std::memcpy(&fmt.sample_rate, &raw[2], 4);
            fmt.bits         = raw[7];
            fmt.have         = true;
            if (chunk_size > 16u)
                std::fseek(f, (long)(chunk_size - 16u), SEEK_CUR);
        } else if (std::strncmp(chunk_id, "data", 4) == 0) {
            data_size  = chunk_size;
            found_data = true;
            return fmt.have;   // soubor pozicovan na zacatku dat
        } else {
            // Preskoc neznamy chunk (word-aligned).
            std::fseek(f, (long)(chunk_size + (chunk_size & 1u)), SEEK_CUR);
        }
    }
    return false;
}

} // namespace

// Prevede jeden vzorek surovych bajtu na float [-1,1] podle formatu. Sdileno
// s dekodovanim blobu (sample_read.cpp) pro bit-exact cteni.
float wavSampleToFloat(const uint8_t* p, uint16_t bits, uint16_t audio_format) {
    if (audio_format == 3 && bits == 32) {          // IEEE float
        float v; std::memcpy(&v, p, 4); return v;
    }
    if (bits == 16) {
        int16_t v; std::memcpy(&v, p, 2);
        return (float)v / 32768.f;
    }
    if (bits == 24) {
        int32_t v = (p[0]) | (p[1] << 8) | (p[2] << 16);
        if (v & 0x800000) v |= ~0xFFFFFF;            // sign-extend
        return (float)v / 8388608.f;
    }
    if (bits == 32) {                                // 32-bit PCM
        int32_t v; std::memcpy(&v, p, 4);
        return (float)v / 2147483648.f;
    }
    return 0.f;
}

WavData readWav(const std::string& path) {
    WavData out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;

    FmtInfo fmt; uint32_t data_size = 0; bool found_data = false;
    if (!parseHeader(f, fmt, data_size, found_data) || !found_data) {
        std::fclose(f);
        return out;
    }

    const int bytes_per_sample = fmt.bits / 8;
    if (bytes_per_sample <= 0 || fmt.channels == 0) { std::fclose(f); return out; }

    std::vector<uint8_t> raw(data_size);
    size_t got = std::fread(raw.data(), 1, data_size, f);
    std::fclose(f);
    if (got < data_size) data_size = (uint32_t)got;   // tolerantni k oriznutemu souboru

    const int total_samples = (int)(data_size / (uint32_t)bytes_per_sample);
    const int frames = total_samples / fmt.channels;

    out.samples.resize((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        const uint8_t* base = raw.data() + (size_t)i * fmt.channels * bytes_per_sample;
        float L = wavSampleToFloat(base, fmt.bits, fmt.audio_format);
        float R = (fmt.channels >= 2)
                ? wavSampleToFloat(base + bytes_per_sample, fmt.bits, fmt.audio_format)
                : L;                                  // mono → zdvoj
        out.samples[(size_t)i * 2]     = L;
        out.samples[(size_t)i * 2 + 1] = R;
    }
    out.frames      = frames;
    out.sample_rate = (int)fmt.sample_rate;
    out.valid       = true;
    return out;
}

WavData readWavRange(const std::string& path, int64_t frame_off, int64_t frame_count) {
    WavData out;
    if (frame_off < 0 || frame_count <= 0) {
        // Defensivne: 0 frame_count -> 0-frames valid result.
        if (frame_off >= 0 && frame_count == 0) { out.valid = true; }
        return out;
    }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;

    FmtInfo fmt; uint32_t data_size = 0; bool found_data = false;
    if (!parseHeader(f, fmt, data_size, found_data) || !found_data) {
        std::fclose(f);
        return out;
    }
    const int bytes_per_sample = fmt.bits / 8;
    if (bytes_per_sample <= 0 || fmt.channels == 0) { std::fclose(f); return out; }

    // parseHeader nechal soubor pozicovany na zacatku data chunku. Zjisti realny
    // zbytek v souboru (header data_size muze lhat — viz peekWavInfo).
    long data_start = std::ftell(f);
    std::fseek(f, 0, SEEK_END);
    long file_end = std::ftell(f);
    uint32_t avail_bytes = (file_end > data_start)
                         ? (uint32_t)(file_end - data_start) : 0u;
    if (avail_bytes < data_size) data_size = avail_bytes;

    const int     frame_bytes  = bytes_per_sample * fmt.channels;
    const int64_t total_frames = (int64_t)data_size / (int64_t)frame_bytes;

    // Offset za koncem souboru → 0 frames, ale valid (signal "konec streamu").
    if (frame_off >= total_frames) {
        std::fclose(f);
        out.frames      = 0;
        out.sample_rate = (int)fmt.sample_rate;
        out.valid       = true;
        return out;
    }

    int64_t avail_frames = total_frames - frame_off;
    int64_t read_frames  = (frame_count < avail_frames) ? frame_count : avail_frames;

    // 64-bit seek pro WAV >2 GB (streaming worker pro velke samply).
    int64_t byte_off = (int64_t)data_start + frame_off * (int64_t)frame_bytes;
#if defined(_WIN32)
    _fseeki64(f, byte_off, SEEK_SET);
#else
    fseeko(f, (off_t)byte_off, SEEK_SET);
#endif

    std::vector<uint8_t> raw((size_t)read_frames * (size_t)frame_bytes);
    size_t got = std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    int64_t actual_frames = (int64_t)(got / (size_t)frame_bytes);

    out.samples.resize((size_t)actual_frames * 2);
    for (int64_t i = 0; i < actual_frames; ++i) {
        const uint8_t* base = raw.data() + (size_t)i * fmt.channels * bytes_per_sample;
        float L = wavSampleToFloat(base, fmt.bits, fmt.audio_format);
        float R = (fmt.channels >= 2)
                ? wavSampleToFloat(base + bytes_per_sample, fmt.bits, fmt.audio_format)
                : L;
        out.samples[(size_t)i * 2]     = L;
        out.samples[(size_t)i * 2 + 1] = R;
    }
    // FUTURE: WavData.frames -> int64 kdyz bude potreba. Per-call read_frames je
    // bounded velikosti pozadavku (typicky maly: preload nebo streaming chunk).
    if (actual_frames > (int64_t)INT32_MAX) actual_frames = INT32_MAX;
    out.frames      = (int)actual_frames;
    out.sample_rate = (int)fmt.sample_rate;
    out.valid       = true;
    return out;
}

WavInfo peekWavInfo(const std::string& path) {
    WavInfo out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    FmtInfo fmt; uint32_t data_size = 0; bool found_data = false;
    bool ok = parseHeader(f, fmt, data_size, found_data);
    if (!ok || !found_data) {
        std::fclose(f);
        return out;
    }

    // parseHeader nechal soubor pozicovany na zacatku data chunku. Header muze
    // lhat (oriznuty/streamovany soubor), takze data_size orizneme na skutecny
    // zbytek souboru do EOF.
    long data_pos = std::ftell(f);
    std::fseek(f, 0, SEEK_END);
    long file_end = std::ftell(f);
    if (data_pos >= 0 && file_end >= data_pos) {
        uint32_t avail = (uint32_t)(file_end - data_pos);
        if (avail < data_size) data_size = avail;
    }
    std::fclose(f);

    const int bytes_per_sample = fmt.bits / 8;
    if (bytes_per_sample <= 0 || fmt.channels == 0) return out;
    out.channels    = fmt.channels;
    out.sample_rate = (int)fmt.sample_rate;
    out.frames      = (int)(data_size / (uint32_t)(bytes_per_sample * fmt.channels));
    out.valid       = true;
    return out;
}

} // namespace ithaca
