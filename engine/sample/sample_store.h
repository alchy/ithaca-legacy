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
Bank loadLegacyBank(const std::string& dir, log::Logger& logger,
                    int cache_budget_mb = 0,
                    int midi_from = 0, int midi_to = 127);

} // namespace ithaca
