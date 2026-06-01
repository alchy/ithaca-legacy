#pragma once
// engine/sample/sample_store.h
// ----------------------------
// Nacteni banky do RAM. loadBank() sam detekuje format (scanBank): fixed-velocity
// (ploche mNNN-velV-fSS.wav) i dynamic-velocity (podslozky mNNN/<hash>.wav). Per-
// sample ingesce je pro oba formaty stejna; velocity vrstvy se radi dle mereneho
// peak RMS. All-in-RAM head preload + streaming (faze 4). cache_budget_mb je jen
// diagnosticky WARNING.

#include "sample/sample_types.h"
#include "util/log.h"

#include <string>

namespace ithaca {

// Nacte banku z adresare `dir` do RAM; format se detekuje automaticky
// (BankFormat ulozeny v Bank.format). Loguje prubeh pres `logger`.
// cache_budget_mb: kdyz nactena data presahnou rozpocet, jen WARNING (porad
// nacita vse). 0 = bez kontroly rozpoctu.
// midi_from / midi_to: nacti jen noty v tomto inkluzivnim rozsahu (default 0..127 = vse).
// preload_ms: kolik ms zacatku kazdeho samplu drzet v RAM (head preload).
// Kratky sampl (vejde se do 2 * preload_ms) zustava cely v RAM (FullyLoaded);
// dlouhy je oznacen Streamed a zbytek se ve fazi 4 streamuje.
Bank loadBank(const std::string& dir, log::Logger& logger,
              int cache_budget_mb = 0,
              int midi_from = 0, int midi_to = 127,
              int preload_ms = 150,
              int resonance_window_ms = 500);

} // namespace ithaca
