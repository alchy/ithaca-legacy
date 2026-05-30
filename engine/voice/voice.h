#pragma once
// engine/voice/voice.h
// --------------------
// Jeden prehravany hlas. Hraje jeden SampleAsset pres jeho prvni mic
// perspektivu (mics[0]); multi-mic mix je faze 6. Pozici drzi v double kvuli
// pitch/SR konverzi (linearni interpolace). Envelope: onset ramp (anti-click)
// + release ramp. Pan z MIDI noty (keyboard spread). Damping buffer umoznuje
// click-free retrigger/kradez. Adaptovano z icr sampler_core.

#include "sample/sample_types.h"

#include <cstdint>

namespace ithaca {

// Konstanty hlasu.
constexpr float  kOnsetMs      = 3.f;     // nabeh proti lupnuti pri note-on
constexpr float  kDampingMs    = 21.f;    // crossfade pri retriggeru/kradezi
constexpr int    kDampMaxFrames = 2048;   // strop damping bufferu (frames)

class Voice {
public:
    // Spusti hlas. asset musi mit aspon 1 mic. pan_l/pan_r jiz spocteny.
    void start(const SampleAsset* asset, double pitch_ratio, float vel_gain,
               float pan_l, float pan_r, float engine_sr);

    // Pripravi damping crossfade ze SOUCASNEHO stavu (pred prepsanim novym
    // tonem) — zabrani lupnuti pri retriggeru/kradezi.
    void prepareDamp(float engine_sr);

    // Spusti release ramp (note-off).
    void release(float release_ms, float engine_sr);

    // Renderuj n_samples additivne do out_l/out_r. Vrati true kdyz stale aktivni.
    bool process(float* out_l, float* out_r, int n_samples) noexcept;

    bool  active()    const { return active_; }
    bool  releasing() const { return releasing_; }
    int   midi()      const { return midi_; }
    void  setMidi(int m)    { midi_ = m; }
    // Aktualni hlasitost (pro kradez nejtissiho hlasu).
    float currentLevel() const noexcept;

private:
    const SampleAsset* asset_ = nullptr;
    const MicLayer*    mic_   = nullptr;     // = &asset_->mics[0]

    bool   active_    = false;
    bool   releasing_ = false;
    bool   in_onset_  = false;
    int    midi_      = -1;

    double position_  = 0.0;                 // frame pozice (fractional)
    double pos_inc_   = 1.0;                 // pitch_ratio * (sample_sr/engine_sr)

    float  vel_gain_  = 1.f;
    float  onset_gain_ = 0.f, onset_step_ = 0.f;
    float  rel_gain_   = 1.f, rel_step_   = 0.f;
    float  pan_l_ = 0.707f, pan_r_ = 0.707f;

    // Damping crossfade buffer (interleaved stereo).
    float  damp_buf_[2 * kDampMaxFrames] = {};
    int    damp_len_ = 0, damp_pos_ = 0;
    bool   damping_  = false;
};

} // namespace ithaca
