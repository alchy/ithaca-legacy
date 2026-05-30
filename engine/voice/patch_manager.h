#pragma once
// engine/voice/patch_manager.h
// ----------------------------
// Rozhodovaci logika prehravani: pro (midi, velocity) vybere konkretni
// SampleAsset z banky + pitch ratio (transpozice u chybejicich not) + vel_gain.
// Ciste funkce nad Bank — bez I/O a bez audio stavu (snadno testovatelne).
// Round-robin stav je drzeny zvlast (RoundRobinState).

#include "sample/sample_types.h"

#include <cstdint>

namespace ithaca {

// Vysledek vyberu: co a jak hrat. asset == nullptr → neni co hrat.
struct VoiceSpec {
    const SampleAsset* asset       = nullptr;
    double             pitch_ratio = 1.0;   // nasobic rychlosti prehravani (transpozice)
    float              vel_gain    = 1.0f;  // gain z velocity
};

// Round-robin pamet: pro kazdou (notu, slot) posledni hranou variantu.
// Jednoduchy deterministicky RNG (LCG) — bez globalniho stavu, testovatelne.
struct RoundRobinState {
    // last[midi][slot] = index naposledy hrane varianty (-1 = jeste nehrano).
    // Pole je rezervovano lazy az pri pouziti, aby struct zustal levny.
    int  last[128][16];
    uint32_t rng = 0x12345678u;            // seed; meni se pri kazdem vyberu
    RoundRobinState() { for (auto& n : last) for (int& s : n) s = -1; }
};

// Vybere hlas pro (midi, velocity). Aktualizuje rr (round-robin + rng).
VoiceSpec selectVoice(const Bank& bank, int midi, int velocity, RoundRobinState& rr);

} // namespace ithaca
