#pragma once
// engine/dsp/dsp_math.h — sdilene DSP primitivy (stateless, inline, RT-safe).
// Portovano z icr/engine/dsp/dsp_math.h. Pouziva BBE, Limiter, AGC.
#include <cmath>
#include <algorithm>

namespace ithaca::dsp {

static constexpr float PI  = 3.14159265358979f;
static constexpr float TAU = 2.f * PI;

// dB -> linearni amplituda. 0 dB -> 1.0
inline float db_to_lin(float db) { return std::pow(10.f, db / 20.f); }
// linearni amplituda -> dB. 1.0 -> 0 dB
inline float lin_to_db(float lin) { return 20.f * std::log10((std::max)(lin, 1e-9f)); }

// Per-sample multiplikativni decay koeficient: exp(-1 / (tau_s * sr)).
inline float decay_coeff(float tau_seconds, float sample_rate) {
    return std::exp(-1.f / (std::max)(tau_seconds * sample_rate, 1.f));
}

// Biquad koeficienty (normalizovane, a0 = 1).
struct BiquadCoeffs { float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f; };
// Biquad stav (DF-II transposed, 2 delay elementy).
struct BiquadState { float s1 = 0.f, s2 = 0.f; };

// Jeden vzorek pres biquad (DF-II transposed). Aktualizuje stav in-place.
inline float biquad_tick(float x, const BiquadCoeffs& c, BiquadState& s) {
    float y = c.b0 * x + s.s1;
    s.s1    = c.b1 * x - c.a1 * y + s.s2;
    s.s2    = c.b2 * x - c.a2 * y;
    return y;
}

// RBJ high-shelf (Audio EQ Cookbook). fc=stred [Hz], gain_db boost/cut, sr.
inline BiquadCoeffs rbj_high_shelf(float fc, float gain_db, float sr) {
    float A = std::pow(10.f, gain_db / 40.f);
    float w0 = TAU * fc / sr, cosw = std::cos(w0), sinw = std::sin(w0);
    float al = sinw / 2.f * std::sqrt((A + 1.f/A) * (1.f/1.f - 1.f) + 2.f);
    float sqA2 = 2.f * std::sqrt(A) * al;
    float a0 = (A+1.f) - (A-1.f)*cosw + sqA2, ia = 1.f / a0;
    BiquadCoeffs c;
    c.b0 = ( A * ((A+1.f) + (A-1.f)*cosw + sqA2)) * ia;
    c.b1 = (-2.f*A * ((A-1.f) + (A+1.f)*cosw))    * ia;
    c.b2 = ( A * ((A+1.f) + (A-1.f)*cosw - sqA2)) * ia;
    c.a1 = ( 2.f * ((A-1.f) - (A+1.f)*cosw))      * ia;
    c.a2 = (       (A+1.f) - (A-1.f)*cosw - sqA2)  * ia;
    return c;
}
// RBJ low-shelf.
inline BiquadCoeffs rbj_low_shelf(float fc, float gain_db, float sr) {
    float A = std::pow(10.f, gain_db / 40.f);
    float w0 = TAU * fc / sr, cosw = std::cos(w0), sinw = std::sin(w0);
    float al = sinw / 2.f * std::sqrt((A + 1.f/A) * (1.f/1.f - 1.f) + 2.f);
    float sqA2 = 2.f * std::sqrt(A) * al;
    float a0 = (A+1.f) + (A-1.f)*cosw + sqA2, ia = 1.f / a0;
    BiquadCoeffs c;
    c.b0 = ( A * ((A+1.f) - (A-1.f)*cosw + sqA2)) * ia;
    c.b1 = ( 2.f*A * ((A-1.f) - (A+1.f)*cosw))    * ia;
    c.b2 = ( A * ((A+1.f) - (A-1.f)*cosw - sqA2)) * ia;
    c.a1 = (-2.f * ((A-1.f) + (A+1.f)*cosw))      * ia;
    c.a2 = (       (A+1.f) + (A-1.f)*cosw - sqA2)  * ia;
    return c;
}

// RBJ low-pass (Audio EQ Cookbook). fc [Hz], q (0.707 = Butterworth), sr.
inline BiquadCoeffs rbj_lowpass(float fc, float q, float sr) {
    float w0 = TAU * fc / sr, cosw = std::cos(w0), sinw = std::sin(w0);
    float al = sinw / (2.f * q);
    float a0 = 1.f + al, ia = 1.f / a0;
    BiquadCoeffs c;
    c.b0 = ((1.f - cosw) * 0.5f) * ia;
    c.b1 = ( 1.f - cosw)         * ia;
    c.b2 = ((1.f - cosw) * 0.5f) * ia;
    c.a1 = (-2.f * cosw)         * ia;
    c.a2 = ( 1.f - al)           * ia;
    return c;
}
// RBJ high-pass.
inline BiquadCoeffs rbj_highpass(float fc, float q, float sr) {
    float w0 = TAU * fc / sr, cosw = std::cos(w0), sinw = std::sin(w0);
    float al = sinw / (2.f * q);
    float a0 = 1.f + al, ia = 1.f / a0;
    BiquadCoeffs c;
    c.b0 = ((1.f + cosw) * 0.5f) * ia;
    c.b1 = (-(1.f + cosw))       * ia;
    c.b2 = ((1.f + cosw) * 0.5f) * ia;
    c.a1 = (-2.f * cosw)         * ia;
    c.a2 = ( 1.f - al)           * ia;
    return c;
}
// Hermite smoothstep: 0 pod edge0, 1 nad edge1, hladce mezi.
inline float smoothstep(float x, float edge0, float edge1) {
    if (edge1 <= edge0) return x < edge0 ? 0.f : 1.f;
    float t = (x - edge0) / (edge1 - edge0);
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    return t * t * (3.f - 2.f * t);
}

// Vyhlazeni gain obalky: rychly attack (snizovani), pomaly release (zotaveni).
inline float gain_envelope_smooth(float current, float target,
                                  float attack_coeff, float release_coeff) {
    if (target < current) return attack_coeff  * current + (1.f - attack_coeff)  * target;
    else                  return release_coeff * current + (1.f - release_coeff) * target;
}

} // namespace ithaca::dsp
