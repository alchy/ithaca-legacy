// engine/sample/sample_store.cpp — viz sample_store.h.
#include "sample/sample_store.h"

#include "io/wav_reader.h"
#include "sample/bank_index.h"
#include "sample/sample_loader.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>

namespace ithaca {

Bank loadLegacyBank(const std::string& dir, log::Logger& logger,
                    int cache_budget_mb,
                    int midi_from, int midi_to,
                    int preload_ms) {
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
    // Faze 4: misto cele WAV cteme jen preload_head (a v dalsi fazi rezonancni
    // okno). Kratky sampl (vejde se do 2*preload) drzime cely v RAM jako
    // FullyLoaded; dlouhy oznacime Streamed (zbytek dotece pres ring buffer).
    for (const auto& entry : scan.files) {
        const ParsedName& p = entry.parsed;
        if (p.midi < 0 || p.midi > 127) continue;
        if (p.midi < midi_from || p.midi > midi_to) continue;   // mimo pozadovany rozsah

        WavInfo info = peekWavInfo(entry.full_path);
        if (!info.valid) {
            logger.log("bank", log::Severity::Warning,
                       "Nelze precist hlavicku: %s", p.filename.c_str());
            continue;
        }

        MicLayer mic;
        mic.mic_name         = "stereo";
        mic.file.path        = entry.full_path;
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
        WavData head = readWavRange(entry.full_path, 0, mic.head_frames);
        if (!head.valid || head.frames < mic.head_frames) {
            logger.log("bank", log::Severity::Warning,
                       "Nelze nacist preload hlavicky: %s", p.filename.c_str());
            continue;
        }
        mic.preload_head = std::move(head.samples);

        // preload_resonance ve fazi 4 zatim prazdny (rezonance je faze 5).

        // peak_rms_db / attack_end_frame meri z preload_head — typicky u piana
        // pokryva attack (peak RMS lezi v prvnich ~10-50 ms). U dlouheho samplu
        // by sice byl idealne meren z resonance regionu (faze 5), ale pro
        // velocity krivku staci preload.
        float rms = measurePeakRmsDb(mic.preload_head.data(),
                                     mic.head_frames, info.sample_rate);
        int   ae  = findAttackEnd  (mic.preload_head.data(),
                                     mic.head_frames, info.sample_rate);

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
        // total_frames: pocitej REZIDENTNI v RAM (head + resonance).
        bank.total_frames += (size_t)m.head_frames + (size_t)m.resonance_frames;
        bank.total_bytes  += (m.preload_head.size() + m.preload_resonance.size())
                             * sizeof(float);
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
