#pragma once
// engine/voice/voice.h
// --------------------
// Jeden prehravany hlas. Hraje jeden SampleAsset pres jeho prvni mic
// perspektivu (mics[0]); multi-mic mix je faze 6. Pozici drzi v double kvuli
// pitch/SR konverzi (linearni interpolace). Envelope: onset ramp (anti-click)
// + release ramp. Pan z MIDI noty (keyboard spread). Damping buffer umoznuje
// click-free retrigger/kradez. Adaptovano z icr sampler_core.

#include "sample/sample_types.h"
#include "stream/streamed_reader.h"

#include <cstdint>

namespace ithaca {

class StreamEngine;

// Konstanty hlasu.
constexpr float  kOnsetMs        = 3.f;   // nabeh proti lupnuti pri note-on
constexpr float  kDampingMs      = 21.f;  // crossfade pri retriggeru/kradezi
constexpr int    kDampMaxFrames  = 2048;  // strop damping bufferu (frames)
constexpr float  kUnderrunFadeMs = 5.f;   // rychly fade pri underrunu

class Voice {
public:
    // Pripoji StreamEngine, ze ktereho si pri startu Streamed samplu Voice
    // vezme ring slot. nullptr = streaming nedostupny (Voice se pak chova
    // jako pred fazi 4: po prekroceni preload_head jen utichne).
    void setStreamEngine(StreamEngine* se) { stream_ = se; }

    // Spusti hlas. asset musi mit aspon 1 mic. pan_l/pan_r jiz spocteny.
    void start(const SampleAsset* asset, double pitch_ratio, float vel_gain,
               float pan_l, float pan_r, float engine_sr);

    // Pripravi damping crossfade ze SOUCASNEHO stavu (pred prepsanim novym
    // tonem) — zabrani lupnuti pri retriggeru/kradezi.
    void prepareDamp(float engine_sr);

    // Spusti release ramp (note-off).
    void release(float release_ms, float engine_sr);

    // Tvrde zastav hlas BEZ cteni sample pameti (na rozdil od prepareDamp).
    // Pouziti: reloadBank uvolnuje bank_, Voice nesmi sahnout na mic_->preload_*.
    void hardStop() noexcept;

    // Note-off s pedalem v sustain pozici: NE-spustime release ramp ted, jen
    // si poznacime ze "klavesa pustena, ale pedal drzi strunu". Sample hraje
    // dal prirozene (jako kdyby nebyl note-off). Az pedal pustis, VoicePool
    // vola release() na vsech `pendingRelease()` hlasech.
    void markPendingRelease() { pending_release_ = true; }
    bool isPendingRelease() const { return pending_release_; }
    void clearPendingRelease() { pending_release_ = false; }

    // Renderuj n_samples additivne do out_l/out_r. Vrati true kdyz stale aktivni.
    bool process(float* out_l, float* out_r, int n_samples) noexcept;

    // Pripoji damp buffer (2*kDampMaxFrames floatu) vlastneny VoicePool.
    // Vola pool pri konstrukci; bez bufferu prepareDamp damp preskoci.
    void setDampBuffer(float* p) { damp_buf_ = p; }

    bool  active()    const { return active_; }
    // True dokud dohrava damping crossfade po prepareDamp (hlas uz je !active,
    // ale ocas MUSI doznit — VoicePool::processBlock ho proto stale zpracovava).
    bool  isDamping() const { return damping_; }
    bool  releasing() const { return releasing_; }
    int   midi()      const { return midi_; }
    void  setMidi(int m)    { midi_ = m; }
    // Aktualni hlasitost (pro kradez nejtissiho hlasu).
    float currentLevel() const noexcept;

private:
    const SampleAsset* asset_ = nullptr;
    const MicLayer*    mic_   = nullptr;     // = &asset_->mics[0]

    bool   active_           = false;
    bool   releasing_        = false;
    bool   in_onset_         = false;
    bool   pending_release_  = false;  // note-off prisel, ale pedal sustainuje
    int    midi_             = -1;

    double position_  = 0.0;                 // frame pozice (fractional)
    double pos_inc_   = 1.0;                 // pitch_ratio * (sample_sr/engine_sr)

    float  vel_gain_  = 1.f;
    float  onset_gain_ = 0.f, onset_step_ = 0.f;
    float  rel_gain_   = 1.f, rel_step_   = 0.f;
    float  pan_l_ = 0.707f, pan_r_ = 0.707f;

    // Damping crossfade buffer (interleaved stereo, 2*kDampMaxFrames floatu).
    // Pamet vlastni VoicePool v jednom souvislem poolu — sizeof(Voice) tak
    // klesl z ~16,5 kB na ~stovky B a skeny poolu (processBlock/findSlot/
    // citace) nekrokuji pres cache po 16 kB.
    float* damp_buf_ = nullptr;
    int    damp_len_ = 0, damp_pos_ = 0;
    bool   damping_  = false;

    // -- Faze 4: streaming z disku --
    StreamEngine* stream_  = nullptr;   // nullptr = streaming nedostupny
    // Ring + lo/hi interpolacni okno + refill heuristika ziji ve sdilenem
    // StreamedSampleReader (kompozice, sdileno s ResonanceVoice). Voice drzi
    // jen POLICY pri prazdnem ringu: cisty EOF (cleanEnd → zero + 5ms fade,
    // deaktivace) vs skutecny underrun (drzi posledni vzorek + 5ms fade).
    // Sev head->ring: seed lo = posledni head frame, hi = prvni ring pop.
    StreamedSampleReader reader_;
    // Fast underrun fade: pri prazdnem ringu pred EOF → ramp do 0 a deaktivace.
    bool     underrun_fading_  = false;
    float    underrun_gain_    = 1.f;
    float    underrun_step_    = 0.f;
};

} // namespace ithaca
