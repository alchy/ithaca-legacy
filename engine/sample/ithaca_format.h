#pragma once
// engine/sample/ithaca_format.h
// Binarni format pakovane banky soundbank.ithaca (little-endian) — konstanty,
// POD struktury a CISTE parsovaci funkce nad byte bufferem (testovatelne bez
// disku). Layout: docs/bank-format-packed.md; rozhodnuti: spec
// docs/superpowers/specs/2026-06-10-packed-soundbank-design.md.
// Soubor: [hlavicka 408 B][metadata JSON][index 64 B/zaznam][names][blob].

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ithaca {

inline constexpr char     kIthacaMagic[8]   = {'I','T','H','A','C','A','B','K'};
inline constexpr uint32_t kIthacaVersion    = 1;
inline constexpr size_t   kIthacaHeaderSize = 408;
inline constexpr size_t   kIthacaEntrySize  = 64;
inline constexpr uint32_t kIthacaNoName     = 0xFFFFFFFFu;
inline constexpr uint64_t kIthacaBlobAlign  = 4096;
inline constexpr const char* kIthacaFileName = "soundbank.ithaca";

// flags bity (v1 musi byt 0 — rezervovano pro v2 sifru/podpis):
inline constexpr uint32_t kIthacaFlagEncrypted = 1u << 0;
inline constexpr uint32_t kIthacaFlagSigned    = 1u << 1;

// sample_format kody (pokryvaji formaty wav_readeru):
inline constexpr uint16_t kSampleFmtPcm16   = 1;
inline constexpr uint16_t kSampleFmtPcm24   = 2;
inline constexpr uint16_t kSampleFmtFloat32 = 3;
inline constexpr uint16_t kSampleFmtPcm32   = 4;

struct IthacaHeader {
    uint32_t version = 0;
    uint32_t flags   = 0;
    uint64_t metadata_offset = 0, metadata_size = 0;
    uint64_t index_offset = 0,    index_size = 0;
    uint64_t names_offset = 0,    names_size = 0;
    uint64_t blob_offset = 0,     blob_size = 0;
    uint32_t entry_count = 0;
    std::array<uint8_t, 32> sha256_index{};     // pres metadata+index+names
    std::array<uint8_t, 32> sha256_payload{};   // pres blob (jen --verify)
};

struct IthacaEntry {
    uint16_t midi = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint64_t entry_offset = 0;      // absolutni offset WAV souboru v .ithaca
    uint64_t entry_size = 0;        // delka WAV souboru v bajtech
    uint32_t pcm_data_offset = 0;   // relativni k entry_offset (prvni vzorek)
    uint16_t sample_format = 0;     // kSampleFmt*
    int64_t  frames = 0;
    float    rms_db = 0.f;          // autoritativni (poradi vrstev)
    uint32_t attack_end = 0;        // frame index v analyzovanem preload oknu
    uint32_t name_offset = kIthacaNoName;
};

// Parsuje 408B hlavicku. false = kratky buffer nebo spatny magic. Verzi,
// flagy a rozsahy NEKONTROLUJE — to dela openIthacaBank (rozlisene hlasky).
bool parseIthacaHeader(const uint8_t* buf, size_t n, IthacaHeader& out);

// Parsuje pole 64B zaznamu; n musi byt presne entry_count * 64.
bool parseIthacaIndex(const uint8_t* buf, size_t n, uint32_t entry_count,
                      std::vector<IthacaEntry>& out);

} // namespace ithaca
