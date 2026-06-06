#include "dsp/convolver.h"
#include "dsp/ir_modal.h"
#include "dsp/ir_wav.h"
#include <algorithm>
#include <cmath>

namespace ithaca::dsp {

const Param Convolver::kParams[4] = {
    {"mix",   "MIX",   0.f, 1.f, 0.15f, "%.2f", false},
    {"decay", "DECAY", 0.f, 1.f, 0.50f, "%.2f", false},
    {"tone",  "TONE",  0.f, 1.f, 0.60f, "%.2f", false},
    {"size",  "SIZE",  0.f, 1.f, 0.50f, "%.2f", false},
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
    // Dva flat passy misto jednoho cyklu s wraparound branchem:
    //   pass A — od write_pos_ smerem k 0  (head ringu)
    //   pass B — od kMaxIr-1 smerem dolu   (wrapped tail), pokud M presahne head
    // Inner loopy nemaji `if` ani loop-carried index → MSVC i clang
    // autovektorizuji na SSE2/AVX2/NEON (4-8x speedup).
    // Pointery na surove buffery — autovektorizace na std::vector indexovani
    // pres operator[] obcas zdrahava kvuli aliasing analyze.
    float* const bl = buf_l_.data();
    float* const br = buf_r_.data();
    const float* const ip = ir.data();
    for (int i = 0; i < n; ++i) {
        bl[write_pos_] = L[i];
        br[write_pos_] = R[i];
        float oL = 0.f, oR = 0.f;
        const int headN = (M < write_pos_ + 1) ? M : (write_pos_ + 1);
        for (int k = 0; k < headN; ++k) {
            const int j = write_pos_ - k;
            oL += ip[k] * bl[j];
            oR += ip[k] * br[j];
        }
        const int tailN = M - headN;
        const int base = kMaxIr - 1;
        for (int k = 0; k < tailN; ++k) {
            const int j = base - k;
            oL += ip[headN + k] * bl[j];
            oR += ip[headN + k] * br[j];
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
    // SIZE: time-resample IR (vetsi telo → roztazeni = nizsi rezonance + delsi).
    // size=0.5 → scale 1.0 neutral; 0.6 (male) .. 1.4 (velke).
    const float size = size_.load(std::memory_order_relaxed);
    const float scale = 0.6f + size * 0.8f;
    std::vector<float> tmp;
    {
        int nl = (int)((float)base_ir_.size() * scale);
        if (nl < 2) nl = 2;
        if (nl > kMaxIr) nl = kMaxIr;
        tmp.resize((size_t)nl);
        // linearni interpolace: tmp[i] = base_ir_[i/scale]
        for (int i = 0; i < nl; ++i) {
            float sp = (float)i / scale;
            int idx = (int)sp; float fr = sp - (float)idx;
            if (idx + 1 < (int)base_ir_.size()) tmp[(size_t)i] = base_ir_[(size_t)idx]*(1.f-fr) + base_ir_[(size_t)(idx+1)]*fr;
            else if (idx < (int)base_ir_.size()) tmp[(size_t)i] = base_ir_[(size_t)idx]*(1.f-fr);
            else tmp[(size_t)i] = 0.f;
        }
    }
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
