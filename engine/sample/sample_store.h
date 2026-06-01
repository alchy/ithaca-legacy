#pragma once
// engine/sample/sample_store.h
// ----------------------------
// Nacteni banky do RAM. Faze 2 resi LEGACY: scan → nacti WAV → zmer RMS+attack
// → postav Bank. All-in-RAM (zadny streaming). cache_budget_mb je jen pro
// diagnosticky WARNING; evikce/streaming prijde ve fazi 4.

#include "sample/sample_types.h"
#include "util/log.h"

#include <string>

namespace ithaca {

// Nacte legacy banku z adresare `dir` do RAM. Loguje prubeh pres `logger`.
// cache_budget_mb: kdyz nactena data presahnou rozpocet, jen WARNING (porad
// nacita vse). 0 = bez kontroly rozpoctu.
// midi_from / midi_to: nacti jen noty v tomto inkluzivnim rozsahu (default 0..127 = vse).
// Slouzi k rychlemu testovani/renderu bez nacteni cele (vicegb) banky.
// preload_ms: kolik ms zacatku kazdeho samplu drzet v RAM (head preload).
// Kratky sampl (vejde se do 2 * preload_ms) zustava cely v RAM (FullyLoaded);
// dlouhy je oznacen Streamed a zbytek se ve fazi 4 streamuje.
Bank loadFixedVelocityBank(const std::string& dir, log::Logger& logger,
                    int cache_budget_mb = 0,
                    int midi_from = 0, int midi_to = 127,
                    int preload_ms = 150,
                    int resonance_window_ms = 500);

} // namespace ithaca
