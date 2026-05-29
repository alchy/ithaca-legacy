// engine/sample/sample_loader.cpp — viz sample_loader.h.
#include "sample/sample_loader.h"

#include <cmath>
#include <cstddef>   // size_t (libc++ ho z <cmath> negarantuje)

namespace ithaca {

namespace {
// Velikost okna pro klouzavy RMS v ms. 50 ms je kompromis: dost dlouhe na
// stabilni RMS, dost kratke na zachyceni peaku v transientu.
constexpr float kWindowMs = 50.f;

// Spocita okenni RMS (mono mix) a vrati (max_rms, frame_kde_max).
// Krok okna = polovina okna (50% overlap) pro rozumnou hustotu vzorkovani.
struct PeakResult { float max_rms; int peak_frame; };

PeakResult slidingPeakRms(const float* data, int frames, int sample_rate) {
    PeakResult r{0.f, 0};
    if (frames <= 0) return r;
    int win = (int)(kWindowMs * 0.001f * (float)sample_rate);
    if (win < 1) win = 1;
    if (win > frames) win = frames;
    const int hop = win > 1 ? win / 2 : 1;

    for (int start = 0; start + win <= frames; start += hop) {
        double acc = 0.0;
        for (int i = 0; i < win; ++i) {
            int f = start + i;
            float mono = 0.5f * (data[(size_t)f * 2] + data[(size_t)f * 2 + 1]);
            acc += (double)mono * (double)mono;
        }
        float rms = (float)std::sqrt(acc / (double)win);
        if (rms > r.max_rms) { r.max_rms = rms; r.peak_frame = start + win / 2; }
    }
    return r;
}
} // namespace

float measurePeakRmsDb(const float* data, int frames, int sample_rate) {
    if (frames <= 0 || data == nullptr) return kSilenceFloorDb;
    PeakResult r = slidingPeakRms(data, frames, sample_rate);
    if (r.max_rms <= 0.f) return kSilenceFloorDb;   // uplne ticho → vyhni se log10(0)
    float db = 20.f * std::log10(r.max_rms);
    // Jediny zdroj pravdy pro podlahu je kSilenceFloorDb — vse pod ni se na ni orizne.
    return db < kSilenceFloorDb ? kSilenceFloorDb : db;
}

int findAttackEnd(const float* data, int frames, int sample_rate) {
    if (frames <= 0 || data == nullptr) return 0;
    PeakResult r = slidingPeakRms(data, frames, sample_rate);
    return r.peak_frame;
}

} // namespace ithaca
