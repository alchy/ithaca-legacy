// engine/resonance/harmonic_proximity.cpp — viz .h
// Partial-coincidence model z idealni harmonicke rady (12-TET zakladni
// frekvence). Strunu N budi parcialy hrane noty P, ktere padnou na parcialy N.
// Sila = Σ_{k,m} drive(k)·recv(m)·overlap(detuning) pres pary parcialu.
// Predpocitano do normalizovane 128×128 matice pri prvnim volani (octave-up≈1.0).
// b > a → fyzikalni up/down asymetrie (oktava nahoru budi ZAKLAD partnera,
// oktava dolu jen jeho 2. parcial → tissi). Viz spec 2026-06-02.
#include "resonance/harmonic_proximity.h"

#include <array>
#include <cmath>

namespace ithaca {

namespace {

constexpr int   kPartials       = 16;     // K — pocet uvazovanych parcialu
constexpr float kDriveExp       = 1.0f;   // A(k) = 1/k^a (energie parcialu P)
constexpr float kRecvExp        = 2.0f;   // R(m) = 1/m^b (receptivita N; b>a → asymetrie)
constexpr float kBandwidthCents = 12.0f;  // sigma rezonancni sirky [centy]

inline float midiHz(int n) {
    return 440.f * std::pow(2.f, (float)(n - 69) / 12.f);
}

// Raw coupling prox(target N, source P): Σ A(k)·R(m)·exp(-(Δc/σ)^2).
float rawProx(int target, int source) {
    if (target == source) return 0.f;
    const float fP = midiHz(source);
    const float fN = midiHz(target);
    float sum = 0.f;
    for (int k = 1; k <= kPartials; ++k) {
        const float fk = (float)k * fP;
        const float A  = 1.f / std::pow((float)k, kDriveExp);
        for (int m = 1; m <= kPartials; ++m) {
            const float fm = (float)m * fN;
            const float dc = 1200.f * std::fabs(std::log2(fk / fm));   // detuning [centy]
            const float x  = dc / kBandwidthCents;
            const float g  = std::exp(-x * x);
            if (g < 1e-4f) continue;   // zanedbatelny prispevek
            const float R  = 1.f / std::pow((float)m, kRecvExp);
            sum += A * R * g;
        }
    }
    return sum;
}

// Predpocitana normalizovana matice (lazy function-local static → thread-safe
// jednorazova inicializace). Build: 128×128×K² ≈ 4M flops jednou pri startu.
const std::array<std::array<float, 128>, 128>& couplingMatrix() {
    static const std::array<std::array<float, 128>, 128> M = [] {
        std::array<std::array<float, 128>, 128> mat{};
        float maxv = 0.f;
        for (int t = 0; t < 128; ++t)
            for (int s = 0; s < 128; ++s) {
                const float v = rawProx(t, s);
                mat[(size_t)t][(size_t)s] = v;
                if (v > maxv) maxv = v;
            }
        if (maxv > 0.f)
            for (auto& row : mat)
                for (auto& v : row) v /= maxv;   // octave-up → 1.0
        return mat;
    }();
    return M;
}

} // namespace

float harmonicProximity(int target_midi, int source_midi) {
    if (target_midi < 0 || target_midi > 127 ||
        source_midi < 0 || source_midi > 127) return 0.f;
    return couplingMatrix()[(size_t)target_midi][(size_t)source_midi];
}

} // namespace ithaca
