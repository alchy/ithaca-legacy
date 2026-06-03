#include "dsp/ir_modal.h"
#include <cmath>
#include <random>
#include <algorithm>
namespace ithaca::dsp {
namespace {
constexpr float kPi = 3.14159265358979f;
// spektralni obalka A(f): energie koncentrovana <~800 Hz (-12 dB/oct vyse),
// BodyBright pridava presence bump ~2.5 kHz.
float envelope(float f, IrPreset p) {
    const float lo = 800.f;
    float a = (f <= lo) ? 1.f : (lo/f)*(lo/f);   // -12 dB/oct nad 800 Hz
    if (p == IrPreset::BodyBright) {
        float g = std::exp(-std::pow((f - 2500.f)/1200.f, 2.f));
        a += 0.6f * g;
    }
    return a;
}
}
float soundboardDampingFve(float f) { return 2e-5f*f*f + 7e-2f*f; }

std::vector<float> generateModalIr(IrPreset preset, float sr, int max_len) {
    // Modalni frekvence: nizke anchory (RR_9530 mereni) + fill do ~5 kHz s mirne
    // rostoucim spacingem.
    std::vector<float> freqs = {27.f,42.f,63.f,86.f,121.f,164.f,210.f,260.f,289.f};
    const float fmax = 5000.f;
    for (float f = 340.f, df = 60.f; f < fmax; f += df, df *= 1.01f) freqs.push_back(f);

    int len = std::min(max_len, (int)(0.15f * sr));   // ~150 ms cap (transient body)
    if (len < 2) len = 2;
    std::vector<float> ir((size_t)len, 0.f);

    std::mt19937 rng(12345u);                          // fixni seed → reprodukovatelne
    std::uniform_real_distribution<float> phase(0.f, 2.f*kPi);
    for (float fn : freqs) {
        if (fn >= sr * 0.5f) continue;
        const float tau = 2.f / soundboardDampingFve(fn);   // s
        const float A   = envelope(fn, preset);
        const float phi = phase(rng);
        const float w   = 2.f * kPi * fn / sr;
        for (int t = 0; t < len; ++t)
            ir[(size_t)t] += A * std::exp(-(float)t/(tau*sr)) * std::sin(w*(float)t + phi);
    }
    // normalizace na unit peak
    float peak = 0.f; for (float v : ir) peak = std::max(peak, std::fabs(v));
    if (peak > 1e-9f) { const float g = 1.f/peak; for (auto& v : ir) v *= g; }
    return ir;
}
}
