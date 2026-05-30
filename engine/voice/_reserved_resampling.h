#pragma once
// engine/voice/_reserved_resampling.h
// -----------------------------------
// RESERVED FOR FUTURE USE — neaktivni kod, neni zapojen do builduje.
//
// Pri navrhu faze 3 byl puvodne v patch_manageru fallback "kdyz chybi nota,
// transponuj nejblizsi nahranou notu pitch-shiftem na cilovou vysku" (dve osy:
// nota + velocity). Rozhodnuti uzivatele 2026-05-30: ZADNY resampling /
// odvozovani neexistujicich samplu. Chybejici (nota, velocity) → ticho.
//
// Kod nize je puvodni implementace presunuta sem pro pripadne budouci pouziti.
// Pokud se nekdy resampling vrati (treba pri tvorbe redsamplovani SR mezi
// 44.1/48/96 kHz pri shode noty+velocity), startovni bod je tady.
//
// JAK SE TO POUZIVALO:
//   src = nearestRecordedNote(bank, midi);
//   if (src >= 0 && src != midi)
//       pitch_ratio = std::pow(2.0, (midi - src) / 12.0);
//   // pos_inc v Voice::start by pak nasobilo prehravaci rychlost.

#include "sample/sample_types.h"

#include <cmath>

namespace ithaca::reserved {

// Najde nejblizsi nahranou notu k `midi`. Vrati -1 kdyz banka nema zadnou.
inline int nearestRecordedNote(const Bank& bank, int midi) {
    if (midi < 0 || midi > 127) return -1;
    if (bank.notes[midi].recorded) return midi;
    for (int d = 1; d < 128; ++d) {
        int lo = midi - d, hi = midi + d;
        if (lo >= 0  && bank.notes[lo].recorded) return lo;
        if (hi < 128 && bank.notes[hi].recorded) return hi;
    }
    return -1;
}

// Pitch ratio pro transpozici o (target - source) pultonu.
// 1.0 = bez transpozice; 2.0 = o oktavu vys; 0.5 = o oktavu niz.
inline double semitonePitchRatio(int target_midi, int source_midi) {
    return std::pow(2.0, (double)(target_midi - source_midi) / 12.0);
}

} // namespace ithaca::reserved
