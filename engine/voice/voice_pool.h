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

constexpr int kDefaultPoolSize = 128;
constexpr int kMaxPoolSize     = 256;

class VoicePool {
public:
    explicit VoicePool(int pool_size = kDefaultPoolSize);

    // Spusti (nebo retriggeruje) ton. keyboard_spread ovlivnuje pan dle noty.
    void noteOn(int midi, const VoiceSpec& spec, float engine_sr,
                float keyboard_spread = 0.6f);
    // Release vsech hlasu dane noty.
    void noteOff(int midi, float release_ms, float engine_sr);
    // Release vsech hlasu (panic).
    void allNotesOff(float release_ms, float engine_sr);

    // Renderuj vsechny aktivni hlasy additivne. Vrati true kdyz neco znelo.
    bool processBlock(float* out_l, float* out_r, int n_samples,
                      float engine_sr) noexcept;

    int activeCount() const noexcept;
    int poolSize() const { return (int)voices_.size(); }

private:
    int findSlot();                          // volny, nebo nejtissi (kradez)

    std::vector<Voice> voices_;
};

} // namespace ithaca
