#pragma once
// engine/dsp/dsp_chain.h — pevny modularni DSP retezec: AGC -> ENHANCER -> Limiter.
// process() spusti jen enabled() stage (disabled = no-op passthrough).
// stage(i) vraci referenci pro GUI (i=0 AGC, 1 ENHANCER, 2 LIMITER).
#include "dsp/dsp_stage.h"
#include "dsp/agc.h"
#include "dsp/enhancer.h"
#include "dsp/limiter.h"

namespace ithaca::dsp {

class DspChain {
public:
    void prepare(float sample_rate, int max_block) {
        for (auto* s : stages_) s->prepare(sample_rate, max_block);
    }
    void reset() { for (auto* s : stages_) s->reset(); }
    void process(float* L, float* R, int n) {
        for (auto* s : stages_) if (s->enabled()) s->process(L, R, n);
    }
    int stageCount() const { return 3; }
    DspStage& stage(int i) { return *stages_[i]; }

private:
    AGC      agc_;
    Enhancer enhancer_;
    Limiter  lim_;
    DspStage* stages_[3] = { &agc_, &enhancer_, &lim_ };
};

} // namespace ithaca::dsp
