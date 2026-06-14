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
    // Damping crossfade = fade-out z NEPREHRANYCH (nadchazejicich) vzorku, NE z
    // historie — aby damp_buf_[0] navazoval na posledni prehrany vzorek (zadna
    // nespojitost = zadny click). V head regionu kopirujeme dopredu z
    // preload_head; za head vyctema dalsi framy z RINGU (jiz prefetchnute), takze
    // doběh funguje i pro streamovane hlasy (drive zde byl click pri retriggeru).
    damping_  = false;
    damp_len_ = 0;
    damp_pos_ = 0;
    if (active_ && mic_ && damp_buf_) {
        const int damp_frames = (std::min)((int)(kDampingMs * 0.001f * engine_sr),
                                           kDampMaxFrames);
        float env = vel_gain_;
        if (in_onset_)        env *= onset_gain_;   // steal behem onsetu: bez skoku nahoru
        if (releasing_)       env *= rel_gain_;
        if (underrun_fading_) env *= underrun_gain_;
        const int pos = (int)position_;

        if (pos < mic_->head_frames) {
            // a) Hlavni region: dopredna kopie nadchazejicich vzorku z preload_head.
            int avail = (std::min)(damp_frames, mic_->head_frames - pos);
            if (avail > 0) {
                const float* src = mic_->preload_head.data() + (size_t)pos * 2;
                float step = 1.f / (float)avail;
                for (int i = 0; i < avail; ++i) {
                    float fade = 1.f - (float)i * step;
                    damp_buf_[i * 2]     = src[i * 2]     * env * fade * pan_l_;
                    damp_buf_[i * 2 + 1] = src[i * 2 + 1] * env * fade * pan_r_;
                }
                damp_len_ = avail;
                damping_  = true;
            }
        } else if (reader_.hasRing()) {
            // b) Streamed region: nadchazejici vzorky uz lezi v ringu — vypopuj
            //    je (ring stejne hned uvolnime) a zafaduj. Pravy doběh waveformy
            //    → bez nespojitosti i mimo head. Kratsi kdyz ring nestaci.
            const int n = reader_.popInto(damp_buf_, damp_frames);
            if (n > 0) {
                float step = 1.f / (float)n;
                for (int i = 0; i < n; ++i) {
                    float fade = 1.f - (float)i * step;
                    damp_buf_[i * 2]     = damp_buf_[i * 2]     * env * fade * pan_l_;
                    damp_buf_[i * 2 + 1] = damp_buf_[i * 2 + 1] * env * fade * pan_r_;
                }
                damp_len_ = n;
                damping_  = true;
            }
        }
    }

    // 2) VZDY cleanup ring + state (i kdyz damp_buf_ nebyl naplnen).
    reader_.release(stream_);
    // DEBUG: zaloguj deaktivaci v dusledku retrigger/steal damping (RT ring).
    LOG_RT_INFO("voice_end",
        "DEACTIVATE midi=%d reason=damped_for_retrigger_or_steal damp_len=%d",
        midi_, damp_len_);
    active_          = false;   // pool ho ted muze rovnou ukrast / pouzit znovu
    releasing_       = false;
    pending_release_ = false;
}

void Voice::hardStop() noexcept {
    reader_.release(stream_);
    underrun_fading_ = false;
    active_          = false;
    releasing_       = false;
    pending_release_ = false;
    damping_         = false;
    damp_len_ = 0; damp_pos_ = 0;
    asset_ = nullptr;
    mic_   = nullptr;
}

void Voice::start(const SampleAsset* asset, double pitch_ratio, float vel_gain,
                  float pan_l, float pan_r, float engine_sr) {
    // Pred prepsanim stavu: kdyby jsme drzeli ring z minule, vrat ho do poolu.
    // (V realnem provozu prepareDamp + start jsou volane na rovne tom samem
    // slotu pri kradezi; volajici nas ring ke staremu samplu uz nepouzije.)
    reader_.release(stream_);
    underrun_fading_  = false;
    underrun_gain_    = 1.f;
    underrun_step_    = 0.f;

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

    // Streamed → alokuj ring a posli prvni read request pro vse za head
    // (reader: acquire + request s no-advance-on-drop). Kdyz acquireRing
    // selze (pool plny), Voice prozatim hraje jen do konce head a pak utichne
    // (zadny crash). FUTURE: voice steal podle ring obsazenosti.
    if (active_ && mic_->mode == MicLayerMode::Streamed && stream_) {
        (void)reader_.begin(stream_, mic_->file,
                            (int64_t)mic_->head_frames,
                            (int64_t)mic_->file.frames);
    }
}

