#pragma once
// engine/sample/sample_loader.h
// -----------------------------
// Analyza uz nacteneho samplu: peak RMS (dBFS) a hranice attack/sustain.
// Ciste funkce nad interleaved stereo bufferem — zadne I/O.

namespace ithaca {

// Podlaha dBFS pro ticho (vyhneme se -inf z log10(0)).
constexpr float kSilenceFloorDb = -120.f;

// Maximum kratko-oknovych RMS (klouzave okno ~50 ms) v dBFS. Pocita z mono
// mixu (L+R)/2. data = interleaved stereo, frames = pocet stereo frames.
float measurePeakRmsDb(const float* data, int frames, int sample_rate);

// Index frame, kde okenni RMS poprve dosahne maxima — priblizny konec attacku
// / zacatek sustainu. Pouziva se pro resonance skip-attack (faze 5). Pro velmi
// kratke samply vrati 0.
int findAttackEnd(const float* data, int frames, int sample_rate);

} // namespace ithaca
