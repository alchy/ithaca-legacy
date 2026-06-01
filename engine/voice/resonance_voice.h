#pragma once
// engine/voice/resonance_voice.h
// -------------------------------
// ResonanceVoice — specializovany hlas pro sympatickou rezonanci (faze 5).
//
// Klicove odlisnosti od `Voice`:
//  - START od `mic_->resonance_start_frame` (skip-attack), NE od 0. Cte z
//    `preload_resonance` regionu (Streamed mic) nebo z `preload_head` s offsetem
//    (FullyLoaded mic, ktery cely sampl drzi v preload_head).
//  - NEMA velocity/onset/release rampu jako Voice. Misto toho ma SPOJITE
//    sledovani `target_gain_` — `gain_` se ke svemu targetu rampuje plynule
//    (~30 ms ramp standardne; `fadeOut()` rapidnejsi 5 ms).
//  - Target_gain meni:
//      * `start(...)`     — pri spusteni nastavi target_gain_ = initial_gain
//                           (gain_ zacina na 0 → fade-in ~30 ms).
//      * `addExcitation`  — relativni: target = max(target, gain + excitation).
//                           Multi-source: dalsi hrana nota take budi tuto strunu.
//      * `setTargetGain`  — ABSOLUTNI: volane per audio block z ResonanceEngine
//                           podle `last_excite * pedal.dampingFor(N)`. Tak se
//                           plynule promita zmena pedalu (vc. half-pedal).
//      * `fadeOut`        — rule B (note-on N kdyz rezonance N zni): target = 0
//                           s ostrejsi 5 ms rampou; po dosazeni 0 deaktivace.
//  - NEMA damping crossfade (rezonance je per-nota unique; retrigger neexistuje
//    — rule B reseno fadeOut + dalsi excitation projde po deaktivaci).
//
// Streaming: identicke s Voice — preload region nejdriv, pak ring (alokovany
// pri start()). Streamed mic posila prvni read request od
// `resonance_start_frame + resonance_frames` (= za preload_resonance).
// Underrun → fast fade do ticha + LOG_RT_WARN.

#include "sample/sample_types.h"

#include <cstdint>

namespace ithaca {

class StreamEngine;
struct RingHandle;

// Default cas ramp na sledovani target_gain_. Volene tak, aby zmeny pedalu
// (per-blok update z ResonanceEngine) byly slysitelne plynule a bez zipperu.
constexpr float kResonanceRampMs       = 30.f;
// Steeper ramp pri fadeOut (rule B): ostrejsi nez 30 ms ale stale bez click.
constexpr float kResonanceFadeOutMs    = 5.f;
// Pod timto prahem na target_gain_ povazujeme rezonanci za umiranou —
// nastavi se is_fading_out_ = true a po dosazeni 0 hlas se deaktivuje.
constexpr float kResonanceTargetEpsilon = 1e-4f;
// Pod timto prahem na gain_ (current) pri is_fading_out_ → deaktivace.
constexpr float kResonanceGainEpsilon   = 1e-5f;

class ResonanceVoice {
public:
    // Pripoji StreamEngine (volat PRED prvnim start()). nullptr = pro Streamed
    // mic nebude ring (hlas dohraje preload_resonance a tise dohaje).
    void setStreamEngine(StreamEngine* se) { stream_ = se; }

    // Spusti rezonancni hlas pro strunu N. `mic` musi byt platny (caller
    // si zarucil ze nota N ma sampl). `initial_gain` = target_gain_ po starte
    // (gain_ rampuje z 0 → initial_gain pres kResonanceRampMs). `engine_sr`
    // potreba pro pos_inc + ramp prepocty.
    void start(int midi, const MicLayer* mic, float initial_gain,
               float pan_l, float pan_r, float engine_sr);

    // Multi-source: pricte buzeni od dalsi hrane noty (target = max(target,
    // gain + excitation_gain)). Pouziti: ResonanceEngine vi, ze dalsi note-on M
    // budi taky struny N, na kterych uz nase rezonance zni — misto zalozeni
    // druhe rezonance jen pricte excitation k jiz znicimu hlasu.
    void addExcitation(float excitation_gain);

