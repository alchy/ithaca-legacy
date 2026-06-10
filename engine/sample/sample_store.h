#pragma once
// engine/sample/sample_store.h
// ----------------------------
// Nacteni banky do RAM. loadBank() sam detekuje format (scanBank): fixed-velocity
// (ploche mNNN-velV-fSS.wav) i dynamic-velocity (podslozky mNNN/<hash>.wav). Per-
// sample ingesce je pro oba formaty stejna; velocity vrstvy se radi dle mereneho
// peak RMS. All-in-RAM head preload + streaming (faze 4). cache_budget_mb je
// RAM strop (OOM guard): pri prekroceni se nacitani PRERUSI (ERROR, neuplna
// banka) misto pádu na bad_alloc.

#include "sample/sample_types.h"
#include "util/log.h"

#include <array>
#include <atomic>
#include <string>

namespace ithaca {

// Prubeh nacitani banky pro GUI (vlastni ji caller — typicky AppContext;
// loader jen plni atomiky, GUI je polluje per frame).
// phase: 0 = scan, 1 = heads (done/total = soubory),
//        2 = rezonancni cache (done/total = nahrane noty), 3 = hotovo.
struct BankLoadProgress {
    std::atomic<int> phase{0};
    std::atomic<int> done{0};
    std::atomic<int> total{0};
};

// Mapovani fazi na jeden progress bar 0..1: heads = 0..0.6, cache = 0.6..1.0
// (cache build cte ~stovky MB z disku — bez vahy by bar "visel" na 100 %).
inline float bankLoadFraction(int phase, int done, int total) {
    const float t = (total > 0) ? (float)done / (float)total : 0.f;
    switch (phase) {
        case 1:  return 0.6f * t;
        case 2:  return 0.6f + 0.4f * t;
        case 3:  return 1.f;
        default: return 0.f;   // scan
    }
}

// Nacte banku z adresare `dir` do RAM; format se detekuje automaticky
// (BankFormat ulozeny v Bank.format). Loguje prubeh pres `logger`.
// cache_budget_mb: RAM strop (OOM guard) — kdyz prubezny soucet preload dat
// prekroci rozpocet, nacitani se PRERUSI (ERROR log, banka zustane neuplna).
// 0 = bez kontroly rozpoctu.
// midi_from / midi_to: nacti jen noty v tomto inkluzivnim rozsahu (default 0..127 = vse).
// preload_ms: kolik ms zacatku kazdeho samplu drzet v RAM (head preload).
// Kratky sampl (vejde se do 2 * preload_ms) zustava cely v RAM (FullyLoaded);
// dlouhy je oznacen Streamed a zbytek se ve fazi 4 streamuje.
// progress (volitelne): loader plni atomiky pro GUI (faze 1 = heads).
Bank loadBank(const std::string& dir, log::Logger& logger,
              int cache_budget_mb = 0,
              int midi_from = 0, int midi_to = 127,
              int preload_ms = 150,
              int resonance_window_ms = 500,
              BankLoadProgress* progress = nullptr);

// Naplni preload_resonance JEN pro per-notu cilovou velocity vrstvu
// (nearestSlotByRms(target_db)), oknem window_ms od resonance_start_frame
// (Streamed mics; cte z disku). Drive cilove sloty, ktere uz cilove nejsou, vycisti.
// FullyLoaded cilove nepotrebuji buffer (hraji z preload_head). Vraci pole
// 128 bool: true = nota ma platnou cilovou vrstvu (cache pouzitelna).
// Off-RT (disk I/O). Volat AZ po sortBankSlotsByRms.
std::array<bool, 128> buildResonanceCache(Bank& bank, float target_db,
                                          int window_ms, log::Logger& logger,
                                          BankLoadProgress* progress = nullptr);

} // namespace ithaca
