// engine/voice/voice.cpp — viz voice.h. Adaptovano z icr sampler_core voice loop.
#include "voice/voice.h"

#include <algorithm>
#include <cmath>

namespace ithaca {

void Voice::prepareDamp(float engine_sr) {
    // Vytvor kratky fade-out ze soucasne pozice → damp_buf_, aby novy ton
    // (retrigger) nelupnul. Funguje jen kdyz hlas hraje a ma data.
    if (!active_ || !mic_) { damping_ = false; return; }
    int pos = (int)position_;
    if (pos >= mic_->frames) { damping_ = false; return; }
    int damp_frames = (std::min)((int)(kDampingMs * 0.001f * engine_sr), kDampMaxFrames);
    int avail = (std::min)(damp_frames, mic_->frames - pos);
    if (avail <= 0) { damping_ = false; return; }
    const float* src = mic_->data.data() + (size_t)pos * 2;
    float env = vel_gain_;
    if (releasing_) env *= rel_gain_;
    float step = 1.f / (float)avail;
    for (int i = 0; i < avail; ++i) {
        float fade = 1.f - (float)i * step;
        damp_buf_[i * 2]     = src[i * 2]     * env * fade * pan_l_;
        damp_buf_[i * 2 + 1] = src[i * 2 + 1] * env * fade * pan_r_;
    }
    damp_len_ = avail;
    damp_pos_ = 0;
    damping_  = true;
}

void Voice::start(const SampleAsset* asset, double pitch_ratio, float vel_gain,
                  float pan_l, float pan_r, float engine_sr) {
    asset_ = asset;
    mic_   = (asset && !asset->mics.empty()) ? &asset->mics[0] : nullptr;
    active_    = (mic_ != nullptr && mic_->frames > 0);
    releasing_ = false;
    in_onset_  = true;
    position_  = 0.0;
    double sample_sr = mic_ ? (double)mic_->sample_rate : (double)engine_sr;
    pos_inc_ = pitch_ratio * (sample_sr / (double)engine_sr);
    vel_gain_  = vel_gain;
    onset_gain_ = 0.f;
    onset_step_ = 1.f / (kOnsetMs * 0.001f * engine_sr);
    rel_gain_   = 1.f;
    rel_step_   = 0.f;
    pan_l_ = pan_l;
    pan_r_ = pan_r;
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
    if (!mic_ || mic_->frames <= 0) { active_ = false; return false; }
    const float* data = mic_->data.data();
    const int max_frames = mic_->frames;

    for (int i = 0; i < n_samples; ++i) {
        // Damping crossfade (zbytek predchoziho tonu pri retriggeru).
        if (damping_ && damp_pos_ < damp_len_) {
            out_l[i] += damp_buf_[damp_pos_ * 2];
            out_r[i] += damp_buf_[damp_pos_ * 2 + 1];
            damp_pos_++;
            if (damp_pos_ >= damp_len_) damping_ = false;
        }

        int p0 = (int)position_;
        if (p0 >= max_frames - 1) { active_ = false; break; }
        float frac = (float)(position_ - (double)p0);
        int p1 = p0 + 1;

        float sL = data[p0 * 2]     * (1.f - frac) + data[p1 * 2]     * frac;
        float sR = data[p0 * 2 + 1] * (1.f - frac) + data[p1 * 2 + 1] * frac;
        position_ += pos_inc_;

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
            if (rel_gain_ <= 0.f) { rel_gain_ = 0.f; active_ = false; }
        }

        float g = vel_gain_ * env;
        out_l[i] += sL * g * pan_l_;
        out_r[i] += sR * g * pan_r_;

        if (!active_) break;
    }
    // Damping muze dobehnout i kdyz uz tlo neni aktivni — hlas je "aktivni"
    // dokud bud hraje tlo, nebo dobiha damping.
    return active_ || damping_;
}

} // namespace ithaca
