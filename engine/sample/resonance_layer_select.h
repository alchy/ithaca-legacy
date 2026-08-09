#pragma once
// engine/sample/resonance_layer_select.h
// Zije v sample/, ne v resonance/: je to cista funkce nad typy ze
// sample/sample_types.h a vola ji sample_store i resonance_engine.
// V resonance/ tvorila adresarovy pseudo-cyklus sample <-> resonance
// (viz docs/architecture/dependencies.md).
// Vyber velocity slotu pro sympatickou rezonanci: vrati index slotu, jehoz
// rms_db je nejbliz cilove hodnote target_db. Pri shode vzdalenosti nizsi index.
// Prazdne slots → -1. Cista funkce (testovatelna bez enginu).
#include "sample/sample_types.h"

namespace ithaca {
int nearestSlotByRms(const NoteSlots& ns, float target_db);
}
