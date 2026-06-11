// engine/sample/ithaca_format.cpp — viz ithaca_format.h.
#include "sample/ithaca_format.h"

#include <cstring>

namespace ithaca {

namespace {
// LE cteni pres memcpy. Pozn.: vsechny cilove platformy jsou LE (stejna
// konvence jako wav_reader.cpp); big-endian port by zde byte-swapoval.
template <typename T>
T rdLE(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }
} // namespace

bool parseIthacaHeader(const uint8_t* buf, size_t n, IthacaHeader& out) {
    if (n < kIthacaHeaderSize) return false;
    if (std::memcmp(buf, kIthacaMagic, 8) != 0) return false;
    out.version         = rdLE<uint32_t>(buf + 8);
    out.flags           = rdLE<uint32_t>(buf + 12);
    out.metadata_offset = rdLE<uint64_t>(buf + 16);
    out.metadata_size   = rdLE<uint64_t>(buf + 24);
    out.index_offset    = rdLE<uint64_t>(buf + 32);
    out.index_size      = rdLE<uint64_t>(buf + 40);
    out.names_offset    = rdLE<uint64_t>(buf + 48);
    out.names_size      = rdLE<uint64_t>(buf + 56);
    out.blob_offset     = rdLE<uint64_t>(buf + 64);
    out.blob_size       = rdLE<uint64_t>(buf + 72);
    out.entry_count     = rdLE<uint32_t>(buf + 80);
    std::memcpy(out.sha256_index.data(),   buf + 88,  32);
    std::memcpy(out.sha256_payload.data(), buf + 120, 32);
    return true;
}

bool parseIthacaIndex(const uint8_t* buf, size_t n, uint32_t entry_count,
                      std::vector<IthacaEntry>& out) {
    if (n != (size_t)entry_count * kIthacaEntrySize) return false;
    out.clear();
    out.reserve(entry_count);
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint8_t* p = buf + (size_t)i * kIthacaEntrySize;
        IthacaEntry e;
        e.midi            = rdLE<uint16_t>(p + 0);
        e.channels        = rdLE<uint16_t>(p + 2);
        e.sample_rate     = rdLE<uint32_t>(p + 4);
        e.entry_offset    = rdLE<uint64_t>(p + 8);
        e.entry_size      = rdLE<uint64_t>(p + 16);
        e.pcm_data_offset = rdLE<uint32_t>(p + 24);
        e.sample_format   = rdLE<uint16_t>(p + 28);
        e.frames          = rdLE<int64_t> (p + 32);
        e.rms_db          = rdLE<float>   (p + 40);
        e.attack_end      = rdLE<uint32_t>(p + 44);
        e.name_offset     = rdLE<uint32_t>(p + 48);
        out.push_back(e);
    }
    return true;
}

} // namespace ithaca
