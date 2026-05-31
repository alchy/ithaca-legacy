#pragma once
// engine/voice/voice_pool.h
// -------------------------
// Pool N hlasu (default 128). noteOn alokuje volny slot, nebo ukrade nejtissi
// (spec: nejtissi sampl z celeho poolu). Retrigger tehoz tonu nejdriv dampne
// stary hlas (click-free), pak spusti novy. noteOff spusti release vsech hlasu
// dane noty. processBlock renderuje vsechny aktivni hlasy additivne.

#include "voice/voice.h"
#include "voice/patch_manager.h"

#include <vector>

namespace ithaca {

class StreamEngine;
class PedalState;

constexpr int kDefaultPoolSize = 128;
constexpr int kMaxPoolSize     = 256;

class VoicePool {
public:
    explicit VoicePool(int pool_size = kDefaultPoolSize);

    // Pripoji StreamEngine ke vsem hlasum (faze 4). Vola Engine::init.
    // nullptr je OK = streaming nedostupny (vsechny Streamed samply utichnou
    // po preload_head).
    void setStreamEngine(StreamEngine* se);

    // Spusti (nebo retriggeruje) ton. keyboard_spread ovlivnuje pan dle noty.
    // pedal (volitelne, muze byt nullptr): kdyz je k dispozici, findSlot pri
    // kradezi PREFERUJE NE-DRZENE noty (uzivatel je uz pustil — pedal je drzi
    // jen v sustainu) pred drzenymi. Ucely pri pedalu DOWN s mnoha hlasy.
    void noteOn(int midi, const VoiceSpec& spec, float engine_sr,
                float keyboard_spread = 0.6f,
                const PedalState* pedal = nullptr);
    // Release vsech hlasu dane noty.
    void noteOff(int midi, float release_ms, float engine_sr);
    // Release vsech hlasu (panic).
    void allNotesOff(float release_ms, float engine_sr);

    // Renderuj vsechny aktivni hlasy additivne. Vrati true kdyz neco znelo.
    bool processBlock(float* out_l, float* out_r, int n_samples,
                      float engine_sr) noexcept;

    int activeCount() const noexcept;
    int poolSize() const { return (int)voices_.size(); }

    // True kdyz nektery hlas v poolu zni notu `midi` (vc. releasing/sustained
    // dozvuku). Pouziva ResonanceEngine pro eligibility filter 5.5.1 (1):
    // rezonance N se nealokuje, dokud existuje hlavni hlas N v jakemkoli stavu.
    bool hasActiveMainVoice(int midi) const noexcept;

private:
    // Volny slot; jinak nejtissi releasing; jinak nejtissi NE-drzeny; jinak
    // nejtissi z celeho poolu. pedal nullptr → posledni dva stupne splynou.
    int findSlot(const PedalState* pedal);

    std::vector<Voice> voices_;
};

} // namespace ithaca
