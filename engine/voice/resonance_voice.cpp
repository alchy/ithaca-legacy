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
    reader_.release(stream_);
    underrun_fading_  = false;
    underrun_gain_    = 1.f;
    underrun_step_    = 0.f;

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

    // Validni stav? Cache mod cte preload_resonance (resonance_frames);
    // stream mod streamuje od resonance_start_frame → rozhoduje file.frames.
    // Behem cache rebuilu (use_cache=false) se resonance_frames NESMI cist —
    // bg thread ho prave prepisuje (review E-nizka).
    const int eff_res_frames0 = (mic_ && use_cache_) ? mic_->resonance_frames : 0;
    active_ = (mic_ != nullptr) &&
              (mic_->mode == MicLayerMode::FullyLoaded
                   ? mic_->head_frames > mic_->resonance_start_frame
                   : (eff_res_frames0 > 0 ||
                      (int64_t)mic_->file.frames
                          > (int64_t)mic_->resonance_start_frame));

    // Streamed → alokuj ring a zazadat o data za preload_resonance regionem
    // (reader: acquire + request s no-advance-on-drop). Pokud uz neni co
    // streamovat (resonance_window pokryval konec souboru), ring se jen
    // oznaci EOF a request se neposle. Ring pool plny → ResonanceVoice
    // doplyne preload_resonance a tise utichne (zadny crash; rezonance je
    // luxus, nikoliv must-have hlavniho hlasu).
    if (active_ && mic_->mode == MicLayerMode::Streamed && stream_) {
        const int     eff_res_frames = use_cache_ ? mic_->resonance_frames : 0;
        const int64_t res_end     = (int64_t)mic_->resonance_start_frame
                                  + (int64_t)eff_res_frames;
        const int64_t total_after = (int64_t)mic_->file.frames - res_end;
        if (total_after <= 0) {
            (void)reader_.beginEofOnly(stream_, res_end);
        } else {
            (void)reader_.begin(stream_, mic_->file.path, res_end,
                                (int64_t)mic_->file.frames);
        }
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
    reader_.release(stream_);
    underrun_fading_ = false;
    active_          = false;
    is_fading_out_   = false;
    gain_ = 0.f; target_gain_ = 0.f; gain_step_ = 0.f;
    mic_  = nullptr;
}

void ResonanceVoice::recomputeGainStep() noexcept {
    // Step na linearni ramp z gain_ → target_gain_ pres ramp_frames_.
    // Pouziva se v `process` per-sample: gain_ += gain_step_; clamp na cil.
    gain_step_ = (target_gain_ - gain_) / ramp_frames_;
}

bool ResonanceVoice::process(float* out_l, float* out_r, int n_samples) noexcept {
    if (!active_ || !mic_) {
        reader_.release(stream_);
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
    // Bulk rezim ringu: 1 acquire + 1 release za blok misto 3 atomik/vzorek.
    if (reader_.hasRing()) reader_.beginBlock();

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
            } else if (reader_.hasRing()) {
                // Streamed: lin. interpolace pres lo/hi okno readeru. Seed lo
                // = posledni preload_resonance frame, hi = prvni ring pop →
                // plynuly sev. (res_frames==0: seedovana nula se zahodi driv,
                // nez se pouzije — overeno trasovanim indexu.)
                if (!reader_.seeded()) {
                    const float lo_l = (res_frames > 0)
                        ? res_data[(size_t)(res_frames - 1) * 2] : 0.f;
                    const float lo_r = (res_frames > 0)
                        ? res_data[(size_t)(res_frames - 1) * 2 + 1] : 0.f;
                    reader_.seed(lo_l, lo_r, res_end - 1);
                }
                const int64_t target = (int64_t)position_;
                bool underrun = false;
                // EOF-hold policy: pri prazdnem ringu s EOF drz posledni
                // vzorek (hi=lo) a dojed k deaktivaci u konce souboru;
                // bez EOF jde o skutecny underrun.
                while (reader_.loIdx() < target) {
                    const auto adv = reader_.advance(target);
                    if (adv == StreamedSampleReader::Advance::Reached) break;
                    if (reader_.eofAcquire()) {
                        reader_.holdHiFromLo();
                        reader_.bumpLoIdx();
                        if (reader_.loIdx() >= (int64_t)total_frames - 1) {
                            active_ = false;
                        }
                        break;
                    } else {
                        underrun = true;
                        break;
                    }
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
                    sL = reader_.loL(); sR = reader_.loR();
                } else {
                    float frac = (float)(position_ - (double)reader_.loIdx());
                    if (frac < 0.f) frac = 0.f;
                    if (frac > 1.f) frac = 1.f;
                    sL = reader_.loL() * (1.f - frac) + reader_.hiL() * frac;
                    sR = reader_.loR() * (1.f - frac) + reader_.hiR() * frac;
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
        if (streamed && reader_.hasRing() && (int)position_ >= total_frames &&
            reader_.eofAcquire() && reader_.ringAvailable() == 0) {
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

    reader_.endBlock();   // commit r_ pred refill (ten cte available z atomik)

    // Refill heuristika (Streamed) zije v readeru — stejny vzor jako Voice.
    if (streamed && reader_.hasRing() && stream_ && active_) {
        reader_.refill(stream_, mic_->file.path);
    }

    // Pri deaktivaci uvolni ring.
    if (!active_ && reader_.hasRing() && stream_) {
        reader_.release(stream_);
    }

    return active_ || produced;
}

} // namespace ithaca
