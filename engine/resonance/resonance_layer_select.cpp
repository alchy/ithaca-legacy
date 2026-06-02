// engine/resonance/resonance_layer_select.cpp — viz .h
#include "resonance/resonance_layer_select.h"
#include <cmath>

namespace ithaca {
int nearestSlotByRms(const NoteSlots& ns, float target_db) {
    int best = -1;
    float bestd = 1e30f;
    for (size_t i = 0; i < ns.slots.size(); ++i) {
        const float d = std::fabs(ns.slots[i].rms_db - target_db);
        if (d < bestd) { bestd = d; best = (int)i; }   // strict < → nizsi index pri shode
    }
    return best;
}
}
