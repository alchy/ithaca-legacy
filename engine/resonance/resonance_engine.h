#pragma once
// engine/resonance/resonance_engine.h
// -----------------------------------
// Centralni koordinator sympaticke rezonance (faze 5). Spravuje 128 slotu
// `ResonanceVoice` (jeden per MIDI nota) a vynucuje invariant 5.5.1:
//
//   "Pro kazdou MIDI notu N v kazdy okamzik existuje maximalne JEDEN znejici
//    zdroj na strunu N — bud hlavni hlas, NEBO rezonancni hlas, NIKDY oba."
//
// Tri vynucujici pravidla v engine:
//   (1) Eligibility filter: rezonance N se nealokuje, dokud existuje hlavni
//       hlas N (jakykoli stav) ANEBO struna N je ztlumena (damping <= eps).
//   (2) Per-nota uniqueness: 1 slot per N — multi-source (M1 i M2 budi N) jen
//       AKTUALIZUJE amplitudu na sloty[N], nealokuje druhy hlas.
//   (3) Rule B (state transition): note-on(N) kdyz rezonance N hraje → fast
//       fade rezonance + okamzite zablokovat dalsi eligibility (until note-off).
//
// Per-blok update (processBlock): kazdy aktivni hlas dostane novy
// `target_gain = last_excite * pedal.dampingFor(N)`. Tim se zmena CC64 (vc.
// half-pedal) plynule promita do hlasitosti rezonance bez "udalosti pedal up".
// `last_excite` se kazdy blok exponencialne tlumi (kExciteDecayPerBlock),
// takze i pri trvale stisknutem pedalu rezonance prirozene utichne.

#include "voice/resonance_voice.h"

#include <array>
#include <atomic>
#include <memory>

namespace ithaca {

class StreamEngine;
class VoicePool;
class PedalState;
struct Bank;

// Default rozpocet aktivnich rezonancnich hlasu. Nezavisle na hlavnim poolu —
// pri prekroceni krademe NEJTISSI rezonancni hlas (nikdy main voice).
constexpr int kDefaultMaxResonanceVoices = 32;

// Prah na excitation gain pod kterym rezonanci vubec nealokujeme (zanedbatelna
// hlasitost, jen bychom kradli sloty).
constexpr float kResonanceExciteMinGain = 0.001f;

// Prah na harmonickou blizkost pod kterou notu vubec nepovazujeme za zdroj
// rezonance (sekunda/tritonus → ~0 v harmonicProximity).
constexpr float kResonanceHarmonicMin = 0.05f;

// Default tau pro exponencialni decay `last_excite` per blok. Cca 5 sekund —
// realna struna se taky tlumi vlivem vnitrniho treni, ne jen tlumitkem.
constexpr float kDefaultExciteDecayMs = 5000.f;

// Default keyboard spread pro pan rezonancnich hlasu. Stejna hodnota jako
// EngineConfig::keyboard_spread; rezonance je obecne tisi nez main voice, takze
// presny pan detail malo zalezi — drzime defualt napevno (bez konfigurace).
constexpr float kResonanceKeyboardSpread = 0.6f;

class ResonanceEngine {
public:
    explicit ResonanceEngine(int max_resonance_voices = kDefaultMaxResonanceVoices);

    // Pripoji StreamEngine (volat jednou pri init, pred prvni note-on).
    void setStreamEngine(StreamEngine* se);

    void reset() noexcept;   // hard-stop vsech rezonancnich hlasu (reloadBank)

    // Linearni gain rezonance (z dB). Realtime-safe; cte se v onPlayedNoteOn.
    void  setGainDb(float db);
    float gainLinear() const;
    // Cilove dB pro vyber velocity vrstvy (nearestSlotByRms). Realtime-safe.
    void  setLayerTargetDb(float db);
    float layerTargetDb() const;
    // Zapnuti/vypnuti sympaticke rezonance (onPlayedNoteOn early-return kdyz off).
    void  setEnabled(bool on);
    bool  enabled() const;

