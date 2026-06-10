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
#include <string>

namespace ithaca {

// Nacte banku z adresare `dir` do RAM; format se detekuje automaticky
// (BankFormat ulozeny v Bank.format). Loguje prubeh pres `logger`.
// cache_budget_mb: RAM strop (OOM guard) — kdyz prubezny soucet preload dat
// prekroci rozpocet, nacitani se PRERUSI (ERROR log, banka zustane neuplna).
// 0 = bez kontroly rozpoctu.
// midi_from / midi_to: nacti jen noty v tomto inkluzivnim rozsahu (default 0..127 = vse).
// preload_ms: kolik ms zacatku kazdeho samplu drzet v RAM (head preload).
// Kratky sampl (vejde se do 2 * preload_ms) zustava cely v RAM (FullyLoaded);
// dlouhy je oznacen Streamed a zbytek se ve fazi 4 streamuje.
Bank loadBank(const std::string& dir, log::Logger& logger,
              int cache_budget_mb = 0,
              int midi_from = 0, int midi_to = 127,
              int preload_ms = 150,
              int resonance_window_ms = 500);

// Naplni preload_resonance JEN pro per-notu cilovou velocity vrstvu
// (nearestSlotByRms(target_db)), oknem window_ms od resonance_start_frame
// (Streamed mics; cte z disku). Drive cilove sloty, ktere uz cilove nejsou, vycisti.
// FullyLoaded cilove nepotrebuji buffer (hraji z preload_head). Vraci pole
// 128 bool: true = nota ma platnou cilovou vrstvu (cache pouzitelna).
// Off-RT (disk I/O). Volat AZ po sortBankSlotsByRms.
std::array<bool, 128> buildResonanceCache(Bank& bank, float target_db,
                                          int window_ms, log::Logger& logger);

} // namespace ithaca
