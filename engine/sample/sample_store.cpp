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
                    int preload_ms,
                    int resonance_window_ms) {
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
        if (!head.valid) {
            logger.log("bank", log::Severity::Warning,
                       "Nelze nacist preload (read failed): %s", p.filename.c_str());
            continue;
        }
        if (head.frames < mic.head_frames) {
            // Soubor je oriznuty — sampl si nechame s tim, co jsme dostali, jen
            // upravime head_frames. FullyLoaded se zaroven prepocita, kdyz uz
            // mame mene nez puvodne planovany head (Streamed sampl by tak
            // streamoval od kratsi pozice — nicotny okrajovy pripad).
            logger.log("bank", log::Severity::Warning,
                       "Oriznuty soubor — pouzivam %d/%d frames: %s",
                       head.frames, mic.head_frames, p.filename.c_str());
            mic.head_frames = head.frames;
            if (mic.head_frames >= mic.file.frames) mic.mode = MicLayerMode::FullyLoaded;
        }
        mic.preload_head = std::move(head.samples);

        // INVARIANT: u piano-class samplu je peak RMS vzdy v attack fazi, ktera
        // se vzdy vejde do preload_head. Pri rozsireni na non-piano zvuky (looped
        // pads atd.) bude treba merit az z preload_resonance regionu (faze 5+).
        float rms = measurePeakRmsDb(mic.preload_head.data(),
                                     mic.head_frames, info.sample_rate);
        int   ae  = findAttackEnd  (mic.preload_head.data(),
                                     mic.head_frames, info.sample_rate);

        // Nacti preload_resonance pro Streamed mic (faze 5: zdroj rezonancnich
        // hlasu — preskoceny attack, drzeny sustain). Pro FullyLoaded je cely
        // sampl v preload_head, takze separatni resonance buffer netreba.
        if (mic.mode == MicLayerMode::Streamed && resonance_window_ms > 0) {
            mic.resonance_start_frame = ae;          // = attack_end_frame
            int rwin = (int)((int64_t)resonance_window_ms * info.sample_rate / 1000);
            // Orizni window na to, co soubor jeste obsahuje.
            int avail = info.frames - mic.resonance_start_frame;
            if (avail < 0) avail = 0;
            if (rwin > avail) rwin = avail;
            if (rwin > 0) {
                WavData rd = readWavRange(entry.full_path,
                                          mic.resonance_start_frame, rwin);
                if (rd.valid && rd.frames > 0) {
                    mic.resonance_frames  = rd.frames;
                    mic.preload_resonance = std::move(rd.samples);
                } else {
                    logger.log("bank", log::Severity::Warning,
                               "Nelze nacist preload_resonance: %s",
                               p.filename.c_str());
                }
            }
        }

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
        // resident_frames: pocitej REZIDENTNI v RAM (head + resonance).
        bank.resident_frames += (size_t)m.head_frames + (size_t)m.resonance_frames;
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
               bank.loaded_samples, bank.resident_frames,
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
