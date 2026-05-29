#pragma once
// engine/sample/bank_index.h
// --------------------------
// Objevi obsah banky: projde adresar, rozparsuje nazvy souboru a detekuje
// format (legacy vs extended). Parsovaci funkce jsou ciste (bez I/O), aby
// sly snadno testovat. scanBank() dela adresarovy sken.

#include "sample/sample_types.h"

#include <string>
#include <vector>

namespace ithaca {

// Vysledek parsovani jednoho nazvu souboru.
struct ParsedName {
    bool        ok   = false;
    int         midi = -1;
    // legacy:
    int         vel    = -1;     // 0-7; -1 kdyz neni legacy
    int         sr_tag = 0;      // 48/44/96 z fSS
    // extended (faze 7 — zde jen naplnime, build neresi):
    std::string mic;             // "front"/"soundboard"
    std::string hash;            // parovaci klic (posledni token pred .wav)
    std::string filename;        // puvodni nazev (pro pozdejsi otevreni)
};

// Jeden nalezeny soubor: parsovana metadata + plna cesta.
struct BankFileEntry {
    ParsedName  parsed;
    std::string full_path;
};

// Vysledek skenu cele banky.
struct BankScan {
    BankFormat                 format = BankFormat::Unknown;
    std::vector<BankFileEntry> files;     // jen soubory odpovidajiciho formatu
    int                        skipped = 0;  // nerozpoznane soubory
};

// -- Ciste parsovaci funkce (testovatelne bez disku) --
ParsedName parseLegacyName(const std::string& filename);
ParsedName parseExtendedName(const std::string& filename);   // faze 7 — minimalni
BankFormat detectFormatFromName(const std::string& filename);

// -- Adresarovy sken --
// Projde `dir`, urci format (podle vetsiny rozpoznanych souboru), vrati
// parsovane zaznamy. Pri prazdnem/neexistujicim adresari je format Unknown.
BankScan scanBank(const std::string& dir);

} // namespace ithaca
