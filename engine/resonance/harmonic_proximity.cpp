// engine/resonance/harmonic_proximity.cpp — viz .h
#include "resonance/harmonic_proximity.h"

#include <cmath>
#include <cstdlib>

namespace ithaca {

namespace {

// Vahy intervalu modulo 12 (semiton 0..11). Hodnoty vychazi z harmonicke
// rady: kazdy obsazeny term v overtone serii dostane vahu cca podle sve
// energetiky.
constexpr float kIntervalWeight[12] = {
    1.00f,  // 0:  unison (vraci se 0 pres self-check, ale vaha pro oktavu)
    0.03f,  // 1:  m2  (same magnitude as tritonus; harmonicky zanedbatelne)
    0.03f,  // 2:  M2
    0.10f,  // 3:  m3
    0.20f,  // 4:  M3
    0.30f,  // 5:  P4
    0.03f,  // 6:  tritonus
    0.60f,  // 7:  P5
    0.10f,  // 8:  m6
    0.15f,  // 9:  M6
    0.10f,  // 10: m7
    0.20f,  // 11: M7
};

// Oktavovy pokles: kazda oktava vzdalenost x0.7
constexpr float kOctaveDecay = 0.7f;

} // namespace

float harmonicProximity(int target_midi, int source_midi) {
    if (target_midi == source_midi) return 0.f;
    int diff = std::abs(target_midi - source_midi);
    int octaves = diff / 12;
    int semis   = diff % 12;
    float w = kIntervalWeight[semis];
    // Pokles s oktavovou vzdalenosti.
    for (int i = 0; i < octaves; ++i) w *= kOctaveDecay;
    return w;
}

} // namespace ithaca
