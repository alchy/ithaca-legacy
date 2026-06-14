#pragma once
// engine/io/file_handle.h
// Bezstavove pozicovane cteni (pread) — zadny sdileny seek kurzor, takze
// paralelni loader workery i streaming worker ctou TYZ handle bez zamku.
// Pouziva pakovana banka (.ithaca blob); bezne WAV cesty ctou dal pres
// wav_reader (fopen per call).

#include <cstdint>
#include <memory>
#include <string>

namespace ithaca {

struct IFileHandle {
    virtual ~IFileHandle() = default;
    // Precte PRESNE n bajtu z absolutniho offsetu. false = kratke cteni nebo
    // chyba (volajici zna velikosti z indexu — kratke cteni je vzdy chyba).
    virtual bool readAt(uint64_t off, void* buf, size_t n) const = 0;
    virtual uint64_t size() const = 0;
};

// Otevre soubor pro pread (sdileny mezi vlakny). nullptr pri chybe.
std::shared_ptr<IFileHandle> openFileHandle(const std::string& path);

} // namespace ithaca
