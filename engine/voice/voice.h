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

class StreamEngine;
struct RingHandle;

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

    bool  active()    const { return active_; }
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

    // Damping crossfade buffer (interleaved stereo).
    float  damp_buf_[2 * kDampMaxFrames] = {};
    int    damp_len_ = 0, damp_pos_ = 0;
    bool   damping_  = false;

    // -- Faze 4: streaming z disku --
    StreamEngine* stream_  = nullptr;   // nullptr = streaming nedostupny
    RingHandle*   ring_    = nullptr;   // alokovano pri start() jen pro Streamed
    // Aktualni file offset (v stereo frames), ze ktereho jsme posledne POZADALI
    // worker, aby cetl. Init = head_frames pri prvnim requestu; navysuje se
    // o size kazdeho dalsiho requestu, aby na sebe casti navazaly.
    int64_t  file_request_off_ = 0;
    // Pendujici flag: zabrani spamovani requestu kazdy audio blok.
    // Voice si ho castuje sam (po push request → true; ringuv naskok ho zase
    // shodi). FUTURE: idealnejsi je dedicated atomic na ring, kterou worker
    // shodi po dokonceni. Pro prvni iteraci stacne flag voice-local + heuristika
    // "kdyz ring uz neni pod prahem, mam volno na novy request".
    bool     stream_pending_   = false;
    // Fast underrun fade: pri prazdnem ringu pred EOF → ramp do 0 a deaktivace.
    bool     underrun_fading_  = false;
    float    underrun_gain_    = 1.f;
    float    underrun_step_    = 0.f;

    // -- SR konverze ve streamovane (ring) casti: lo/hi sliding window --
    // Pro lin. interpolaci drzime DVA sousedni framy: ring_lo_* na indexu
    // floor(position_) a ring_hi_* na floor(position_)+1 (lookahead). Vystup =
    // lo*(1-frac) + hi*frac. Okno se posouva popovanim z ringu dokud lo
    // nedosahne floor(position_). Seed pri prvnim vstupu: lo = posledni head
    // frame, hi = prvni ring pop (plynuly sev head->ring). Pri EOF clamp hi=lo
    // (hold last sample). Na 48k (pos_inc=1) je frac=0 → vystup = puvodni vzorek.
    float    ring_lo_l_   = 0.f;
    float    ring_lo_r_   = 0.f;
    float    ring_hi_l_   = 0.f;
    float    ring_hi_r_   = 0.f;
    int64_t  ring_lo_idx_ = -1;   // -1 = neseedovano
};

} // namespace ithaca
