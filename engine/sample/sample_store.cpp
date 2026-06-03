// engine/sample/sample_store.cpp — viz sample_store.h.
#include "sample/sample_store.h"

#include "io/wav_reader.h"
#include "resonance/resonance_layer_select.h"
#include "sample/bank_index.h"
#include "sample/sample_loader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>

namespace ithaca {

namespace {

// Nacte JEDEN WAV soubor jako VelocitySlot a vlozi ho do bank.notes[midi].
// Sdileno fixed-velocity i dynamic-velocity loaderem — oba se lisi jen v tom,
// jak objevi dvojice (midi, cesta); samotna ingesce (peek → MicLayer → preload
// head → peak RMS → attack end → resonance okno → slot + statistiky) je spolecna.
// Vraci true pri uspechu. `filename` je jen pro logy.
bool ingestSampleFile(Bank& bank, int midi, const std::string& full_path,
                      const std::string& filename, log::Logger& logger,
                      int preload_ms, int resonance_window_ms) {
    (void)resonance_window_ms;   // ingest uz neplni preload_resonance — dela to buildResonanceCache
    WavInfo info = peekWavInfo(full_path);
    if (!info.valid) {
        logger.log("bank", log::Severity::Warning,
                   "Nelze precist hlavicku: %s", filename.c_str());
        return false;
    }

    MicLayer mic;
    mic.mic_name         = "stereo";
    mic.file.path        = full_path;
    mic.file.frames      = info.frames;
    mic.file.sample_rate = info.sample_rate;
    mic.file.valid       = true;

    // "Kratky" sampl = vejde se do 2 * preload_ms. Drzime cely v RAM (head).
    const int preload_frames =
        (int)((int64_t)preload_ms * info.sample_rate / 1000);
    const int short_threshold_frames = preload_frames * 2;
    if (info.frames <= short_threshold_frames) {
        mic.mode        = MicLayerMode::FullyLoaded;
        mic.head_frames = info.frames;
    } else {
        mic.mode        = MicLayerMode::Streamed;
        mic.head_frames = preload_frames;
    }

    // Nacti preload_head [0 .. head_frames).
    WavData head = readWavRange(full_path, 0, mic.head_frames);
    if (!head.valid) {
        logger.log("bank", log::Severity::Warning,
                   "Nelze nacist preload (read failed): %s", filename.c_str());
        return false;
    }
    if (head.frames < mic.head_frames) {
        // Soubor je oriznuty — sampl si nechame s tim, co jsme dostali, jen
        // upravime head_frames. FullyLoaded se zaroven prepocita, kdyz uz mame
        // mene nez puvodne planovany head.
        logger.log("bank", log::Severity::Warning,
                   "Oriznuty soubor — pouzivam %d/%d frames: %s",
                   head.frames, mic.head_frames, filename.c_str());
        mic.head_frames = head.frames;
        if (mic.head_frames >= mic.file.frames) mic.mode = MicLayerMode::FullyLoaded;
    }
    mic.preload_head = std::move(head.samples);

    // INVARIANT: u piano-class samplu je peak RMS vzdy v attack fazi, ktera se
    // vzdy vejde do preload_head. Pri rozsireni na non-piano zvuky (looped pads
    // atd.) bude treba merit az z preload_resonance regionu (faze 5+).
    float rms = measurePeakRmsDb(mic.preload_head.data(),
                                 mic.head_frames, info.sample_rate);
    int   ae  = findAttackEnd  (mic.preload_head.data(),
                                 mic.head_frames, info.sample_rate);

    // Resonance buffer plni az buildResonanceCache (po sortBankSlotsByRms), a to
    // JEN pro per-notu cilovou velocity vrstvu. Ingest jen ulozi start frame.
    if (mic.mode == MicLayerMode::Streamed) {
        mic.resonance_start_frame = ae;          // = attack_end_frame (buffer plni buildResonanceCache)
    }

    SampleAsset asset;
    asset.peak_rms_db      = rms;
    asset.attack_end_frame = ae;
    asset.mics.push_back(std::move(mic));

    VelocitySlot slot;
    slot.rms_db = rms;
    slot.variants.push_back(std::move(asset));

    bank.notes[midi].slots.push_back(std::move(slot));
    bank.notes[midi].recorded = true;

    const MicLayer& m = bank.notes[midi].slots.back().variants[0].mics[0];
    // resident_frames: pocitej REZIDENTNI v RAM (head + resonance).
    bank.resident_frames += (size_t)m.head_frames + (size_t)m.resonance_frames;
    bank.total_bytes  += (m.preload_head.size() + m.preload_resonance.size())
                         * sizeof(float);
    bank.loaded_samples++;
    return true;
}

// Serad sloty kazde noty VZESTUPNE podle peak RMS (nejtissi → nejhlasitejsi).
// Toto je autoritativni razeni velocity vrstev pro OBA formaty — pripadny
// velocity tag z nazvu (fixed-velocity) je jen poradni.
void sortBankSlotsByRms(Bank& bank) {
    for (int n = 0; n < 128; ++n) {
        auto& slots = bank.notes[n].slots;
        std::sort(slots.begin(), slots.end(),
                  [](const VelocitySlot& a, const VelocitySlot& b) {
                      return a.rms_db < b.rms_db;
                  });
    }
}

// Spolecne zaverecne logovani + cache-rozpocet varovani.
void logBankSummary(const Bank& bank, log::Logger& logger, int cache_budget_mb) {
    logger.log("bank", log::Severity::Info,
               "Nacteno %d samplu, %zu frames, ~%zu MB RAM",
               bank.loaded_samples, bank.resident_frames,
               bank.total_bytes / (1024 * 1024));
    if (cache_budget_mb > 0) {
        size_t budget_bytes = (size_t)cache_budget_mb * 1024 * 1024;
        if (bank.total_bytes > budget_bytes)
            logger.log("bank", log::Severity::Warning,
                       "Banka presahuje cache rozpocet (%d MB) — faze 4 doplni streaming",
                       cache_budget_mb);
    }
}

} // namespace

Bank loadBank(const std::string& dir, log::Logger& logger,
              int cache_budget_mb,
              int midi_from, int midi_to,
              int preload_ms,
              int resonance_window_ms) {
    Bank bank;
    bank.path = dir;
    bank.name = std::filesystem::path(dir).filename().string();

    BankScan scan = scanBank(dir);   // detekuje fixed-velocity / dynamic-velocity
    bank.format = scan.format;

    if (scan.format == BankFormat::Unknown) {
        logger.log("bank", log::Severity::Warning,
                   "Banka '%s': zadne rozpoznane samply (%d preskoceno)",
                   bank.name.c_str(), scan.skipped);
        return bank;
    }
    if (scan.format == BankFormat::Extended) {
        logger.log("bank", log::Severity::Warning,
                   "Banka '%s': extended format zatim nepodporovan (faze 7)",
                   bank.name.c_str());
        return bank;
    }

    logger.log("bank", log::Severity::Info,
               "Banka '%s': %s, %zu souboru",
               bank.name.c_str(), bankFormatName(scan.format), scan.files.size());

    // Discovery se lisi dle formatu (ploche soubory vs mNNN/ slozky), ale
    // scanBank uz vratil jednotny seznam (midi, cesta) — ingesce je spolecna.
    // RAM budget (OOM guard): kdyz preload heads prekroci budget, PRERUSIME
    // nacitani (neuplna banka + error) misto pádu na bad_alloc. Budget kryje
    // hlavne preload heads; RAM cache rezonance se staví pozdeji a chrani ji
    // bad_alloc try/catch v Engine::loadBank.
    const size_t budget_bytes = cache_budget_mb > 0
                              ? (size_t)cache_budget_mb * 1024 * 1024 : 0;
    for (const auto& entry : scan.files) {
        const ParsedName& p = entry.parsed;
        if (p.midi < 0 || p.midi > 127) continue;
        if (p.midi < midi_from || p.midi > midi_to) continue;   // mimo pozadovany rozsah
        if (budget_bytes && bank.total_bytes >= budget_bytes) {
            logger.log("bank", log::Severity::Error,
                       "Banka '%s': RAM budget %d MB prekrocen (~%zu MB) — nacitani "
                       "PRERUSENO, banka NEUPLNA. Sniz banku / preload_ms / "
                       "resonance_window_ms, nebo zvys cache_budget_mb.",
                       bank.name.c_str(), cache_budget_mb,
                       bank.total_bytes / (1024 * 1024));
            break;
        }
        ingestSampleFile(bank, p.midi, entry.full_path, p.filename, logger,
                         preload_ms, resonance_window_ms);
    }

    sortBankSlotsByRms(bank);
    logBankSummary(bank, logger, cache_budget_mb);
    return bank;
}

std::array<bool, 128> buildResonanceCache(Bank& bank, float target_db,
                                          int window_ms, log::Logger& logger) {
    std::array<bool, 128> ready{};   // vse false
    for (int n = 0; n < 128; ++n) {
        auto& note = bank.notes[(size_t)n];
        if (!note.recorded || note.slots.empty()) continue;
        const int si = nearestSlotByRms(note, target_db);
        if (si < 0) continue;
        for (int s = 0; s < (int)note.slots.size(); ++s) {
            auto& vslot = note.slots[(size_t)s];
            if (vslot.variants.empty() || vslot.variants[0].mics.empty()) continue;
            MicLayer& m = vslot.variants[0].mics[0];
            if (s != si) {
                m.preload_resonance.clear();
                m.preload_resonance.shrink_to_fit();
                m.resonance_frames = 0;
                continue;
            }
            // cilovy slot
            if (m.mode == MicLayerMode::FullyLoaded) { ready[(size_t)n] = true; continue; }
            int rwin  = (int)((int64_t)window_ms * m.file.sample_rate / 1000);
            int avail = m.file.frames - m.resonance_start_frame;
            if (avail < 0) avail = 0;
            if (rwin > avail) rwin = avail;
            if (rwin <= 0) { m.preload_resonance.clear(); m.resonance_frames = 0; ready[(size_t)n] = true; continue; }
            WavData rd = readWavRange(m.file.path, m.resonance_start_frame, rwin);
            if (rd.valid && rd.frames > 0) {
                m.resonance_frames  = rd.frames;
                m.preload_resonance = std::move(rd.samples);
                ready[(size_t)n] = true;
            } else {
                logger.log("bank", log::Severity::Warning,
                           "buildResonanceCache: read failed note %d", n);
            }
        }
    }
    return ready;
}

} // namespace ithaca
