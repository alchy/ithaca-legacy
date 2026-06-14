// engine/sample/sample_store.cpp — viz sample_store.h.
#include "sample/sample_store.h"

#include "io/sample_read.h"
#include "io/wav_reader.h"
#include "resonance/resonance_layer_select.h"
#include "sample/bank_index.h"
#include "sample/ithaca_bank.h"
#include "sample/sample_loader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

namespace ithaca {

namespace {

// Jednoduchy fork-join: rozdej indexy 0..n-1 mezi az n_workers vlaken
// (atomic counter; kazdy worker si bere dalsi volny index). Pouziva paralelni
// ingest a stavba rezonancni cache — per-index prace je nezavisla.
template <typename Fn>
void parallelFor(int n, int n_workers, Fn&& fn) {
    if (n <= 0) return;
    const int nw = (std::min)(n_workers, n);
    if (nw <= 1) { for (int i = 0; i < n; ++i) fn(i); return; }
    std::atomic<int> next{0};
    std::vector<std::thread> ts;
    ts.reserve((size_t)nw);
    for (int t = 0; t < nw; ++t)
        ts.emplace_back([&] {
            for (;;) {
                const int i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) break;
                fn(i);
            }
        });
    for (auto& t : ts) t.join();
}

int loaderWorkers() {
    int hc = (int)std::thread::hardware_concurrency();
    if (hc <= 0) hc = 4;
    return std::clamp(hc / 2, 2, 8);
}

// Vysledek pripravy JEDNOHO WAV souboru (paralelni faze — bez zapisu do Bank).
struct PreparedSample {
    int          midi = -1;
    std::string  filename;     // jen pro logy
    VelocitySlot slot;
    size_t       bytes = 0;    // preload_head bajty (budget/statistiky)
    bool         ok = false;
};

// Cteni + analyza JEDNOHO souboru (peek → MicLayer → preload head → peak RMS
// → attack end). Sdileno fixed- i dynamic-velocity loaderem; bezi PARALELNE
// pres worker pool (logger ma vlastni mutex). budget_bytes/approx_bytes:
// hruby OOM guard paralelniho cteni — kdyz rozectene heads presahnou 2x
// budget, soubor se uz NEcte (autoritativni presna kontrola je v commitu;
// prekroceni je tak jako tak ERROR + neuplna banka, presna mnozina nactenych
// souboru v chybovem pripade neni garantovana).
PreparedSample prepareSampleFile(int midi, const std::string& full_path,
                                 const std::string& filename,
                                 log::Logger& logger, int preload_ms,
                                 size_t budget_bytes,
                                 std::atomic<size_t>& approx_bytes) {
    PreparedSample out;
    out.midi = midi;
    out.filename = filename;
    WavInfo info = peekWavInfo(full_path);
    if (!info.valid) {
        logger.log("bank", log::Severity::Warning,
                   "Nelze precist hlavicku: %s", filename.c_str());
        return out;
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
    if (info.frames <= preload_frames * 2) {
        mic.mode        = MicLayerMode::FullyLoaded;
        mic.head_frames = info.frames;
    } else {
        mic.mode        = MicLayerMode::Streamed;
        mic.head_frames = preload_frames;
    }

    const size_t est = (size_t)mic.head_frames * 2 * sizeof(float);
    if (budget_bytes &&
        approx_bytes.fetch_add(est, std::memory_order_relaxed) + est
            > budget_bytes * 2) {
        return out;   // OOM guard paralelniho cteni (commit by stejne odmitl)
    }

    // Nacti preload_head [0 .. head_frames). Pres dispatcher: blob==null u WAV
    // → deleguje na readWavRange (shodne chovani; jeden cteci kanal pro oba formaty).
    WavData head = readSampleRange(mic.file, 0, mic.head_frames);
    if (!head.valid) {
        logger.log("bank", log::Severity::Warning,
                   "Nelze nacist preload (read failed): %s", filename.c_str());
        return out;
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

    out.bytes = mic.preload_head.size() * sizeof(float);

    SampleAsset asset;
    asset.peak_rms_db      = rms;
    asset.attack_end_frame = ae;
    asset.mics.push_back(std::move(mic));

    out.slot.rms_db = rms;
    out.slot.variants.push_back(std::move(asset));
    out.ok = true;
    return out;
}

// Vlozeni pripraveneho samplu do banky + statistiky. POUZE z merge vlakna,
// ve scan poradi (deterministicka banka + presna budget kontrola).
void commitSample(Bank& bank, PreparedSample&& p) {
    const MicLayer& m = p.slot.variants[0].mics[0];
    // resident_frames: pocitej REZIDENTNI v RAM (head + resonance).
    bank.resident_frames += (size_t)m.head_frames + (size_t)m.resonance_frames;
    bank.total_bytes     += p.bytes;
    bank.loaded_samples++;
    bank.notes[p.midi].slots.push_back(std::move(p.slot));
    bank.notes[p.midi].recorded = true;
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

// Slouceny merge pripravenych samplu do banky (sdili adresarova i pakovana
// vetev): jednovlaknove ve scan poradi → deterministicka banka + presny RAM
// budget. Pri prekroceni budgetu PRERUSI (ERROR, banka NEUPLNA, truncated).
// Po merge zapise presny bytes_loaded. Razeni slotu (sortBankSlotsByRms) si
// resi adresarova vetev sama — packed ma poradi z indexu autoritativni.
void mergePrepared(Bank& bank, std::vector<PreparedSample>& prepared,
                   size_t budget_bytes, int cache_budget_mb,
                   log::Logger& logger, BankLoadProgress* progress) {
    for (auto& p : prepared) {
        if (!p.ok) continue;
        if (budget_bytes && bank.total_bytes >= budget_bytes) {
            logger.log("bank", log::Severity::Error,
                       "Banka '%s': RAM budget %d MB prekrocen (~%zu MB) — nacitani "
                       "PRERUSENO, banka NEUPLNA. Sniz banku / preload_ms / "
                       "resonance_window_ms, nebo zvys cache_budget_mb.",
                       bank.name.c_str(), cache_budget_mb,
                       bank.total_bytes / (1024 * 1024));
            if (progress) progress->truncated.store(true, std::memory_order_relaxed);
            break;
        }
        commitSample(bank, std::move(p));
    }
    if (progress)
        progress->bytes_loaded.store(bank.total_bytes, std::memory_order_relaxed);
}

// Priprava JEDNOHO zaznamu pakovane banky: ZADNA analyza (RMS/attack jsou
// baked v indexu — autoritativni), jen preload head pres readSampleRange.
// Mode FullyLoaded/Streamed zustava runtime rozhodnuti (baked frames vs
// aktualni preload_ms).
PreparedSample preparePackedSample(const IthacaEntry& e,
                                   const std::shared_ptr<IFileHandle>& blob,
                                   const std::string& ithaca_path,
                                   log::Logger& logger, int preload_ms,
                                   size_t budget_bytes,
                                   std::atomic<size_t>& approx_bytes) {
    PreparedSample out;
    out.midi     = (int)e.midi;
    out.filename = ithaca_path;   // puvodni jmeno nedrzime (jen pro logy)

    MicLayer mic;
    mic.mic_name           = "stereo";
    mic.file.path          = ithaca_path;
    mic.file.frames        = (int)(std::min)(e.frames, (int64_t)INT32_MAX);
    mic.file.sample_rate   = (int)e.sample_rate;
    mic.file.valid         = true;
    mic.file.blob          = blob;
    mic.file.pcm_offset    = e.entry_offset + e.pcm_data_offset;
    mic.file.channels      = e.channels;
    mic.file.sample_format = e.sample_format;

    const int preload_frames =
        (int)((int64_t)preload_ms * mic.file.sample_rate / 1000);
    if (mic.file.frames <= preload_frames * 2) {
        mic.mode        = MicLayerMode::FullyLoaded;
        mic.head_frames = mic.file.frames;
    } else {
        mic.mode        = MicLayerMode::Streamed;
        mic.head_frames = preload_frames;
    }

    const size_t est = (size_t)mic.head_frames * 2 * sizeof(float);
    if (budget_bytes &&
        approx_bytes.fetch_add(est, std::memory_order_relaxed) + est
            > budget_bytes * 2) {
        return out;   // OOM guard (stejne jako prepareSampleFile)
    }

    WavData head = readSampleRange(mic.file, 0, mic.head_frames);
    if (!head.valid) {
        logger.log("bank", log::Severity::Warning,
                   "Packed: nelze nacist preload midi %d @ %llu",
                   out.midi, (unsigned long long)mic.file.pcm_offset);
        return out;
    }
    mic.preload_head = std::move(head.samples);

    // Baked analyza z indexu — zadne mereni.
    const float rms = e.rms_db;
    const int   ae  = (int)e.attack_end;
    if (mic.mode == MicLayerMode::Streamed)
        mic.resonance_start_frame = ae;

    out.bytes = mic.preload_head.size() * sizeof(float);
    SampleAsset asset;
    asset.peak_rms_db      = rms;
    asset.attack_end_frame = ae;
    asset.mics.push_back(std::move(mic));
    out.slot.rms_db = rms;
    out.slot.variants.push_back(std::move(asset));
    out.ok = true;
    return out;
}

// Nacteni pakovane banky: kostra primo z indexu (zadny directory scan, zadna
// RMS analyza, zadny sort — index je predrazeny dle (midi, rms)). Faze heads
// bezi paralelne jako u adresarove banky; merge ve scan poradi = poradi
// indexu, takze sloty prijdou vzestupne dle baked RMS.
void loadPackedBank(Bank& bank, const std::string& dir, log::Logger& logger,
                    int cache_budget_mb, int midi_from, int midi_to,
                    int preload_ms, BankLoadProgress* progress) {
    namespace fs = std::filesystem;
    const std::string ithaca_path = (fs::path(dir) / kIthacaFileName).string();
    IthacaBankFile pf = openIthacaBank(ithaca_path);
    if (!pf.ok) {
        logger.log("bank", log::Severity::Error,
                   "Banka '%s': soundbank.ithaca odmitnut — %s",
                   bank.name.c_str(), pf.error.c_str());
        return;   // prazdna banka (format zustava PackedIthaca)
    }
    logger.log("bank", log::Severity::Info,
               "Banka '%s': packed-ithaca, %zu zaznamu",
               bank.name.c_str(), pf.entries.size());

    const size_t budget_bytes = cache_budget_mb > 0
                              ? (size_t)cache_budget_mb * 1024 * 1024 : 0;
    std::vector<int> idx;
    idx.reserve(pf.entries.size());
    for (int i = 0; i < (int)pf.entries.size(); ++i) {
        const int midi = (int)pf.entries[(size_t)i].midi;
        if (midi < midi_from || midi > midi_to) continue;
        idx.push_back(i);
    }
    if (progress) {
        progress->phase.store(1);
        progress->total.store((int)idx.size());
        progress->done.store(0);
    }
    std::vector<PreparedSample> prepared(idx.size());
    std::atomic<size_t> approx_bytes{0};
    parallelFor((int)idx.size(), loaderWorkers(), [&](int i) {
        prepared[(size_t)i] = preparePackedSample(
            pf.entries[(size_t)idx[(size_t)i]], pf.handle, ithaca_path,
            logger, preload_ms, budget_bytes, approx_bytes);
        if (progress) {
            progress->done.fetch_add(1, std::memory_order_relaxed);
            progress->bytes_loaded.fetch_add(prepared[(size_t)i].bytes,
                                             std::memory_order_relaxed);
        }
    });
    mergePrepared(bank, prepared, budget_bytes, cache_budget_mb, logger, progress);
    // ZADNY sortBankSlotsByRms: poradi indexu (midi, rms vzestupne) je
    // autoritativni; std::sort neni stabilni a u shodnych RMS by mohl
    // prohodit bake poradi.
    logBankSummary(bank, logger, cache_budget_mb);
}

} // namespace

Bank loadBank(const std::string& dir, log::Logger& logger,
              int cache_budget_mb,
              int midi_from, int midi_to,
              int preload_ms,
              int resonance_window_ms,
              BankLoadProgress* progress) {
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
    if (scan.format == BankFormat::PackedIthaca) {
        loadPackedBank(bank, dir, logger, cache_budget_mb, midi_from, midi_to,
                       preload_ms, progress);
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
    // Filtr eligible souboru predem (progress total + podklad pro paralelni
    // ingest): indexy do scan.files.
    std::vector<int> idx;
    idx.reserve(scan.files.size());
    for (int i = 0; i < (int)scan.files.size(); ++i) {
        const ParsedName& p = scan.files[(size_t)i].parsed;
        if (p.midi < 0 || p.midi > 127) continue;
        if (p.midi < midi_from || p.midi > midi_to) continue;
        idx.push_back(i);
    }
    if (progress) {
        progress->phase.store(1);
        progress->total.store((int)idx.size());
        progress->done.store(0);
    }
    (void)resonance_window_ms;   // resonanci plni az buildResonanceCache
    // Paralelni priprava (cteni + analyza) — kazdy worker pise jen svuj index.
    std::vector<PreparedSample> prepared(idx.size());
    std::atomic<size_t> approx_bytes{0};
    parallelFor((int)idx.size(), loaderWorkers(), [&](int i) {
        const BankFileEntry& e = scan.files[(size_t)idx[(size_t)i]];
        prepared[(size_t)i] = prepareSampleFile(
            e.parsed.midi, e.full_path, e.parsed.filename, logger,
            preload_ms, budget_bytes, approx_bytes);
        if (progress) {
            progress->done.fetch_add(1, std::memory_order_relaxed);
            progress->bytes_loaded.fetch_add(prepared[(size_t)i].bytes,
                                             std::memory_order_relaxed);
        }
    });
    // Merge jednovlaknove ve scan poradi → deterministicka banka + presny budget.
    mergePrepared(bank, prepared, budget_bytes, cache_budget_mb, logger, progress);

    sortBankSlotsByRms(bank);
    logBankSummary(bank, logger, cache_budget_mb);
    return bank;
}

std::array<bool, 128> buildResonanceCache(Bank& bank, float target_db,
                                          int window_ms, log::Logger& logger,
                                          BankLoadProgress* progress) {
    std::array<bool, 128> ready{};   // vse false
    if (progress) {
        int recorded = 0;
        for (int n = 0; n < 128; ++n)
            if (bank.notes[(size_t)n].recorded && !bank.notes[(size_t)n].slots.empty())
                ++recorded;
        progress->phase.store(2);
        progress->total.store(recorded);
        progress->done.store(0);
    }
    // Paralelne pres noty: kazda nota pise JEN sve sloty a svuj prvek ready[n]
    // (nezavisle pametove lokace); logger ma mutex, progress je atomic.
    parallelFor(128, loaderWorkers(), [&](int n) {
        auto& note = bank.notes[(size_t)n];
        if (!note.recorded || note.slots.empty()) return;
        if (progress) progress->done.fetch_add(1, std::memory_order_relaxed);
        const int si = nearestSlotByRms(note, target_db);
        if (si < 0) return;
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
            WavData rd = readSampleRange(m.file, m.resonance_start_frame, rwin);
            if (rd.valid && rd.frames > 0) {
                m.resonance_frames  = rd.frames;
                m.preload_resonance = std::move(rd.samples);
                ready[(size_t)n] = true;
                if (progress)
                    progress->bytes_loaded.fetch_add(
                        m.preload_resonance.size() * sizeof(float),
                        std::memory_order_relaxed);
            } else {
                logger.log("bank", log::Severity::Warning,
                           "buildResonanceCache: read failed note %d", n);
            }
        }
    });
    return ready;
}

} // namespace ithaca
