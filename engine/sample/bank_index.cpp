// engine/sample/bank_index.cpp — viz bank_index.h.
#include "sample/bank_index.h"

#include <cctype>
#include <exception>
#include <filesystem>
#include <regex>

namespace ithaca {

namespace {
// Fixed-velocity: mNNN-velV-fSS.wav (3 cislice noty, 1 cislice vel, 2-3 SR).
// SR tag je 44/48/96 → max 3 cislice; omezeni drzi std::stoi v bezpecnem rozsahu
// (neomezene \d+ mohlo vyhodit std::out_of_range na poskozenem nazvu).
const std::regex& fixedVelRe() {
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
// Dynamic-velocity: nazev podslozky "m<MIDI>" (1-3 cislice, 0-127). Vrati MIDI
// nebo -1 kdyz nazev neodpovida. Soubory uvnitr jsou nepruhledne hashe.
int parseNoteFolder(const std::string& name) {
    static const std::regex re(R"(m(\d{1,3}))", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(name, m, re)) return -1;
    try {
        int midi = std::stoi(m[1].str());
        return (midi >= 0 && midi <= 127) ? midi : -1;
    } catch (const std::exception&) { return -1; }
}
// Case-insensitive ".wav" pripona.
bool isWavFile(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".wav";
}
} // namespace

ParsedName parseFixedVelocityName(const std::string& filename) {
    ParsedName p;
    std::smatch m;
    if (!std::regex_match(filename, m, fixedVelRe())) return p;
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
    if (parseFixedVelocityName(filename).ok)   return BankFormat::FixedVelocity;
    if (parseExtendedName(filename).ok) return BankFormat::Extended;
    return BankFormat::Unknown;
}

BankScan scanBank(const std::string& dir) {
    BankScan scan;
    namespace fs = std::filesystem;
    std::error_code ec;

    // 1) Dynamic-velocity detekce (ma prednost): obsahuje adresar podslozky
    //    "m<MIDI>"? Pak je to novy folder format — kazda slozka = jedna nota,
    //    uvnitr hashovane WAV (libovolny pocet velocity vrstev). Discriminuje
    //    jednoznacne od plochych formatu (ty zadne m### podslozky nemaji).
    {
        std::vector<std::pair<int, std::string>> note_dirs;   // (midi, cesta)
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!e.is_directory()) continue;
            int midi = parseNoteFolder(e.path().filename().string());
            if (midi >= 0) note_dirs.emplace_back(midi, e.path().string());
        }
        if (!note_dirs.empty()) {
            scan.format = BankFormat::DynamicVelocity;
            for (const auto& [midi, path] : note_dirs) {
                std::error_code ec2;
                for (const auto& w : fs::directory_iterator(path, ec2)) {
                    if (ec2) break;
                    if (!w.is_regular_file() || !isWavFile(w.path())) { scan.skipped++; continue; }
                    ParsedName pn;            // dynamic: jen MIDI ze slozky, vel/sr nepouzity
                    pn.ok       = true;
                    pn.midi     = midi;
                    pn.filename = w.path().filename().string();
                    scan.files.push_back({pn, w.path().string()});
                }
            }
            return scan;
        }
    }

    // 2) Ploche soubory (fixed-velocity / extended) — format urci vetsina.
    int fixed_count = 0, extended_count = 0;
    std::vector<std::pair<ParsedName, std::string>> fixed_files, extended_files;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();

        ParsedName lg = parseFixedVelocityName(fname);
        if (lg.ok) {
            fixed_files.emplace_back(lg, entry.path().string());
            fixed_count++;
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
    if (fixed_count == 0 && extended_count == 0) {
        scan.format = BankFormat::Unknown;
        return scan;
    }
    if (fixed_count >= extended_count) {
        scan.format = BankFormat::FixedVelocity;
        for (auto& f : fixed_files)
            scan.files.push_back({f.first, f.second});
    } else {
        scan.format = BankFormat::Extended;
        for (auto& f : extended_files)
            scan.files.push_back({f.first, f.second});
    }
    return scan;
}

} // namespace ithaca
