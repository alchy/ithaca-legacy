// engine/pedal/pedal_state.cpp — viz pedal_state.h.
#include "pedal/pedal_state.h"

namespace ithaca {

void PedalState::setSustainCC(uint8_t cc) {
    cc64_ = cc;
    recompute();
}

void PedalState::noteOn(int midi) {
    if (midi < 0 || midi > 127) return;
    held_.set((size_t)midi);
    recompute();
}

void PedalState::noteOff(int midi) {
    if (midi < 0 || midi > 127) return;
    held_.reset((size_t)midi);
    recompute();
}

void PedalState::allNotesOff() {
    held_.reset();
    recompute();
}

void PedalState::recompute() {
    // Spojita damping mapa: drzena klavesa vzdy 1.0. Ne-drzena: dolni lost-motion
    // dead-zona (cc64 <= kDamperBiteCC → 0, dusitka jeste na strunach), nad ni
    // spojite cc64/127. Dead-zona zarucuje, ze po uvolneni pedalu (i kdyz
    // kontinualni pedal nedojede presne na 0) se ne-drzene struny zatlumi.
    const float lift = (cc64_ <= kDamperBiteCC) ? 0.f : (float)cc64_ / 127.f;
    for (int n = 0; n < 128; ++n) {
        damping_[n] = held_[(size_t)n] ? 1.f : lift;
    }
}

float PedalState::dampingFor(int midi) const {
    if (midi < 0 || midi > 127) return 0.f;
    return damping_[midi];
}

} // namespace ithaca
