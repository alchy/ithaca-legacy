#include "dsp/convolver.h"
#include "dsp/ir_modal.h"
#include "dsp/ir_wav.h"
#include <algorithm>
#include <cmath>

namespace ithaca::dsp {

const Param Convolver::kParams[3] = {
    {"mix",   "MIX",   0.f, 1.f, 0.15f, "%.2f", false},
    {"decay", "DECAY", 0.f, 1.f, 0.50f, "%.2f", false},
    {"tone",  "TONE",  0.f, 1.f, 0.60f, "%.2f", false},
};

void Convolver::prepare(float sr, int /*max_block*/) {
    sr_ = sr;
    buf_l_.assign(kMaxIr, 0.f);
    buf_r_.assign(kMaxIr, 0.f);
    write_pos_ = 0;
    choice_names_ = {"Body soft (modal)", "Body bright (modal)"};
    base_ir_ = generateModalIr(IrPreset::BodySoft, sr_, kMaxIr);
    rebuildIr();
    cur_choice_.store(0, std::memory_order_relaxed);
}

void Convolver::reset() {
    std::fill(buf_l_.begin(), buf_l_.end(), 0.f);
    std::fill(buf_r_.begin(), buf_r_.end(), 0.f);
    write_pos_ = 0;
}

void Convolver::setIR(const std::vector<float>& ir) {
    const int inactive = 1 - active_.load(std::memory_order_relaxed);
    std::vector<float> tmp = ir;
    if ((int)tmp.size() > kMaxIr) tmp.resize(kMaxIr);
    float peak = 0.f; for (float v : tmp) peak = std::max(peak, std::fabs(v));
    if (peak > 1e-9f) { const float g = 1.f/peak; for (auto& v : tmp) v *= g; }
    ir_[(size_t)inactive] = std::move(tmp);
    active_.store(inactive, std::memory_order_release);
}

int Convolver::irLength() const {
    return (int)ir_[(size_t)active_.load(std::memory_order_acquire)].size();
}

void Convolver::process(float* L, float* R, int n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    const auto& ir = ir_[(size_t)active_.load(std::memory_order_acquire)];
    const int M = (int)ir.size();
    if (M == 0) return;
    const float wet = mix_.load(std::memory_order_relaxed);
    const float dry = 1.f - wet;
    for (int i = 0; i < n; ++i) {
        buf_l_[(size_t)write_pos_] = L[i];
        buf_r_[(size_t)write_pos_] = R[i];
        float oL = 0.f, oR = 0.f;
        int rp = write_pos_;
        for (int k = 0; k < M; ++k) {
            oL += ir[(size_t)k] * buf_l_[(size_t)rp];
            oR += ir[(size_t)k] * buf_r_[(size_t)rp];
            if (--rp < 0) rp = kMaxIr - 1;
        }
        L[i] = dry * L[i] + wet * oL;
        R[i] = dry * R[i] + wet * oR;
        if (++write_pos_ >= kMaxIr) write_pos_ = 0;
    }
}

int Convolver::choiceCount() const { return (int)choice_names_.size(); }
const char* Convolver::choiceName(int i) const {
    return (i >= 0 && i < (int)choice_names_.size()) ? choice_names_[(size_t)i].c_str() : "";
}
void Convolver::selectChoice(int i) {
    if (i < 0 || i >= (int)choice_names_.size()) return;
    cur_choice_.store(i, std::memory_order_relaxed);
    if (i == 0)      base_ir_ = generateModalIr(IrPreset::BodySoft,   sr_, kMaxIr);
    else if (i == 1) base_ir_ = generateModalIr(IrPreset::BodyBright, sr_, kMaxIr);
    else {
        std::vector<float> ir;
        if (loadIrWavMono(choice_names_[(size_t)i], sr_, kMaxIr, ir)) base_ir_ = std::move(ir);
        else return;
    }
    rebuildIr();
}

void Convolver::rebuildIr() {
    if (base_ir_.empty()) return;
    const float decay = decay_.load(std::memory_order_relaxed);
    const float tone  = tone_.load(std::memory_order_relaxed);
    std::vector<float> tmp = base_ir_;
    // DECAY: extra exponencialni okno (nizsi = kratsi/subtilnejsi); decay=1 → beze zmeny
    const float k = (1.f - decay) * (1.f / (0.01f * sr_));
    if (k > 0.f) for (size_t t = 0; t < tmp.size(); ++t) tmp[t] *= std::exp(-k * (float)t);
    // TONE: one-pole LP (nizsi = tmavsi); tone=1 → ~Nyquist (bright/passthrough)
    const float fc = 400.f + tone * (0.45f * sr_ - 400.f);
    const float a  = 1.f - std::exp(-2.f * 3.14159265f * fc / sr_);
    float y = 0.f;
    for (auto& v : tmp) { y += a * (v - y); v = y; }
    setIR(tmp);   // normalizuje + atomicky publikuje
}

} // namespace ithaca::dsp
