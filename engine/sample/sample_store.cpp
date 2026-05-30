// engine/sample/sample_store.cpp — viz sample_store.h.
#include "sample/sample_store.h"

#include "io/wav_reader.h"
#include "sample/bank_index.h"
#include "sample/sample_loader.h"

#include <algorithm>
#include <filesystem>

namespace ithaca {

Bank loadLegacyBank(const std::string& dir, log::Logger& logger,
                    int cache_budget_mb,
                    int midi_from, int midi_to) {
    Bank bank;
    bank.path = dir;
    bank.name = std::filesystem::path(dir).filename().string();

    BankScan scan = scanBank(dir);
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
               "Banka '%s': legacy, %zu souboru",
               bank.name.c_str(), scan.files.size());

    // Nacti kazdy soubor, zmer, vloz jako jeden VelocitySlot do prislusne noty.
    for (const auto& entry : scan.files) {
        const ParsedName& p = entry.parsed;
        if (p.midi < 0 || p.midi > 127) continue;
        if (p.midi < midi_from || p.midi > midi_to) continue;   // mimo pozadovany rozsah

        WavData w = readWav(entry.full_path);
        if (!w.valid) {
            logger.log("bank", log::Severity::Warning,
                       "Nelze nacist: %s", p.filename.c_str());
            continue;
        }

        float rms = measurePeakRmsDb(w.samples.data(), w.frames, w.sample_rate);
        int   ae  = findAttackEnd(w.samples.data(), w.frames, w.sample_rate);

        MicLayer mic;
        mic.mic_name    = "stereo";
        mic.frames      = w.frames;
        mic.sample_rate = w.sample_rate;
        mic.data        = std::move(w.samples);

        SampleAsset asset;
        asset.peak_rms_db      = rms;
        asset.attack_end_frame = ae;
        asset.mics.push_back(std::move(mic));

        VelocitySlot slot;
        slot.rms_db = rms;
        slot.variants.push_back(std::move(asset));

        bank.notes[p.midi].slots.push_back(std::move(slot));
        bank.notes[p.midi].recorded = true;

        const MicLayer& m = bank.notes[p.midi].slots.back().variants[0].mics[0];
        bank.total_frames += (size_t)m.frames;
        bank.total_bytes  += m.data.size() * sizeof(float);
        bank.loaded_samples++;
    }

    // Serad sloty kazde noty vzestupne podle RMS (nejtissi → nejhlasitejsi).
    // Legacy vel tag je obvykle uz serazeny, ale RMS je autoritativni.
    for (int n = 0; n < 128; ++n) {
        auto& slots = bank.notes[n].slots;
        std::sort(slots.begin(), slots.end(),
                  [](const VelocitySlot& a, const VelocitySlot& b) {
                      return a.rms_db < b.rms_db;
                  });
    }

    logger.log("bank", log::Severity::Info,
               "Nacteno %d samplu, %zu frames, ~%zu MB RAM",
               bank.loaded_samples, bank.total_frames,
               bank.total_bytes / (1024 * 1024));

    if (cache_budget_mb > 0) {
        size_t budget_bytes = (size_t)cache_budget_mb * 1024 * 1024;
        if (bank.total_bytes > budget_bytes)
            logger.log("bank", log::Severity::Warning,
                       "Banka presahuje cache rozpocet (%d MB) — faze 4 doplni streaming",
                       cache_budget_mb);
    }
    return bank;
}

} // namespace ithaca
