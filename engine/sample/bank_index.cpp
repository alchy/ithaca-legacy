// engine/sample/bank_index.cpp — viz bank_index.h.
#include "sample/bank_index.h"

#include <exception>
#include <filesystem>
#include <regex>

namespace ithaca {

namespace {
// Legacy: mNNN-velV-fSS.wav  (3 cislice noty, 1 cislice vel, 2-3 cislice SR).
// SR tag je 44/48/96 → max 3 cislice; omezeni drzi std::stoi v bezpecnem rozsahu
// (neomezene \d+ mohlo vyhodit std::out_of_range na poskozenem nazvu).
const std::regex& legacyRe() {
    static const std::regex re(R"(m(\d{3})-vel(\d)-f(\d{2,3})\.wav)",
                               std::regex::icase);
    return re;
}
// Extended: mNN-MIC-HASH.wav  (MIC = pismena, HASH = posledni token pred .wav).
// MIDI nota max 3 cislice (0-127) → omezeni drzi std::stoi v bezpecnem rozsahu.
const std::regex& extendedRe() {
    static const std::regex re(R"(m(\d{1,3})-([a-z]+)-([A-Za-z0-9]+)\.wav)",
                               std::regex::icase);
    return re;
}
} // namespace

ParsedName parseLegacyName(const std::string& filename) {
    ParsedName p;
    std::smatch m;
    if (!std::regex_match(filename, m, legacyRe())) return p;
    // Belt-and-suspenders: i s omezenymi regexy obalime stoi, aby se zadna
    // vyjimka nedostala ven a neshodila scanBank na poskozenem nazvu.
    try {
        p.midi   = std::stoi(m[1].str());
        p.vel    = std::stoi(m[2].str());
        p.sr_tag = std::stoi(m[3].str());
    } catch (const std::exception&) {
        return ParsedName{};   // poskozeny nazev → neparsovatelny
    }
    p.ok       = true;
    p.filename = filename;
    return p;
}

ParsedName parseExtendedName(const std::string& filename) {
    ParsedName p;
    std::smatch m;
    if (!std::regex_match(filename, m, extendedRe())) return p;
    // Pozn.: legacy je podmnozina tohoto vzoru jen kdyby MIC byl "vel" — to ale
    // detectFormatFromName resi prioritou legacy, takze sem se legacy nedostane.
    // Belt-and-suspenders: obalime stoi, aby vyjimka nikdy neunikla.
    try {
        p.midi = std::stoi(m[1].str());
    } catch (const std::exception&) {
        return ParsedName{};   // poskozeny nazev → neparsovatelny
    }
    p.ok       = true;
    p.mic      = m[2].str();
    p.hash     = m[3].str();
    p.filename = filename;
    return p;
}

BankFormat detectFormatFromName(const std::string& filename) {
    if (parseLegacyName(filename).ok)   return BankFormat::Legacy;
    if (parseExtendedName(filename).ok) return BankFormat::Extended;
    return BankFormat::Unknown;
}

BankScan scanBank(const std::string& dir) {
    BankScan scan;
    namespace fs = std::filesystem;
    std::error_code ec;

    int legacy_count = 0, extended_count = 0;
    std::vector<std::pair<ParsedName, std::string>> legacy_files, extended_files;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();

        ParsedName lg = parseLegacyName(fname);
        if (lg.ok) {
            legacy_files.emplace_back(lg, entry.path().string());
            legacy_count++;
            continue;
        }
        ParsedName ex = parseExtendedName(fname);
        if (ex.ok) {
            extended_files.emplace_back(ex, entry.path().string());
            extended_count++;
            continue;
        }
        scan.skipped++;
    }

    // Format urci vetsina rozpoznanych souboru (banka je homogenni).
    if (legacy_count == 0 && extended_count == 0) {
        scan.format = BankFormat::Unknown;
        return scan;
    }
    if (legacy_count >= extended_count) {
        scan.format = BankFormat::Legacy;
        for (auto& f : legacy_files)
            scan.files.push_back({f.first, f.second});
    } else {
        scan.format = BankFormat::Extended;
        for (auto& f : extended_files)
            scan.files.push_back({f.first, f.second});
    }
    return scan;
}

} // namespace ithaca
