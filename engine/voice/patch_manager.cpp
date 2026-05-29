// engine/voice/patch_manager.cpp — viz patch_manager.h.
#include "voice/patch_manager.h"

#include <cmath>

namespace ithaca {

namespace {

// Najde nejblizsi nahranou notu k `midi`. Vrati -1 kdyz banka nema zadnou.
int nearestRecordedNote(const Bank& bank, int midi) {
    if (bank.notes[midi].recorded) return midi;
    for (int d = 1; d < 128; ++d) {
        int lo = midi - d, hi = midi + d;
        if (lo >= 0 && bank.notes[lo].recorded)  return lo;
        if (hi < 128 && bank.notes[hi].recorded) return hi;
    }
    return -1;
}

// Namapuj velocity 0-127 na index slotu (sloty serazene vzestupne dle RMS).
int slotIndexForVelocity(int velocity, int nslots) {
    if (nslots <= 1) return 0;
    float t = (float)velocity / 127.f;                 // 0..1
    int idx = (int)std::lround(t * (float)(nslots - 1));
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

    int src = nearestRecordedNote(bank, midi);
    if (src < 0) return out;                            // prazdna banka

    const NoteSlots& note = bank.notes[src];
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
        int last = (sidx < 16) ? rr.last[src][sidx] : -1;
        // Nahodny vyber, opakuj dokud != last (pri nvar>=2 vzdy skonci rychle).
        do {
            chosen = (int)(lcgNext(rr.rng) % (uint32_t)nvar);
        } while (chosen == last);
        if (sidx < 16) rr.last[src][sidx] = chosen;
    }

    out.asset       = &slot.variants[chosen];
    out.pitch_ratio = std::pow(2.0, (double)(midi - src) / 12.0);
    float vn = (float)velocity / 127.f;
    out.vel_gain    = vn * vn;                          // percepcni (kvadraticky)
    return out;
}

} // namespace ithaca
