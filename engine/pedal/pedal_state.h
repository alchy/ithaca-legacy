#pragma once
// engine/pedal/pedal_state.h
// --------------------------
// PedalState drzi aktualni stav sustain pedalu (CC64, 0-127) a **spojite**
// damping_[128] (per-string damping koeficient v [0, 1]). Sustain pedal NENI
// on/off prah — je to spojity parametr, ktery se promita do hlasitosti
// doznivani i rezonance (half-pedal).
//
// Vzorec per strunu N:
//   damping_[N] = 1.0           pokud N je drzena (held)
//   damping_[N] = cc64_ / 127.0 pokud N neni drzena
//
// Resonance engine pouzije damping_[N] jako multiplikator excitacniho gainu:
//   excite = (vel/127) × harm × strength × damping_[N]
//
// API je jednovlaknove — vsechna volani z audio threadu pri drainu MidiQueue.
// GUI/MIDI thread posila zmeny VYHRADNE pres MidiQueue (jako noteOn/noteOff).

#include <bitset>
#include <cstdint>

namespace ithaca {

// Prah CC64 podle MIDI konvence (>=64 = pedal dolu); slouzi jen pro helper
// `isPedalDown()` a release-time scaling (5.4). Damping je VZDY spojite.
constexpr uint8_t kPedalDownThreshold = 64;

// Epsilon prah pro `isUndamped()` eligibility check — pod nim je rezonance
// tak slaba, ze ji povazujeme za prakticky ztlumenou.
constexpr float kDampingEpsilon = 0.001f;

class PedalState {
public:
    PedalState() { recompute(); }

    // Update z MidiEvent::Sustain (audio thread, drain MidiQueue).
    void setSustainCC(uint8_t cc);
    // Bookkeeping z note-on/off (audio thread, drain MidiQueue).
    void noteOn(int midi);
    void noteOff(int midi);
    void allNotesOff();

    // -- Read API (audio thread, volane resonance_engine + Engine release scaling) --

    // Per-string damping koeficient [0, 1]. 1.0 = struna zni volne, 0.0 = ztlumena.
    float dampingFor(int midi) const;

    // Rychly bool: damping > epsilon (= prakticky undamped).
    bool isUndamped(int midi) const { return dampingFor(midi) > kDampingEpsilon; }

    // Drzena klavesa?
    bool isHeld(int midi) const { return midi >= 0 && midi < 128 && held_[(size_t)midi]; }

    // Helpery pro continuous release-time scaling a UI (5.4 spec).
    bool   isPedalDown() const { return cc64_ >= kPedalDownThreshold; }
    uint8_t sustainCC()  const { return cc64_; }

private:
    // Recompute damping_ podle (cc64_, held_).
    void recompute();

    uint8_t          cc64_ = 0;
    std::bitset<128> held_;        // klavesa drzena (note-on bez note-off)
    float            damping_[128] = {};  // per-string damping [0..1]
};

} // namespace ithaca
