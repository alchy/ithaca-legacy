#pragma once
// engine/resonance/resonance_layer_select.h
// Vyber velocity slotu pro sympatickou rezonanci: vrati index slotu, jehoz
// rms_db je nejbliz cilove hodnote target_db. Pri shode vzdalenosti nizsi index.
// Prazdne slots → -1. Cista funkce (testovatelna bez enginu).
#include "sample/sample_types.h"

namespace ithaca {
int nearestSlotByRms(const NoteSlots& ns, float target_db);
}