    // Zivy strop poctu soucasne znejicich rezonancnich hlasu (GUI slider, dle HW).
    // Meni jen kolik hlasu smi znit (enforceVoiceBudget) — bez resize poli/ringu.
    void setMaxVoices(int n) noexcept {
        if (n < 1)  n = 1;
        if (n > 64) n = 64;
        max_voices_.store(n, std::memory_order_relaxed);
    }
    int  maxVoices() const noexcept { return max_voices_.load(std::memory_order_relaxed); }

    // Aktualni decay koeficient (vystaven kvuli testum + diagnostice).
    float exciteDecayPerBlock() const { return decay_per_block_; }
    // Spocti decay tak, aby `last_excite` mel pozadovany tau v ms za audio blok.
    void  setExciteDecayTimeMs(float tau_ms, int block_size, float engine_sr);

    // Rule B — volat PRED `voice_pool.noteOn(N)`. Pokud rezonance N hraje,
    // spusti fast fade-out a vynuluje last_excite (eligibility filter pak
    // blokuje dalsi excitaci, dokud main voice N existuje).
    void onSelfNoteOn(int played_midi, float engine_sr);

    // Volat PO `voice_pool.noteOn(M)`. Pro vsechny noty N != M, ktere jsou
    // eligibilni (damping > eps & !hasActiveMainVoice & !fadingOut), spusti
    // nebo updatuje rezonanci s amplitudou `(V/127) * harm(N,M) * strength`.
    void onPlayedNoteOn(int played_midi, int velocity,
                        const Bank& bank, const VoicePool& pool,
                        const PedalState& pedal, float engine_sr);

    // Per-blok render. Pred renderem aplikuje per-blok decay na `last_excite`
    // a aktualizuje target_gain kazdeho aktivniho hlasu podle aktualniho
    // `pedal.dampingFor(N)`. Vraci true kdyz nekterý hlas produkoval samply.
    bool processBlock(float* out_l, float* out_r, int n_samples,
                      const PedalState& pedal) noexcept;

    // RAM cache rezonance: per-nota true = cilova vrstva ma naplneny preload_resonance
    // (cache mod). false = stream mod (ring). Psano off-RT (loadBank/rebuild), cteno
    // audio threadem v onPlayedNoteOn.
    void setCacheReady(const std::array<bool, 128>& ready) noexcept;
    void clearCacheReady() noexcept;                 // vse false (start rebuildu)
    void requestRecacheFade() noexcept;              // audio thread fadene aktivni pri pristim processBlock

    // -- Diagnostika a test helpery --

    int  activeCount() const noexcept;
    bool isResonating(int midi) const noexcept;
    // Aktualni gain rezonance N (0 pokud nehraje).
    float currentLevelFor(int midi) const noexcept;

private:
    // Drzime "posledni excitation" per N (nezavisle na pedalu) tak, aby
    // processBlock mohl per-blok pretahnout target_gain = last_excite * damping.
    struct ExciteState { float last_excite = 0.f; };

    // Pokud aktivnich hlasu > max_voices_, najdi nejtissi a fadeOut. Opakuj
    // dokud nejsou v limitu. Nikdy nekradne main voice (jine pole).
    void enforceVoiceBudget(float engine_sr);

    // Pan pro rezonancni hlas — stejny vzorec jako VoicePool, jen s pevnou
    // keyboard_spread (rezonance je tise, detail panu malo zalezi).
    static void panForNote(int midi, float& pan_l, float& pan_r);

    std::array<std::unique_ptr<ResonanceVoice>, 128> voices_;
    ExciteState         excite_state_[128];
    std::atomic<float>  gain_lin_{0.251f};       // ~ -12 dB default
    std::atomic<float>  layer_target_db_{-30.f};
    std::atomic<bool>   enabled_{true};
    float               decay_per_block_ = 0.998f;  // ~5 s @ 256 samples/48k
    std::atomic<int>    max_voices_{kDefaultMaxResonanceVoices};
    StreamEngine*       stream_          = nullptr;

    std::array<std::atomic<bool>, 128> reso_cache_ready_{};   // default vse false
    std::atomic<bool>                  recache_fade_request_{false};
    float                              last_engine_sr_ = 48000.f;  // pro fade v processBlock
};

} // namespace ithaca
