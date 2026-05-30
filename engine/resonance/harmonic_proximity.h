#pragma once
// engine/resonance/harmonic_proximity.h
// -------------------------------------
// Vraci harmonickou "blizkost" mezi dvema MIDI strunami [0, 1] — jak silne
// nota `source` budi rezonanci na strune `target`. Cista funkce, bez stavu.
//
// Model (faze 5, jednoduchy):
//   1. Vaha intervalu modulo 12: oktava 1.0, kvinta 0.6, kvarta 0.3, ...
//   2. Klesa s oktavovou vzdalenosti — pricitam (12-step distance)*octave_decay.
//   3. f(N, N) = 0 (play-on-self resi voice_pool, ne rezonance).
//
// FUTURE (faze 5+): mozna nahradit FFT-based modelem podle skutecne spektralni
// energie samplu — to az kdyz budou nove banky.

namespace ithaca {

float harmonicProximity(int target_midi, int source_midi);

} // namespace ithaca