    // ABSOLUTNI target: volane per audio block z ResonanceEngine podle
    // `last_excite * pedal.dampingFor(N)`. Pri target_gain_ < epsilon prepne
    // is_fading_out_=true a po dosazeni 0 hlas se deaktivuje.
    void setTargetGain(float target);

    // Rule B (note-on N kdyz rezonance N hraje): rapidnejsi fade na 0.
    // Po dosazeni 0 → active_ = false.
    void fadeOut(float engine_sr);

    // Tvrde zastav (reloadBank): uvolni ring, nesahej na mic_->preload_*.
    void hardStop() noexcept;

    // Renderuj n_samples additivne do out_l/out_r. Vrati true kdyz stale aktivni.
    bool process(float* out_l, float* out_r, int n_samples) noexcept;

    bool  active()       const { return active_; }
    int   midi()         const { return midi_; }
    float currentLevel() const noexcept { return gain_; }
    // True kdyz hlas je v rezimu fade-out (rule B nebo target_gain → 0). Pouziva
    // ResonanceEngine, aby pri nove excitaci NEPRZAL fade probehajici v ramci
    // pravidla B (note-on na rezonujici notu): novou rezonanci od dalsiho
    // note-on M na N ignorujeme, dokud rule-B fade nedozni a slot se neuvolni.
    bool  fadingOut()    const noexcept { return is_fading_out_; }

private:
    const MicLayer*  mic_   = nullptr;

    bool   active_         = false;
    int    midi_           = -1;

    // Pozice je FILE-GLOBAL frame offset (NE lokalni v bufferu). To umoznuje
    // jednoduchou aritmetiku pro detekci: pred resonance_start+resonance_frames
    // (preload region) vs. za nim (ring / EOF).
    double position_       = 0.0;
    double pos_inc_        = 1.0;   // sample_sr / engine_sr (pitch_ratio = 1.0)

    // Gain a jeho target — jedina envelope ResonanceVoice. Rampuje sample-by-
    // sample step-em `gain_step_` (recalkulovan vzdycky kdyz se zmeni target
    // nebo zacne fadeOut).
    float  gain_           = 0.f;
    float  target_gain_    = 0.f;
    float  gain_step_      = 0.f;
    // Cas (v sekundach) na ramp gain_ → target_gain_ (default kResonanceRampMs).
    // Drzime ho jako "frames" pro rychlou aritmetiku ve `setTargetGain`.
    float  ramp_frames_    = 1.f;   // pocet frames na full-range ramp (recompute pri start)
    // Nastaveno true kdyz target_gain_ klesl pod epsilon → po dosazeni gain ~0
    // se hlas deaktivuje.
    bool   is_fading_out_  = false;

    float  pan_l_          = 0.707f;
    float  pan_r_          = 0.707f;

    // Pomocna kopie engine_sr (kvuli `addExcitation` / `setTargetGain` recompute
    // ramp stepu bez nutnosti predavat sr).
    float  engine_sr_      = 48000.f;

    // -- Streaming (stejne jako Voice) --
    StreamEngine* stream_       = nullptr;
    RingHandle*   ring_         = nullptr;
    int64_t       file_request_off_ = 0;
    bool          stream_pending_   = false;
    bool          underrun_fading_  = false;
    float         underrun_gain_    = 1.f;
    float         underrun_step_    = 0.f;

    // -- SR konverze ve streamovane (ring) casti (viz Voice). --
    float    ring_cur_l_   = 0.f;
    float    ring_cur_r_   = 0.f;
    int64_t  ring_cur_idx_ = -1;

    // Recompute `gain_step_` z (gain_, target_gain_, ramp_frames_).
    // Pri is_fading_out_ se nepocita znovu (uz nastaveny ostry step z fadeOut).
    void recomputeGainStep() noexcept;
};

} // namespace ithaca
