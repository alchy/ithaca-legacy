#pragma once
// engine/dsp/dsp_stage.h — genericke rozhrani DSP stage + parametricky deskriptor.
// IParamPage je GUI-facing (implementuje ho kazda stage I "VOICE" stranka v GUI).
// DspStage pridava audio-thread zpracovani. Viz spec 2026-06-01-dsp-chain-config.

namespace ithaca::dsp {

// Deskriptor jednoho parametru pro genericke GUI vykresleni + persistenci.
struct Param {
    const char* id;            // stabilni klic pro persistenci ("threshold_db")
    const char* label;         // UI eyebrow ("THRESHOLD")
    float       min, max, def;
    const char* fmt;           // DecoSlider format ("%.1f dB")
    bool        readonly = false;
};

// GUI-facing rozhrani. set() klampuje do [min,max].
struct IParamPage {
    virtual ~IParamPage() = default;
    virtual const char* name() const = 0;
    virtual int          paramCount() const = 0;
    virtual const Param& param(int i) const = 0;
    virtual float        get(int i) const = 0;
    virtual void         set(int i, float v) = 0;
    virtual bool         hasEnable() const = 0;   // VOICE: false
    virtual bool         enabled() const = 0;
    virtual void         setEnabled(bool on) = 0;
    // Volitelny read-only metr (limiter GR / AGC current gain).
    // Vraci false kdyz stranka metr nema.
    virtual bool         meter(float& value, const char*& label) const = 0;
};

// Audio-thread stage = param page + DSP. set()/setEnabled() z GUI threadu,
// process() z audio threadu; implementace drzi parametry atomicky.
struct DspStage : IParamPage {
    virtual void prepare(float sample_rate, int max_block) = 0;
    virtual void reset() = 0;
    virtual void process(float* L, float* R, int n) = 0;
};

} // namespace ithaca::dsp
