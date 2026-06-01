// engine/voice/voice.cpp — viz voice.h. Adaptovano z icr sampler_core voice loop.
#include "voice/voice.h"

#include "stream/stream_engine.h"
#include "util/log.h"

#include <algorithm>

namespace ithaca {

void Voice::prepareDamp(float engine_sr) {
    // KRITICKE: cleanup (ring release + active=false) MUSI probehnout VZDY kdyz
    // je hlas aktivni. Drive prepareDamp delal 3 early-return PRED cleanupem
    // → pro streamovane hlasy past head se NIC nestalo → voice zustal active
    // s ringem → retrigger spawnuje novy voice → 2 voicy pro stejnou midi +
    // ring leak. Pri opakovanem retriggeru (rozklad akordu pod pedalem) se
    // ring pool (32) vycerpa a nove hlasy dostanou ring=no → hraji jen
    // preload-head a umiraji fullyloaded_past_head.
    //
    // Damping crossfade (fade-out kopie z hlavy samplu) zustava best-effort —
    // funguje jen kdyz hlas je jeste v head regionu. Pro streamovane voicy
    // past head je damp_buf_ prazdny (1ms click pri retriggeru, akceptovatelne;
    // FUTURE: damping z ring bufferu).

    // 1) Best-effort fade-out kopie z hlavy samplu do damp_buf_.
    damping_  = false;
    damp_len_ = 0;
    damp_pos_ = 0;
    if (active_ && mic_) {
        const int pos = (int)position_;
        if (pos < mic_->head_frames) {
            int damp_frames = (std::min)((int)(kDampingMs * 0.001f * engine_sr),
                                         kDampMaxFrames);
            int avail = (std::min)(damp_frames, mic_->head_frames - pos);
            if (avail > 0) {
                const float* src = mic_->preload_head.data() + (size_t)pos * 2;
                float env = vel_gain_;
                if (releasing_) env *= rel_gain_;
                float step = 1.f / (float)avail;
                for (int i = 0; i < avail; ++i) {
                    float fade = 1.f - (float)i * step;
                    damp_buf_[i * 2]     = src[i * 2]     * env * fade * pan_l_;
                    damp_buf_[i * 2 + 1] = src[i * 2 + 1] * env * fade * pan_r_;
                }
                damp_len_ = avail;
                damping_  = true;
            }
        }
    }

    // 2) VZDY cleanup ring + state (i kdyz damp_buf_ nebyl naplnen).
    if (ring_ && stream_) {
        stream_->releaseRing(ring_);
        ring_ = nullptr;
    }
    stream_pending_  = false;
    // DEBUG: zaloguj deaktivaci v dusledku retrigger/steal damping.
    log::Logger::default_().log("voice_end", log::Severity::Info,
        "DEACTIVATE midi=%d reason=damped_for_retrigger_or_steal damp_len=%d",
        midi_, damp_len_);
    active_          = false;   // pool ho ted muze rovnou ukrast / pouzit znovu
    releasing_       = false;
    pending_release_ = false;
}

void Voice::hardStop() noexcept {
    if (ring_ && stream_) { stream_->releaseRing(ring_); ring_ = nullptr; }
    stream_pending_  = false;
    underrun_fading_ = false;
    active_          = false;
    releasing_       = false;
    pending_release_ = false;
    damping_         = false;
    damp_len_ = 0; damp_pos_ = 0;
    asset_ = nullptr;
    mic_   = nullptr;
    ring_cur_idx_ = -1;
}

void Voice::start(const SampleAsset* asset, double pitch_ratio, float vel_gain,
                  float pan_l, float pan_r, float engine_sr) {
    // Pred prepsanim stavu: kdyby jsme drzeli ring z minule, vrat ho do poolu.
    // (V realnem provozu prepareDamp + start jsou volane na rovne tom samem
    // slotu pri kradezi; volajici nas ring ke staremu samplu uz nepouzije.)
    if (ring_ && stream_) {
        stream_->releaseRing(ring_);
        ring_ = nullptr;
    }
    stream_pending_   = false;
    underrun_fading_  = false;
    underrun_gain_    = 1.f;
    underrun_step_    = 0.f;
    file_request_off_ = 0;
    ring_cur_l_ = 0.f; ring_cur_r_ = 0.f; ring_cur_idx_ = -1;

    asset_ = asset;
    mic_   = (asset && !asset->mics.empty()) ? &asset->mics[0] : nullptr;
    active_           = (mic_ != nullptr && mic_->head_frames > 0);
    releasing_        = false;
    pending_release_  = false;   // retrigger ruse predchozi pending stav
    in_onset_         = true;
    position_         = 0.0;
    double sample_sr = mic_ ? (double)mic_->file.sample_rate : (double)engine_sr;
    pos_inc_ = pitch_ratio * (sample_sr / (double)engine_sr);
    vel_gain_  = vel_gain;
    onset_gain_ = 0.f;
    onset_step_ = 1.f / (kOnsetMs * 0.001f * engine_sr);
    rel_gain_   = 1.f;
    rel_step_   = 0.f;
    pan_l_ = pan_l;
    pan_r_ = pan_r;

    underrun_step_ = -1.f / (kUnderrunFadeMs * 0.001f * engine_sr);

    // Streamed → alokuj ring a posli prvni read request pro vse za head.
    if (active_ && mic_->mode == MicLayerMode::Streamed && stream_) {
        ring_ = stream_->acquireRing();
        if (ring_) {
            const int cap = ring_->capacity_frames;
            const int64_t want = (int64_t)cap;
            // Konec streamovaneho regionu = file.frames - head_frames. Kdyz se
            // celkove cely zbytek vleze do ringu, oznacime to jako eof_when_done
            // a worker po dohranou ring nastavi eof_.
            const int64_t total_stream = (int64_t)mic_->file.frames
                                       - (int64_t)mic_->head_frames;
            const bool eof_done = (want >= total_stream);
            const int64_t actual = (want < total_stream) ? want : total_stream;
            file_request_off_ = (int64_t)mic_->head_frames + actual;
            stream_->requestRead(ring_, mic_->file.path,
                                 (int64_t)mic_->head_frames, actual, eof_done);
            stream_pending_ = true;
        }
        // Kdyz acquireRing selhal (pool plny), Voice prozatim hraje jen do
        // konce head a pak utichne (zadny crash). FUTURE: voice steal podle
        // ring obsazenosti.
    }
}

void Voice::release(float release_ms, float engine_sr) {
    if (!active_ || releasing_) return;
    releasing_ = true;
    rel_gain_  = in_onset_ ? onset_gain_ : 1.f;
    rel_step_  = -rel_gain_ / (release_ms * 0.001f * engine_sr);
}

float Voice::currentLevel() const noexcept {
    if (!active_) return 0.f;
    float env = vel_gain_;
    if (in_onset_)  env *= onset_gain_;
    if (releasing_) env *= rel_gain_;
    return env;
}

bool Voice::process(float* out_l, float* out_r, int n_samples) noexcept {
    // DEBUG helper: zaloguj kazdou cestu deaktivace s konkretnim duvodem.
    // Pozn.: pouziva non-RT logger (mutex) — to porusi RT-safety, ale je to
    // ladici nastroj. V produkcni faze odstranit nebo nahradit RT ringem +
    // periodickym flushem.
    auto log_end = [this](const char* reason) {
        log::Logger::default_().log("voice_end", log::Severity::Info,
            "DEACTIVATE midi=%d reason=%s pos=%lld total=%d head=%d "
            "ring=%s ring_avail=%d ring_eof=%d releasing=%d in_onset=%d "
            "underrun_fading=%d damping=%d",
            midi_, reason, (long long)position_,
            mic_ ? mic_->file.frames : -1,
            mic_ ? mic_->head_frames : -1,
            ring_ ? "yes" : "no",
            ring_ ? ring_->available() : -1,
            (ring_ && ring_->eof_.load(std::memory_order_relaxed)) ? 1 : 0,
            (int)releasing_, (int)in_onset_,
            (int)underrun_fading_, (int)damping_);
    };

    if (!mic_ || mic_->head_frames <= 0) {
        // Nic k hrani; pripadny drzeny ring vrat.
        if (ring_ && stream_) { stream_->releaseRing(ring_); ring_ = nullptr; }
        if (active_) log_end("no_mic_or_empty_head");
        active_ = false;
        return false;
    }
    const float* head_data = mic_->preload_head.data();
    const int    head_frames  = mic_->head_frames;
    const int    total_frames = mic_->file.frames;

    bool produced = false;

    for (int i = 0; i < n_samples; ++i) {
        // Damping crossfade (zbytek predchoziho tonu pri retriggeru).
        if (damping_ && damp_pos_ < damp_len_) {
            out_l[i] += damp_buf_[damp_pos_ * 2];
            out_r[i] += damp_buf_[damp_pos_ * 2 + 1];
            damp_pos_++;
            if (damp_pos_ >= damp_len_) damping_ = false;
        }

        if (!active_) continue;   // damping muze dohravat i po active_=false

        int p0 = (int)position_;
        float sL = 0.f, sR = 0.f;

        if (p0 < head_frames - 1) {
            // V hlave: linearni interpolace mezi p0 a p0+1 (jako pred fazi 4).
            float frac = (float)(position_ - (double)p0);
            int   p1 = p0 + 1;
            sL = head_data[p0 * 2]     * (1.f - frac) + head_data[p1 * 2]     * frac;
            sR = head_data[p0 * 2 + 1] * (1.f - frac) + head_data[p1 * 2 + 1] * frac;
            position_ += pos_inc_;
        } else if (p0 < head_frames) {
            // Posledni vzorek hlavy — bez interpolace pres hranici (TODO faze
            // 6/7: 1-frame lookbehind v ringu pro plynuly join).
            sL = head_data[p0 * 2];
            sR = head_data[p0 * 2 + 1];
            position_ += pos_inc_;
        } else if (ring_) {
            // Streamed: nearest-neighbor, ale konzumuj spravny pocet frames podle
            // pos_inc_. ring_cur_idx_ = FILE-GLOBAL index naposledy popnuteho
            // frame; popujeme dokud nedosahneme floor(position_).
            if (ring_cur_idx_ < 0) {
                ring_cur_idx_ = (int64_t)head_frames - 1;
                ring_cur_l_   = head_data[(size_t)(head_frames - 1) * 2];
                ring_cur_r_   = head_data[(size_t)(head_frames - 1) * 2 + 1];
            }
            const int64_t target = (int64_t)position_;
            bool ok = true;
            while (ring_cur_idx_ < target) {
                float L, R;
                if (!ring_->popFrame(L, R)) { ok = false; break; }
                ring_cur_l_ = L; ring_cur_r_ = R; ring_cur_idx_++;
            }
            if (!ok) {
                if (ring_->eof_.load(std::memory_order_acquire)) {
                    log_end("ring_eof_drained");
                    active_ = false;
                    break;
                }
                if (!underrun_fading_) {
                    underrun_fading_ = true;
                    underrun_gain_   = 1.f;
                    log::Logger::default_().log("voice_end", log::Severity::Warning,
                        "UNDERRUN midi=%d pos=%lld total=%d head=%d ring_avail=%d "
                        "ring_eof=%d", midi_, (long long)position_, total_frames,
                        head_frames, ring_->available(),
                        (int)ring_->eof_.load(std::memory_order_relaxed));
                }
                sL = 0.f; sR = 0.f;
            } else {
                sL = ring_cur_l_; sR = ring_cur_r_;
            }
            position_ += pos_inc_;
        } else {
            // FullyLoaded sampl prekrocil head (= file frames) → konec.
            log_end("fullyloaded_past_head");
            active_ = false;
            break;
        }

        // Hard guard na konec souboru (kdyby pos jsel mimo file.frames i v ringu).
        if ((int)position_ >= total_frames && ring_ &&
            ring_->eof_.load(std::memory_order_acquire) &&
            ring_->available() == 0) {
            // Spotrebovan posledni vzorek; cisto vypneme po tomto frame.
            log_end("hard_guard_file_end");
            active_ = false;
        }

        // Onset ramp.
        float env = 1.f;
        if (in_onset_) {
            onset_gain_ += onset_step_;
            if (onset_gain_ >= 1.f) { onset_gain_ = 1.f; in_onset_ = false; }
            env = onset_gain_;
        }
        // Release ramp.
        if (releasing_) {
            env *= rel_gain_;
            rel_gain_ += rel_step_;
            if (rel_gain_ <= 0.f) {
                rel_gain_ = 0.f;
                log_end("release_ramp_zero");
                active_ = false;
            }
        }
        // Underrun fast fade (multiplikuje pres ostatni envelopy).
        if (underrun_fading_) {
            env *= underrun_gain_;
            underrun_gain_ += underrun_step_;
            if (underrun_gain_ <= 0.f) {
                underrun_gain_ = 0.f;
                log_end("underrun_fade_zero");
                active_ = false;
            }
        }

        float g = vel_gain_ * env;
        out_l[i] += sL * g * pan_l_;
        out_r[i] += sR * g * pan_r_;
        produced = true;

        if (!active_) break;
    }

    // Refill heuristika: kdyz ring klesl pod prah a soubor jeste nedohranl,
    // posli novy request. stream_pending_ ridime per-ring frame budgetem:
    // pamatujeme size posledniho requestu a "ocekavany" minimalni avail po
    // nem; jakmile worker dosahl/prekrocil ocekavanou napln (= request byl
    // splnen) NEBO klesli jsme zase pod prah (= avail je nizky → bezpecne
    // poslat dalsi i kdyby predchozi jeste neskoncil — fronta ma kapacitu
    // a worker stejne dotahne v sekvenci), pending shodime.
    //
    // Jednodussi a robustnejsi: shodit pending pokud avail je nad polovinou
    // kapacity — tj. predchozi refill jiz dorazil. Pokud byl drop-on-full,
    // dalsi request stejne projde a worker se chova idempotenne.
    if (ring_ && stream_ && active_) {
        const int avail = ring_->available();
        const int thr   = stream_->refillThresholdFrames();
        const int half_cap = ring_->capacity_frames / 2;
        if (stream_pending_ && avail >= half_cap) {
            stream_pending_ = false;   // worker dohnal predchozi request
        }
        if (!stream_pending_ && avail < thr) {
            const int64_t remain = (int64_t)total_frames - file_request_off_;
            if (remain > 0) {
                // Pozadej tolik, kolik se ted vejde do volneho mista v ringu.
                int64_t want = (int64_t)(ring_->capacity_frames - avail);
                if (want > remain) want = remain;
                const bool eof_done = (file_request_off_ + want >= (int64_t)total_frames);
                stream_->requestRead(ring_, mic_->file.path,
                                     file_request_off_, want, eof_done);
                file_request_off_ += want;
                stream_pending_    = true;
            } else {
                // Cely zbytek souboru uz byl pozadan; jakmile worker dohraje,
                // nastavi eof_ a Voice doplyne hlas cisto. Zadny dalsi request.
            }
        }
    }

    // Pri deaktivaci uvolni ring (jednou).
    if (!active_ && ring_ && stream_) {
        stream_->releaseRing(ring_);
        ring_ = nullptr;
    }

    return active_ || damping_ || produced;
}

} // namespace ithaca
