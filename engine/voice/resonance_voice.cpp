// engine/voice/resonance_voice.cpp — viz resonance_voice.h.
//
// Adaptovano z Voice::process; sdileny vzor pro cteni z preload + ringu.
// Hlavni rozdily: jiny startovy offset (resonance_start_frame misto 0),
// zdroj cteni `preload_resonance` (Streamed) nebo `preload_head` s offsetem
// (FullyLoaded), a jedina envelope = `gain_` sledujici `target_gain_`.
#include "voice/resonance_voice.h"

#include "stream/stream_engine.h"
#include "util/log.h"
#include "voice/voice.h"   // sdilime kUnderrunFadeMs (stejny fade pri ring underrunu)

#include <algorithm>

namespace ithaca {

void ResonanceVoice::start(int midi, const MicLayer* mic, float initial_gain,
                           float pan_l, float pan_r, float engine_sr, bool use_cache) {
    use_cache_ = use_cache;
    // Kdybychom drzeli ring z minule, vrat ho. (V realnem provozu se ResonanceVoice
    // slot recykluje az po deaktivaci, kdy ring uz byl uvolnen v process();
    // ale defensive cleanup pro pripady ze caller re-start aktivni slot.)
    if (ring_ && stream_) {
        stream_->releaseRing(ring_);
        ring_ = nullptr;
    }
    stream_pending_   = false;
    underrun_fading_  = false;
    underrun_gain_    = 1.f;
    underrun_step_    = 0.f;
    file_request_off_ = 0;
    ring_lo_l_ = 0.f; ring_lo_r_ = 0.f;
    ring_hi_l_ = 0.f; ring_hi_r_ = 0.f;
    ring_lo_idx_ = -1;

    midi_       = midi;
    mic_        = mic;
    pan_l_      = pan_l;
    pan_r_      = pan_r;
    engine_sr_  = engine_sr;

    // Skip-attack: zacni cist od resonance_start_frame (FILE-GLOBAL offset).
    // U FullyLoaded mic je to pozice v preload_head; u Streamed mic je to
    // zacatek preload_resonance bufferu (lokalni idx = position - resonance_start_frame).
    position_ = mic_ ? (double)mic_->resonance_start_frame : 0.0;

    // Pitch_ratio = 1.0 vzdy (rezonance neni transponovana). SR konverze
    // zustava pripadu kdy se sample_sr != engine_sr.
    double sample_sr = mic_ ? (double)mic_->file.sample_rate : (double)engine_sr;
    pos_inc_ = sample_sr / (double)engine_sr;

    // Gain ramp: zacina na 0, target = initial_gain → fade-in pres kResonanceRampMs.
    gain_        = 0.f;
    target_gain_ = initial_gain;
    ramp_frames_ = (std::max)(1.f, kResonanceRampMs * 0.001f * engine_sr);
    is_fading_out_ = false;
    recomputeGainStep();

    underrun_step_ = -1.f / (kUnderrunFadeMs * 0.001f * engine_sr);

    // Validni stav?
    active_ = (mic_ != nullptr) &&
              (mic_->mode == MicLayerMode::FullyLoaded
                   ? mic_->head_frames > mic_->resonance_start_frame
                   : (mic_->resonance_frames > 0 || mic_->head_frames > mic_->resonance_start_frame));

    // Streamed → alokuj ring a zazadat o data za preload_resonance regionem.
    if (active_ && mic_->mode == MicLayerMode::Streamed && stream_) {
        ring_ = stream_->acquireRing();
        if (ring_) {
            const int64_t cap         = (int64_t)ring_->capacity_frames;
            const int     eff_res_frames = use_cache_ ? mic_->resonance_frames : 0;
            const int64_t res_end     = (int64_t)mic_->resonance_start_frame
                                      + (int64_t)eff_res_frames;
            const int64_t total_after = (int64_t)mic_->file.frames - res_end;
            // Pokud uz neni co streamovat (resonance_window pokryval konec souboru),
            // tak ring stejne mame, jen ho oznacime jako EOF a request neposleme.
            if (total_after <= 0) {
                ring_->eof_.store(true, std::memory_order_release);
                file_request_off_ = res_end;
            } else {
                const int64_t want    = (cap < total_after) ? cap : total_after;
                const bool    eof_done = (want >= total_after);
                if (stream_->requestRead(ring_, mic_->file.path,
                                         res_end, want, eof_done)) {
                    file_request_off_ = res_end + want;
                    stream_pending_   = true;
                } else {
                    // Fronta plna: offset neposouvat, refill v process() zopakuje.
                    file_request_off_ = res_end;
                }
            }
        }
        // Ring pool plny → ResonanceVoice doplyne preload_resonance a tise utichne
        // (zadny crash; rezonance je luxus, nikoliv must-have hlavnho hlasu).
    }

    // DEBUG: kazdy resonance voice start logujeme s timestamp + midi + initial_gain.
    // Pomaha izolovat bug "rezonance firi i bez pedalu" — sledujeme presne, ktere
    // tony vybudi rezonanci, s jakou silou a kdy. Pokud je init_gain >> 0 pri
    // cc64=0, je to bug eligibility filtru / damping mapping.
    // RT-safe (start bezi na audio threadu) — LOG_RT do lock-free ringu.
    LOG_RT_DEBUG("resonance_voice",
        "START midi=%d init_gain=%.4f mic_mode=%s pos=%d",
        midi, initial_gain,
        (mic_->mode == MicLayerMode::Streamed) ? "Streamed" : "FullyLoaded",
        mic_->resonance_start_frame);
}

void ResonanceVoice::addExcitation(float excitation_gain) {
    if (!active_ || excitation_gain <= 0.f) return;
    // Relativni: target = max(target, gain + excitation). Tj. dalsi note-on
    // muze jen ZVYSIT target. Pokud uz rezonance zni hlasitejs (gain > target+exc),
    // nehybeme.
    const float new_target = gain_ + excitation_gain;
    if (new_target > target_gain_) {
        target_gain_ = new_target;
        // Nova excitation = zruseni fade-out (rezonance znovu nabuzena).
        is_fading_out_ = false;
        recomputeGainStep();
    }
}

void ResonanceVoice::setTargetGain(float target) {
    if (!active_) return;
    if (target < 0.f) target = 0.f;
    if (target <= kResonanceTargetEpsilon) {
        // Pod prahem cilime na PRAVOU nulu (ne na zbytkovou hodnotu). Jinak by se
        // gain_ ustalil mezi kResonanceGainEpsilon (1e-5) a kResonanceTargetEpsilon
        // (1e-4) — oznaceny jako fading-out, ale nad deaktivacnim prahem → hlas by
        // se NIKDY nedeaktivoval (stuck voice / leak). Fade na 0 → deaktivace.
        target_gain_   = 0.f;
        is_fading_out_ = true;
    } else {
        target_gain_   = target;
        is_fading_out_ = false;
    }
    recomputeGainStep();
}

void ResonanceVoice::fadeOut(float engine_sr) {
    if (!active_) return;
    target_gain_   = 0.f;
    is_fading_out_ = true;
    // Strmejsi rampu: -gain / fade_frames (ostry ramp ze SOUCASNE gain_ → 0).
    const float fade_frames = (std::max)(1.f, kResonanceFadeOutMs * 0.001f * engine_sr);
    gain_step_ = -gain_ / fade_frames;
    if (gain_step_ > 0.f) gain_step_ = 0.f;  // bezpecnost: nikdy nesmi rostout pri fadeOut
}

void ResonanceVoice::hardStop() noexcept {
    if (ring_ && stream_) { stream_->releaseRing(ring_); ring_ = nullptr; }
    stream_pending_  = false;
    underrun_fading_ = false;
    active_          = false;
    is_fading_out_   = false;
    gain_ = 0.f; target_gain_ = 0.f; gain_step_ = 0.f;
    mic_  = nullptr;
    ring_lo_idx_ = -1;
}

void ResonanceVoice::recomputeGainStep() noexcept {
    // Step na linearni ramp z gain_ → target_gain_ pres ramp_frames_.
    // Pouziva se v `process` per-sample: gain_ += gain_step_; clamp na cil.
    gain_step_ = (target_gain_ - gain_) / ramp_frames_;
}

bool ResonanceVoice::process(float* out_l, float* out_r, int n_samples) noexcept {
    if (!active_ || !mic_) {
        if (ring_ && stream_) { stream_->releaseRing(ring_); ring_ = nullptr; }
        active_ = false;
        return false;
    }
    const float*  head_data    = mic_->preload_head.data();
    const int     head_frames  = mic_->head_frames;
    const int     total_frames = mic_->file.frames;
    const float*  res_data     = use_cache_ ? mic_->preload_resonance.data() : nullptr;
    const int     res_start    = mic_->resonance_start_frame;
    const int     res_frames   = use_cache_ ? mic_->resonance_frames : 0;
    const int64_t res_end      = (int64_t)res_start + (int64_t)res_frames;
    const bool    streamed     = (mic_->mode == MicLayerMode::Streamed);

    bool produced = false;

    for (int i = 0; i < n_samples; ++i) {
        int p0 = (int)position_;
        float sL = 0.f, sR = 0.f;

        if (streamed) {
            // Streamed: nejdrive cti z preload_resonance (lokalni idx =
            // position - res_start), pak z ringu (za res_end).
            if (p0 < res_start) {
                // Pod resonance_start (nemelo by se stavat — start nas nastavil
                // primo na res_start). Defensive: tichy vzorek + posun.
                sL = 0.f; sR = 0.f;
                position_ += pos_inc_;
            } else if (p0 < res_end - 1 && res_frames > 1) {
                // V preload_resonance regionu (RAM): lin. interpolace mezi
                // local a local+1. Posledni frame regionu spada do ring vetve
                // (sev preload->ring).
                int   local = p0 - res_start;
                float frac  = (float)(position_ - (double)p0);
                sL = res_data[local * 2]     * (1.f - frac) + res_data[(local + 1) * 2]     * frac;
                sR = res_data[local * 2 + 1] * (1.f - frac) + res_data[(local + 1) * 2 + 1] * frac;
                position_ += pos_inc_;
            } else if (ring_) {
                // Streamed: lin. interpolace pres lo/hi okno. Seed lo = posledni
                // preload_resonance frame, hi = prvni ring pop → plynuly sev.
                if (ring_lo_idx_ < 0) {
                    ring_lo_idx_ = res_end - 1;
                    if (res_frames > 0) {
                        ring_lo_l_ = res_data[(size_t)(res_frames - 1) * 2];
                        ring_lo_r_ = res_data[(size_t)(res_frames - 1) * 2 + 1];
                    }
                    float L, R;
                    if (ring_->popFrame(L, R)) { ring_hi_l_ = L; ring_hi_r_ = R; }
                    else { ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_; }
                }
                const int64_t target = (int64_t)position_;
                bool underrun = false;
                while (ring_lo_idx_ < target) {
                    ring_lo_l_ = ring_hi_l_; ring_lo_r_ = ring_hi_r_;
                    float L, R;
                    if (ring_->popFrame(L, R)) {
                        ring_hi_l_ = L; ring_hi_r_ = R;
                    } else if (ring_->eof_.load(std::memory_order_acquire)) {
                        ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_;
                        ring_lo_idx_++;
                        if (ring_lo_idx_ >= (int64_t)total_frames - 1) {
                            active_ = false;
                        }
                        break;
                    } else {
                        underrun = true;
                        break;
                    }
                    ring_lo_idx_++;
                }
                if (underrun) {
                    if (!underrun_fading_) {
                        underrun_fading_ = true;
                        if (stream_) stream_->noteUnderrun();
                        underrun_gain_   = 1.f;
                        LOG_RT_WARN("resonance_voice", "underrun midi=%d", midi_);
                    }
                    // Drz posledni znamy vzorek — underrun rampa ho fadne k 0
                    // (nuly by fade obesly = tvrdy strih).
                    sL = ring_lo_l_; sR = ring_lo_r_;
                } else {
                    float frac = (float)(position_ - (double)ring_lo_idx_);
                    if (frac < 0.f) frac = 0.f;
                    if (frac > 1.f) frac = 1.f;
                    sL = ring_lo_l_ * (1.f - frac) + ring_hi_l_ * frac;
                    sR = ring_lo_r_ * (1.f - frac) + ring_hi_r_ * frac;
                }
                position_ += pos_inc_;
            } else {
                // Streamed bez ringu (acquireRing fail nebo preload pokryval cely zbytek)
                // a vyhrali jsme preload_resonance — konec hlasu.
                active_ = false;
                break;
            }
        } else {
            // FullyLoaded: cely sampl je v preload_head. Cteme od `position_`
            // (jiz inicializovano na res_start), pokracujeme dokud nedosahneme
            // konce hlavy. preload_resonance je v tomto rezimu prazdny.
            if (p0 < head_frames - 1) {
                // Linearni interpolace (jako Voice v head regionu).
                float frac = (float)(position_ - (double)p0);
                int   p1   = p0 + 1;
                sL = head_data[p0 * 2]     * (1.f - frac) + head_data[p1 * 2]     * frac;
                sR = head_data[p0 * 2 + 1] * (1.f - frac) + head_data[p1 * 2 + 1] * frac;
                position_ += pos_inc_;
            } else if (p0 < head_frames) {
                // Posledni vzorek hlavy.
                sL = head_data[p0 * 2];
                sR = head_data[p0 * 2 + 1];
                position_ += pos_inc_;
            } else {
                // Dosli jsme za hlavu. Pro FullyLoaded je to konec — sampl byl
                // cely v preload_head. V praxi se nemelo stat, protoze
                // ResonanceEngine snizuje last_excite → target_gain_ → 0 dlouho
                // pred koncem souboru. Pripadne tichy konec.
                active_ = false;
                break;
            }
        }

        // Hard guard na konec souboru (kdyby pos jsel mimo file.frames i v ringu).
        if (streamed && ring_ && (int)position_ >= total_frames &&
            ring_->eof_.load(std::memory_order_acquire) &&
            ring_->available() == 0) {
            active_ = false;
        }

        // Smooth gain ramp na target_gain_. Single envelope, bez onset/release.
        // Step recompute se deje pri zmene targetu (setTargetGain / addExcitation /
        // fadeOut); zde jen aplikujeme.
        gain_ += gain_step_;
        // Clamp k cilu (pri klesajicim stepu i rostoucim).
        if ((gain_step_ > 0.f && gain_ > target_gain_) ||
            (gain_step_ < 0.f && gain_ < target_gain_)) {
            gain_ = target_gain_;
            gain_step_ = 0.f;
        }
        if (gain_ < 0.f) gain_ = 0.f;  // bezpecnost (napr. fadeOut overshoot)

        // Underrun fast fade (multiplikuje pres gain_).
        float env = gain_;
        if (underrun_fading_) {
            env *= underrun_gain_;
            underrun_gain_ += underrun_step_;
            if (underrun_gain_ <= 0.f) {
                underrun_gain_ = 0.f;
                active_ = false;
            }
        }

        out_l[i] += sL * env * pan_l_;
        out_r[i] += sR * env * pan_r_;
        produced = true;

        // Deaktivace pri dosazeni 0 v fade-out modu.
        if (is_fading_out_ && gain_ <= kResonanceGainEpsilon) {
            gain_   = 0.f;
            active_ = false;
            break;
        }

        if (!active_) break;
    }

    // Refill heuristika (Streamed). Stejny vzor jako Voice — kdyz avail klesne
    // pod prah, pozadat o dalsi data.
    if (streamed && ring_ && stream_ && active_) {
        const int avail = ring_->available();
        const int thr   = stream_->refillThresholdFrames();
        const int half_cap = ring_->capacity_frames / 2;
        if (stream_pending_ && avail >= half_cap) {
            stream_pending_ = false;
        }
        if (!stream_pending_ && avail < thr) {
            const int64_t remain = (int64_t)total_frames - file_request_off_;
            if (remain > 0) {
                int64_t want = (int64_t)(ring_->capacity_frames - avail);
                if (want > remain) want = remain;
                const bool eof_done = (file_request_off_ + want >= (int64_t)total_frames);
                if (stream_->requestRead(ring_, mic_->file.path,
                                         file_request_off_, want, eof_done)) {
                    file_request_off_ += want;
                    stream_pending_    = true;
                }
                // false → drop; zadny posun offsetu (retry pristi blok).
            }
        }
    }

    // Pri deaktivaci uvolni ring.
    if (!active_ && ring_ && stream_) {
        stream_->releaseRing(ring_);
        ring_ = nullptr;
    }

    return active_ || produced;
}

} // namespace ithaca
