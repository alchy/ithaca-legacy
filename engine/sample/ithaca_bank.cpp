// engine/sample/ithaca_bank.cpp — viz ithaca_bank.h.
#include "sample/ithaca_bank.h"

#include "util/sha256.h"

#include <algorithm>
#include <cstring>

namespace ithaca {

namespace {
// Precti celou sekci do bufferu; false pri prekroceni souboru/chybe cteni.
bool readSection(const IFileHandle& h, uint64_t off, uint64_t size,
                 std::vector<uint8_t>& out) {
    if (off + size > h.size() || off + size < off) return false;   // overflow guard
    out.resize((size_t)size);
    if (size == 0) return true;
    return h.readAt(off, out.data(), (size_t)size);
}

IthacaBankFile fail(std::string msg) {
    IthacaBankFile f;
    f.error = std::move(msg);
    return f;
}
} // namespace

IthacaBankFile openIthacaBank(const std::string& path) {
    auto handle = openFileHandle(path);
    if (!handle) return fail("nelze otevrit soubor");
    if (handle->size() < kIthacaHeaderSize) return fail("soubor kratsi nez hlavicka");

    uint8_t hdr_buf[kIthacaHeaderSize];
    if (!handle->readAt(0, hdr_buf, sizeof(hdr_buf)))
        return fail("nelze precist hlavicku");

    IthacaBankFile f;
    if (!parseIthacaHeader(hdr_buf, sizeof(hdr_buf), f.header))
        return fail("spatny magic (neni soundbank.ithaca)");
    const IthacaHeader& h = f.header;
    if (h.version != kIthacaVersion)
        return fail("nepodporovana verze formatu (" + std::to_string(h.version) + ")");
    if (h.flags != 0)
        return fail("flags != 0 (sifrovana/podepsana banka — vyzaduje novejsi verzi)");
    if (h.entry_count == 0) return fail("prazdny index (entry_count=0)");
    if (h.index_size != (uint64_t)h.entry_count * kIthacaEntrySize)
        return fail("index_size nesedi s entry_count");

    // Sekce + hash indexu (metadata+index+names v poradi souboru).
    std::vector<uint8_t> metadata, index_bytes, names;
    if (!readSection(*handle, h.metadata_offset, h.metadata_size, metadata) ||
        !readSection(*handle, h.index_offset,    h.index_size,    index_bytes) ||
        !readSection(*handle, h.names_offset,    h.names_size,    names))
        return fail("sekce mimo rozsah souboru");
    if (h.blob_offset + h.blob_size > handle->size() ||
        h.blob_offset + h.blob_size < h.blob_offset)
        return fail("blob mimo rozsah souboru");

    Sha256 sha;
    sha.update(metadata.data(),    metadata.size());
    sha.update(index_bytes.data(), index_bytes.size());
    sha.update(names.data(),       names.size());
    if (sha.finish() != h.sha256_index)
        return fail("hash indexu nesouhlasi (poskozeny soubor)");

    if (!parseIthacaIndex(index_bytes.data(), index_bytes.size(),
                          h.entry_count, f.entries))
        return fail("nelze parsovat index");

    // Per-zaznam validace rozsahu a hodnot.
    for (const IthacaEntry& e : f.entries) {
        if (e.midi > 127)                      return fail("zaznam: midi > 127");
        if (e.channels != 1 && e.channels != 2) return fail("zaznam: channels mimo 1/2");
        if (e.sample_format < kSampleFmtPcm16 || e.sample_format > kSampleFmtPcm32)
            return fail("zaznam: neznamy sample_format");
        if (e.frames <= 0 || e.sample_rate == 0) return fail("zaznam: frames/sample_rate");
        if (e.entry_offset < h.blob_offset ||
            e.entry_offset + e.entry_size > h.blob_offset + h.blob_size ||
            e.entry_offset + e.entry_size < e.entry_offset)
            return fail("zaznam: mimo rozsah blobu");
        const uint64_t bps = (e.sample_format == kSampleFmtPcm16) ? 2
                           : (e.sample_format == kSampleFmtPcm24) ? 3 : 4;
        // Division-form guard: frames * channels * bps by pro obri frames
        // preteklo u64 (untrusted vstup) — porovnavame delenim.
        const uint64_t frame_bytes = (uint64_t)e.channels * bps;
        if ((uint64_t)e.frames > (e.entry_size - (std::min)((uint64_t)e.pcm_data_offset, e.entry_size)) / frame_bytes)
            return fail("zaznam: PCM data presahuji entry_size");
    }

    f.handle = std::move(handle);
    f.ok = true;
    return f;
}

} // namespace ithaca
