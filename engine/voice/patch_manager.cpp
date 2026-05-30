// engine/voice/patch_manager.cpp — viz patch_manager.h.
//
// Pozn. (2026-05-30): puvodni "fallback ve dvou osach" (pitch-shift z nejblizsi
// nahrane noty) je ZRUSEN. Chybejici nota → prazdny VoiceSpec → player to
// chape jako ticho. Pitch-shift kod je presunut do _reserved_resampling.h pro
// pripadne budouci pouziti. Velocity-slot vyber (mapovani MIDI vel na nahrane
// dynamiky noty) ZUSTAVA — to neni odvozovani, jen vyber z toho co je nahrane.

#include "voice/patch_manager.h"

#include <cstdint>

namespace ithaca {

namespace {

// Namapuj velocity 0-127 na index slotu (sloty serazene vzestupne dle RMS).
int slotIndexForVelocity(int velocity, int nslots) {
    if (nslots <= 1) return 0;
    float t = (float)velocity / 127.f;                 // 0..1
    int idx = (int)((t * (float)(nslots - 1)) + 0.5f); // round
    if (idx < 0) idx = 0;
    if (idx >= nslots) idx = nslots - 1;
    return idx;
}

// LCG krok — levny deterministicky RNG (Numerical Recipes konstanty).
uint32_t lcgNext(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

} // namespace

VoiceSpec selectVoice(const Bank& bank, int midi, int velocity, RoundRobinState& rr) {
    VoiceSpec out;
    if (midi < 0 || midi > 127) return out;

    // Bez resamplingu: pouziva se VYHRADNE nahrany sampl pro tuto notu.
    // Kdyz nota neni nahrana → ticho (prazdny VoiceSpec).
    if (!bank.notes[midi].recorded) return out;

    const NoteSlots& note = bank.notes[midi];
    if (note.slots.empty()) return out;

    int nslots = (int)note.slots.size();
    int sidx = slotIndexForVelocity(velocity, nslots);
    const VelocitySlot& slot = note.slots[sidx];
    if (slot.variants.empty()) return out;

    // Round-robin: vyber variantu != naposledy hrana (kdyz je vic nez jedna).
    int nvar = (int)slot.variants.size();
    int chosen = 0;
    if (nvar == 1) {
        chosen = 0;
    } else {
        int last = (sidx < 16) ? rr.last[midi][sidx] : -1;
        // Nahodny vyber, opakuj dokud != last (pri nvar>=2 vzdy skonci rychle).
        do {
            chosen = (int)(lcgNext(rr.rng) % (uint32_t)nvar);
        } while (chosen == last);
        if (sidx < 16) rr.last[midi][sidx] = chosen;
    }

    out.asset       = &slot.variants[chosen];
    out.pitch_ratio = 1.0;                              // bez transpozice
    float vn = (float)velocity / 127.f;
    out.vel_gain    = vn * vn;                          // percepcni (kvadraticky)
    return out;
}

} // namespace ithaca
