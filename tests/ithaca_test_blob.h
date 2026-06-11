#pragma once
// tests/ithaca_test_blob.h
// Helper: postavi maly validni soundbank.ithaca v temp adresari. Vnitrni
// WAVy = ramp signal pres writeWavStereo16 (PCM16 stereo). Vraci cesty +
// ocekavane hodnoty pro asserty. Pouzivaji testy ithaca_bank / sample_read /
// packed_bank_load / packed_stream.

#include "io/wav_reader.h"
#include "io/wav_writer.h"
#include "sample/ithaca_format.h"
#include "util/sha256.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ithaca_test {

struct BlobSpec {
    int      midi;
    int      frames;
    int      sample_rate;
    float    rms_db;        // bake hodnota (testy ji jen prenaseji)
    uint32_t attack_end;
};

struct BuiltBlob {
    std::string dir;            // temp adresar banky
    std::string ithaca_path;    // dir + "/soundbank.ithaca"
    std::vector<ithaca::IthacaEntry> entries;   // co bylo zapsano
    // Ramp data k porovnani: pro kazdy zaznam plne interleaved stereo float
    // tak, jak je vrati readWav (PCM16 kvantovane).
    std::vector<std::vector<float>> expected_samples;
};

// Ramp signal jako v tests/test_wav_range.cpp: L[i]=i/N, R[i]=-i/N.
inline std::vector<float> rampSamples(int frames) {
    std::vector<float> s((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        s[(size_t)i*2]   =  (float)i / (float)frames;
        s[(size_t)i*2+1] = -(float)i / (float)frames;
    }
    return s;
}

// Najde offset zacatku PCM dat v hotovem WAV souboru (po RIFF hlavicce):
// hleda "data" chunk stejnym pruchodem jako wav_reader::parseHeader.
inline uint32_t findDataOffset(const std::vector<uint8_t>& wav) {
    size_t pos = 12;   // RIFF(4) + size(4) + WAVE(4)
    while (pos + 8 <= wav.size()) {
        uint32_t sz; std::memcpy(&sz, wav.data() + pos + 4, 4);
        if (std::memcmp(wav.data() + pos, "data", 4) == 0)
            return (uint32_t)(pos + 8);
        pos += 8 + sz + (sz & 1u);
    }
    return 0;
}

inline std::vector<uint8_t> readFileBytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

// Postavi .ithaca z danych specu. Zaznamy zapise v poradi specu (testy je
// predavaji uz serazene dle (midi, rms)). corrupt_*: zamerne poskozeni pro
// negativni testy.
inline BuiltBlob buildTestIthaca(const char* tag,
                                 const std::vector<BlobSpec>& specs,
                                 bool corrupt_index_hash = false,
                                 uint32_t force_version = ithaca::kIthacaVersion,
                                 uint32_t force_flags = 0) {
    namespace fs = std::filesystem;
    using namespace ithaca;
    BuiltBlob out;
    out.dir = std::string("/tmp/ithaca_blob_") + tag;
    fs::remove_all(out.dir);
    fs::create_directories(out.dir);
    out.ithaca_path = out.dir + "/" + kIthacaFileName;

    // 1) Vyrob vnitrni WAVy (temp) a nacti jejich bajty.
    std::vector<std::vector<uint8_t>> wavs;
    for (size_t i = 0; i < specs.size(); ++i) {
        std::string wp = out.dir + "/tmp_" + std::to_string(i) + ".wav";
        auto samples = rampSamples(specs[i].frames);
        writeWavStereo16(wp, samples, specs[i].sample_rate);
        wavs.push_back(readFileBytes(wp));
        // Ocekavana data = round-trip pres NAS wav_reader (zadna rucni
        // kvantizace — garantovana shoda s tim, co cte loader).
        ithaca::WavData rd = ithaca::readWav(wp);
        out.expected_samples.push_back(std::move(rd.samples));
        std::remove(wp.c_str());
    }

    // 2) Sekce: metadata JSON, index, names, blob (4K align).
    std::string metadata = "{\"bank_name\":\"test\",\"created_at\":\"2026-06-11\","
        "\"bake_tool_version\":\"test\",\"analysis_preload_ms\":150,"
        "\"source_format\":\"dynamic\"}";
    const uint64_t metadata_offset = kIthacaHeaderSize;
    const uint64_t index_offset    = metadata_offset + metadata.size();
    const uint64_t index_size      = specs.size() * kIthacaEntrySize;
    const uint64_t names_offset    = index_offset + index_size;
    const uint64_t names_size      = 0;     // testy jmena nepotrebuji
    uint64_t blob_offset = names_offset + names_size;
    blob_offset = (blob_offset + kIthacaBlobAlign - 1) / kIthacaBlobAlign
                  * kIthacaBlobAlign;

    std::vector<uint8_t> blob;
    for (size_t i = 0; i < specs.size(); ++i) {
        uint64_t off = blob_offset + blob.size();
        IthacaEntry e;
        e.midi            = (uint16_t)specs[i].midi;
        e.channels        = 2;
        e.sample_rate     = (uint32_t)specs[i].sample_rate;
        e.entry_offset    = off;
        e.entry_size      = wavs[i].size();
        e.pcm_data_offset = findDataOffset(wavs[i]);
        e.sample_format   = kSampleFmtPcm16;
        e.frames          = specs[i].frames;
        e.rms_db          = specs[i].rms_db;
        e.attack_end      = specs[i].attack_end;
        e.name_offset     = kIthacaNoName;
        out.entries.push_back(e);
        blob.insert(blob.end(), wavs[i].begin(), wavs[i].end());
        // 4K zarovnani dalsiho zaznamu.
        while ((blob_offset + blob.size()) % kIthacaBlobAlign != 0)
            blob.push_back(0);
    }
    const uint64_t blob_size = blob.size();

    // 3) Serializace indexu (LE memcpy — zrcadlo parseIthacaIndex).
    std::vector<uint8_t> index_bytes(index_size, 0);
    for (size_t i = 0; i < out.entries.size(); ++i) {
        uint8_t* p = index_bytes.data() + i * kIthacaEntrySize;
        const IthacaEntry& e = out.entries[i];
        std::memcpy(p + 0,  &e.midi, 2);
        std::memcpy(p + 2,  &e.channels, 2);
        std::memcpy(p + 4,  &e.sample_rate, 4);
        std::memcpy(p + 8,  &e.entry_offset, 8);
        std::memcpy(p + 16, &e.entry_size, 8);
        std::memcpy(p + 24, &e.pcm_data_offset, 4);
        std::memcpy(p + 28, &e.sample_format, 2);
        std::memcpy(p + 32, &e.frames, 8);
        std::memcpy(p + 40, &e.rms_db, 4);
        std::memcpy(p + 44, &e.attack_end, 4);
        std::memcpy(p + 48, &e.name_offset, 4);
    }

    // 4) Hashe: sha256_index pres metadata+index+names; payload pres blob.
    Sha256 hi;
    hi.update(metadata.data(), metadata.size());
    hi.update(index_bytes.data(), index_bytes.size());
    auto sha_index = hi.finish();
    if (corrupt_index_hash) sha_index[0] ^= 0xFF;
    auto sha_payload = Sha256::hash(blob.data(), blob.size());

    // 5) Hlavicka.
    std::vector<uint8_t> header(kIthacaHeaderSize, 0);
    std::memcpy(header.data(), kIthacaMagic, 8);
    std::memcpy(header.data() + 8,  &force_version, 4);
    std::memcpy(header.data() + 12, &force_flags, 4);
    std::memcpy(header.data() + 16, &metadata_offset, 8);
    uint64_t msz = metadata.size();
    std::memcpy(header.data() + 24, &msz, 8);
    std::memcpy(header.data() + 32, &index_offset, 8);
    std::memcpy(header.data() + 40, &index_size, 8);
    std::memcpy(header.data() + 48, &names_offset, 8);
    std::memcpy(header.data() + 56, &names_size, 8);
    std::memcpy(header.data() + 64, &blob_offset, 8);
    std::memcpy(header.data() + 72, &blob_size, 8);
    uint32_t ec = (uint32_t)out.entries.size();
    std::memcpy(header.data() + 80, &ec, 4);
    std::memcpy(header.data() + 88,  sha_index.data(), 32);
    std::memcpy(header.data() + 120, sha_payload.data(), 32);

    // 6) Zapis: hlavicka + metadata + index + (names) + pad + blob.
    std::ofstream f(out.ithaca_path, std::ios::binary);
    f.write((const char*)header.data(), (std::streamsize)header.size());
    f.write(metadata.data(), (std::streamsize)metadata.size());
    f.write((const char*)index_bytes.data(), (std::streamsize)index_bytes.size());
    uint64_t pad = blob_offset - (names_offset + names_size);
    std::vector<char> zeros((size_t)pad, 0);
    f.write(zeros.data(), (std::streamsize)zeros.size());
    f.write((const char*)blob.data(), (std::streamsize)blob.size());
    f.close();
    return out;
}

// Uklid temp adresare (volat na konci testu).
inline void removeBlob(const BuiltBlob& b) {
    std::filesystem::remove_all(b.dir);
}

} // namespace ithaca_test