void Voice::release(float release_ms, float engine_sr) {
    if (!active_ || releasing_) return;
    if (release_ms < 0.1f) release_ms = 0.1f;   // guard: 0 → -inf step
    releasing_ = true;
    // Soucin onset*release je spojity: release startuje VZDY z 1.0 a onset
    // rampa dobiha dal. (Drive rel_gain_=onset_gain_ pri onsetu → v process
    // se nasobi OBE rampy → env skok g0 → g0^2 = klik pri staccatu.)
    rel_gain_  = 1.f;
    rel_step_  = -1.f / (release_ms * 0.001f * engine_sr);
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
    // RT-safe: LOG_RT_* pise do lock-free ringu (flush dela non-RT thread).
    auto log_end = [this](const char* reason) {
        LOG_RT_INFO("voice_end",
            "DEACTIVATE midi=%d reason=%s pos=%lld total=%d head=%d "
            "ring=%s ring_avail=%d ring_eof=%d releasing=%d in_onset=%d "
            "underrun_fading=%d damping=%d",
            midi_, reason, (long long)position_,
            mic_ ? mic_->file.frames : -1,
            mic_ ? mic_->head_frames : -1,
            reader_.hasRing() ? "yes" : "no",
            reader_.ringAvailable(),
            reader_.eofRelaxed() ? 1 : 0,
            (int)releasing_, (int)in_onset_,
            (int)underrun_fading_, (int)damping_);
    };

    if (!mic_ || mic_->head_frames <= 0) {
        // Nic k hrani; pripadny drzeny ring vrat.
        reader_.release(stream_);
        if (active_) log_end("no_mic_or_empty_head");
        active_ = false;
        return false;
    }
    const float* head_data = mic_->preload_head.data();
    const int    head_frames  = mic_->head_frames;
    const int    total_frames = mic_->file.frames;

    bool produced = false;
    // Bulk rezim ringu: 1 acquire + 1 release za blok misto 3 atomik/vzorek.
    if (reader_.hasRing()) reader_.beginBlock();

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
        } else if (reader_.hasRing()) {
            // Streamed: lin. interpolace pres lo/hi okno readeru. Posledni
            // head frame (p0 == head_frames-1) sem spada take — seed lo =
            // posledni head frame, hi = prvni ring pop → plynuly sev.
            if (!reader_.seeded()) {
                reader_.seed(head_data[(size_t)(head_frames - 1) * 2],
                             head_data[(size_t)(head_frames - 1) * 2 + 1],
                             (int64_t)head_frames - 1);
            }
            const bool underrun =
                reader_.advance((int64_t)position_) ==
                StreamedSampleReader::Advance::RingEmpty;
            if (underrun) {
                // Cisty konec: cely soubor uz byl vyzadan (requestOffset
                // dosahl konce) a ring je prazdny → legitimni konec, Info.
                // Jinak worker nestihl dodat data → skutecny underrun, Warning.
                // noteUnderrun() razitkujeme JEN pri skutecnem underrunu (ne
                // pri cistem konci samplu) — jinak by MAIN ring indikator
                // blikal cervene po kazde normalne dohraje dlouhe note.
                const bool clean_end = reader_.cleanEnd();
                if (!underrun_fading_) {
                    underrun_fading_ = true;
                    underrun_gain_   = 1.f;
                    if (clean_end) {
                        LOG_RT_INFO("voice_end",
                            "END-OF-SAMPLE midi=%d pos=%lld total=%d", midi_,
                            (long long)position_, total_frames);
                    } else {
                        if (stream_) stream_->noteUnderrun();
                        LOG_RT_WARN("voice_end",
                            "UNDERRUN midi=%d pos=%lld total=%d head=%d ring_avail=%d",
                            midi_, (long long)position_, total_frames,
                            head_frames, reader_.ringAvailable());
                    }
                }
                if (clean_end) {
                    // Cisty EOF: deactivate+zero (reference chovani icr — sampl
                    // prirozene dohral do ~ticha, neni co drzet).
                    sL = 0.f; sR = 0.f;
                } else {
                    // Skutecny underrun: drz posledni znamy vzorek, fade ho
                    // tvaruje (nuly by 5ms rampu obesly = tvrdy strih/klik).
                    sL = reader_.loL(); sR = reader_.loR();
                }
            } else {
                float frac = (float)(position_ - (double)reader_.loIdx());
                if (frac < 0.f) frac = 0.f;
                if (frac > 1.f) frac = 1.f;
                sL = reader_.loL() * (1.f - frac) + reader_.hiL() * frac;
                sR = reader_.loR() * (1.f - frac) + reader_.hiR() * frac;
            }
            position_ += pos_inc_;
        } else {
            // FullyLoaded sampl prekrocil head (= file frames) → konec.
            log_end("fullyloaded_past_head");
            active_ = false;
            break;
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

    reader_.endBlock();   // commit r_ pred refill (ten cte available z atomik)

    // Refill heuristika zije v readeru (prah z StreamEngine, half-cap reset
    // pendingu, no-advance-on-drop).
    if (reader_.hasRing() && stream_ && active_) {
        reader_.refill(stream_, mic_->file);
    }

    // Pri deaktivaci uvolni ring (jednou).
    if (!active_ && reader_.hasRing() && stream_) {
        reader_.release(stream_);
    }

    return active_ || damping_ || produced;
}

} // namespace ithaca
