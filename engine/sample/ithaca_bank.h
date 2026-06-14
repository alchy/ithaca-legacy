#pragma once
// engine/sample/ithaca_bank.h
// Otevreni a validace pakovane banky soundbank.ithaca: hlavicka, SHA-256
// indexovych sekci (metadata+index+names — VZDY pri nacteni; blob hash
// overuje jen bake --verify), rozsahy zaznamu. Vraci zaznamy + otevreny
// handle pro readSampleRange (preload + streaming).
// Prekryvy sekci se NEvaliduji — hash je integrita, ne autenticita (v2 podpis).

#include "io/file_handle.h"
#include "sample/ithaca_format.h"

#include <string>
#include <vector>

namespace ithaca {

struct IthacaBankFile {
    bool        ok = false;
    std::string error;       // duvod pri ok=false (caller loguje)
    IthacaHeader header;
    std::vector<IthacaEntry> entries;
    std::shared_ptr<IFileHandle> handle;
};

// Otevre <path> (plna cesta k soundbank.ithaca). Pri jakekoliv chybe ok=false
// + error; volajici zaloguje ERROR a vrati prazdnou banku.
IthacaBankFile openIthacaBank(const std::string& path);

} // namespace ithaca
