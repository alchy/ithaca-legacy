// engine/resonance/resonance_engine.cpp — viz resonance_engine.h.
//
// Implementacni poznamky k design rozhodnutim (delegovane z planu na implementaci):
//
// 1) FadingOut accessor: ResonanceVoice::fadingOut() vystavuje is_fading_out_.
//    onPlayedNoteOn pak preskoci N, ktere prave fade-uji (rule B in-progress).
//    Bez toho by nova excitace addExcitation() resetla is_fading_out_ → fight
//    s rule-B fade. Cista varianta — explicitni "rule B drzi slot dokud nedohaje".
//
// 2) panForNote: stejny vzorec jako VoicePool::panForNote, jen s pevnou
//    keyboard_spread = kResonanceKeyboardSpread (= 0.6, default jako EngineConfig).
//    Rezonance je obecne mnohem tisi nez main voice, takze presny pan detail
//    malo zalezi — drzime default bez konfigurace skrz ResonanceEngine.
//
// 3) decay_per_block_: per-blok exponencialni decay last_excite. Default odpovida
//    tau ~5 s pri block_size=256 @ 48k (=> ~5.3 ms/blok => exp(-5.3/5000) ≈ 0.998).
//    Engine si po setBlockSize zavola setExciteDecayTimeMs s realnym sr.
//
// 4) Voice budget: pri prekroceni max_voices_ kradneme NEJTISSI rezonancni hlas
//    pres fadeOut() (NE hard cut — ten by lupnul). Nikdy nekrademe main voice
//    (jine pole). Pri "nelze najit obet" (vse uz fade-uje) ukoncime — dalsi
//    rezonance prosto nealokujeme, ale neuvolnujeme tvrde existujici.
#include "resonance/resonance_engine.h"

#include "pedal/pedal_state.h"
#include "resonance/harmonic_proximity.h"
#include "resonance/resonance_layer_select.h"
#include "sample/sample_types.h"
#include "util/log.h"
#include "voice/voice_pool.h"

#include <algorithm>
#include <cmath>

namespace ithaca {

ResonanceEngine::ResonanceEngine(int max_resonance_voices) {
    // Predalokace vsech 128 hlasu. (Drive lazy make_unique v onPlayedNoteOn
    // = malloc na audio vlakne; ~20 kB celkem nestoji za RT prohrešek.)
    for (auto& v : voices_) v = std::make_unique<ResonanceVoice>();
    setMaxVoices(max_resonance_voices);
}

void ResonanceEngine::setStreamEngine(StreamEngine* se) {
    stream_ = se;
    // Existujici hlasy taky aktualizuj (pripad: setStreamEngine po prvni note-on).
    for (auto& v : voices_) {
        if (v) v->setStreamEngine(se);
    }
}

void ResonanceEngine::reset() noexcept {
    for (auto& slot : voices_) { if (slot) slot->hardStop(); }
    for (auto& es : excite_state_) es.last_excite = 0.f;
}

void ResonanceEngine::setGainDb(float db) {
    gain_lin_.store(std::pow(10.f, db / 20.f), std::memory_order_relaxed);
}
float ResonanceEngine::gainLinear() const {
    return gain_lin_.load(std::memory_order_relaxed);
}
void ResonanceEngine::setLayerTargetDb(float db) {
    layer_target_db_.store(db, std::memory_order_relaxed);
}
float ResonanceEngine::layerTargetDb() const {
    return layer_target_db_.load(std::memory_order_relaxed);
}
void ResonanceEngine::setEnabled(bool on) {
    enabled_.store(on, std::memory_order_relaxed);
}
bool ResonanceEngine::enabled() const {
    return enabled_.load(std::memory_order_relaxed);
}
void ResonanceEngine::setCacheReady(const std::array<bool, 128>& ready) noexcept {
    for (int n = 0; n < 128; ++n)
        reso_cache_ready_[(size_t)n].store(ready[(size_t)n], std::memory_order_release);
}
void ResonanceEngine::clearCacheReady() noexcept {
    for (auto& f : reso_cache_ready_) f.store(false, std::memory_order_release);
}
void ResonanceEngine::requestRecacheFade() noexcept {
    recache_fade_request_.store(true, std::memory_order_relaxed);
}

void ResonanceEngine::setExciteDecayTimeMs(float tau_ms, int block_size, float engine_sr) {
    if (tau_ms <= 0.f || block_size <= 0 || engine_sr <= 0.f) return;
    // block_ms = block_size * 1000 / engine_sr; decay = exp(-block_ms/tau_ms).
    const float block_ms = (float)block_size * 1000.f / engine_sr;
    float d = std::exp(-block_ms / tau_ms);
    if (d < 0.f) d = 0.f;
    if (d > 1.f) d = 1.f;
    decay_per_block_.store(d, std::memory_order_relaxed);
}

void ResonanceEngine::onSelfNoteOn(int played_midi, float engine_sr) {
    // Rule B: note-on N kdyz rezonance N hraje → fast fade + zablokovani dalsi
    // eligibility. Eligibility filter (1) pak blokuje, dokud hraje main voice N.
    if (played_midi < 0 || played_midi >= 128) return;
    auto& slot = voices_[(size_t)played_midi];
    if (slot && slot->active() && !slot->fadingOut()) {
        slot->fadeOut(engine_sr);
        excite_state_[played_midi].last_excite = 0.f;
    }
}

void ResonanceEngine::onPlayedNoteOn(int played_midi, int velocity,
                                     const Bank& bank, const VoicePool& pool,
                                     const PedalState& pedal, float engine_sr) {
    if (played_midi < 0 || played_midi >= 128) return;
    if (!enabled_.load(std::memory_order_relaxed)) return;
    const float gain = gain_lin_.load(std::memory_order_relaxed);
    const float vel_norm = (float)velocity / 127.f;
    if (vel_norm <= 0.f) return;
    last_engine_sr_ = engine_sr;

    for (int N = 0; N < 128; ++N) {
        if (N == played_midi) continue;              // play-on-self
        // Levne O(1) testy nejdriv: harmonicka blizkost vyradi ~90 % not,
        // drahy pool scan (hasActiveMainVoice, O(pool)) az nakonec.
        const float harm = harmonicProximity(N, played_midi);
        if (harm < kResonanceHarmonicMin) continue;

        // Holy excitation BEZ damping multiplikatoru (damping aplikuje
        // processBlock per-blok pres setTargetGain — tim se zmeny pedalu
        // promitaji plynule do hlasitosti existujici rezonance).
        const float excite = vel_norm * harm * gain;
        if (excite < kResonanceExciteMinGain) continue;

        // Eligibility filter 5.5.1 (1):
        if (!pedal.isUndamped(N)) continue;          // damping <= eps → ineligibilni
        // Rule B in-progress: rezonance N prave fade-uje — neprzme to.
        auto& slot = voices_[(size_t)N];
        if (slot->active() && slot->fadingOut()) continue;
        if (pool.hasActiveMainVoice(N)) continue;    // main voice N existuje → ineligibilni

        if (slot->active()) {
            // Per-nota uniqueness 5.5.1 (2): existujici slot, jen aktualizuj
            // amplitudu. max() drzi nejvyssi z N excitaci v pohledu na blok
            // (smerodatne pro decay-derived target_gain v processBlock).
            excite_state_[N].last_excite =
                std::max(excite_state_[N].last_excite, excite);
            slot->addExcitation(excite);
            // DEBUG: existujici rezonance posilena. RT-safe (onPlayedNoteOn bezi
            // na audio threadu pres processBlock) — LOG_RT do lock-free ringu.
            LOG_RT_DEBUG("resonance",
                "EXCITE+ played=%d N=%d harm=%.3f excite=%.4f cc64=%d damping[N]=%.3f",
                played_midi, N, harm, excite, (int)pedal.sustainCC(),
                pedal.dampingFor(N));
            continue;
        }

        // Alokuj novy rezonancni hlas. Nejdriv najdi sampl pro N.
        const NoteSlots& ns = bank.notes[N];
        if (!ns.recorded) continue;
        const int si = nearestSlotByRms(ns, layer_target_db_.load(std::memory_order_relaxed));
        if (si < 0) continue;
        const VelocitySlot& vs = ns.slots[(size_t)si];
        if (vs.variants.empty() || vs.variants[0].mics.empty()) continue;  // nahravka chybi
        const SampleAsset& a = vs.variants[0];
        const MicLayer*    m = &a.mics[0];

        slot->setStreamEngine(stream_);   // hlasy predalokovane v konstruktoru

        float pl, pr;
        panForNote(N, pl, pr);

        // Initial target = excite * damping_[N]. Damping pri full sustain = 1.0,
        // pri half-pedal = ~0.5 → rezonance startuje tise. processBlock pak
        // udrzuje target podle aktualniho dampingu.
        const float init_gain = excite * pedal.dampingFor(N);
        // Half-pedal: damping muze srazit init_gain hluboko pod slysitelnost —
        // nealokuj ring + diskova cteni pro hlas, ktery nikdo neuslysi.
        if (init_gain < kResonanceExciteMinGain) continue;

        // Budget gate PRED spawnem: nealokuj ring + necti z disku pro hlas,
        // ktery by se stejne hned ztlumil. Steal jen kdyz je novy hlasitejsi nez
        // nejtissi prave znejici (vzor jako hlavni voice findSlot). Tim odpada
        // spawn-churn (drive: spawn VSECH harmonik → acquireRing + requestRead →
        // fadeOut pres budget; zbytecna diskova cteni hladovela streamujici
        // prezivajici rezonanci → underrun i pri MAX RESONANCE=1).
        // Uroven = max(currentLevel, targetGain): cerstve spawnuty hlas ma
        // gain_ jeste 0 (rampa ~30 ms), ale target uz plny — srovnani ciste
        // pres currentLevel() delalo z prave spawnutych hlasu nulove obeti
        // a spawn-churn se vracel uvnitr jedineho onPlayedNoteOn.
        {
            int   live = 0, quietest = -1;
            float qlevel = 1e30f;
            for (int k = 0; k < 128; ++k) {
                const auto& s = voices_[(size_t)k];
                if (!s->active() || s->fadingOut()) continue;
                ++live;
                const float lvl = std::max(s->currentLevel(), s->targetGain());
                if (lvl < qlevel) { qlevel = lvl; quietest = k; }
            }
            const int cap = max_voices_.load(std::memory_order_relaxed);
            if (live >= cap) {
                if (init_gain <= qlevel) continue;   // nepreznel by → nespawnuj
                if (quietest >= 0) {                 // jinak uvolni nejtissi slot
                    voices_[(size_t)quietest]->fadeOut(engine_sr);
                    excite_state_[quietest].last_excite = 0.f;
                }
            }
        }
        // DEBUG: novy rezonancni hlas alokovan. Pokud init_gain > 0 pri cc64=0,
        // damping[N] musi byt > 0 → buď N je drzene (main voice eligibility
        // filter to ma blokovat), nebo damping nevynulovany pri lift.
        // RT-safe (audio thread) — LOG_RT do lock-free ringu.
        LOG_RT_DEBUG("resonance",
            "SPAWN  played=%d N=%d harm=%.3f excite=%.4f init_gain=%.4f cc64=%d damping[N]=%.3f",
            played_midi, N, harm, excite, init_gain, (int)pedal.sustainCC(),
            pedal.dampingFor(N));
        const bool use_cache = reso_cache_ready_[(size_t)N].load(std::memory_order_acquire);
        slot->start(N, m, init_gain, pl, pr, engine_sr, use_cache);
        excite_state_[N].last_excite = excite;
    }
    // Po smycce: dorovnej budget (resi i ZIVE snizeni MAX RESONANCE sliderem —
    // ztlumi prebytecne nejtissi). Spawn-churn uz vyresil gate vyse, takze tady
    // se uz jen pripadne dorovna po zmene budgetu (jinak no-op).
    enforceVoiceBudget(engine_sr);
}

bool ResonanceEngine::processBlock(float* out_l, float* out_r, int n_samples,
                                   const PedalState& pedal) noexcept {
    if (recache_fade_request_.exchange(false, std::memory_order_relaxed)) {
        for (auto& slot : voices_)
            if (slot && slot->active() && !slot->fadingOut())
                slot->fadeOut(last_engine_sr_);
    }
    // 1) Per-blok decay last_excite + update target_gain podle pedalu.
    //    Aplikujeme jen na hlasy, ktere NEJSOU ve fade-out (rule B / target=0)
    //    — ty si rampu drzi po cele dobe fade-out.
    const float decay = decay_per_block_.load(std::memory_order_relaxed);
    for (int N = 0; N < 128; ++N) {
        auto& slot = voices_[(size_t)N];
        if (!slot || !slot->active() || slot->fadingOut()) continue;
        excite_state_[N].last_excite *= decay;
        const float target = excite_state_[N].last_excite * pedal.dampingFor(N);
        slot->setTargetGain(target);
    }
    // 2) Render vsech hlasu. Kazdy hlas si sam zaznamena active_=false pri
    //    dosazeni nulovho gainu — pri pristim onPlayedNoteOn ho mozeme zalozit
    //    od znova (slot zustava alokovany, jen active()=false).
    bool any = false;
    for (int N = 0; N < 128; ++N) {
        auto& slot = voices_[(size_t)N];
        if (!slot) continue;
        if (slot->process(out_l, out_r, n_samples)) any = true;
    }
    return any;
}

int ResonanceEngine::activeCount() const noexcept {
    int n = 0;
    for (const auto& slot : voices_) {
        if (slot && slot->active()) n++;
    }
    return n;
}

bool ResonanceEngine::isResonating(int midi) const noexcept {
    if (midi < 0 || midi >= 128) return false;
    const auto& slot = voices_[(size_t)midi];
    return slot && slot->active();
}

float ResonanceEngine::currentLevelFor(int midi) const noexcept {
    if (midi < 0 || midi >= 128) return 0.f;
    const auto& slot = voices_[(size_t)midi];
    if (!slot || !slot->active()) return 0.f;
    return slot->currentLevel();
}

void ResonanceEngine::enforceVoiceBudget(float engine_sr) {
    // Krad pri prekroceni rozpoctu: najdi nejtissi NE-fadingOut hlas a fadeOut.
    // KRITICKE: do limitu pocitame jen NE-fadingOut hlasy. Fade-out hlasy uz
    // sami umiraji (active() zustava true dokud gain nedohaje), takze kdyby se
    // pocitaly do limitu, jedno prekroceni by spustilo fadeOut na VSECHNY hlasy
    // (activeCount se po fadeOut nezmensi → smycka fade-uje dal, dokud nezbude
    // jen fading → counter spadne na ~0, rezonance jen problikne). Pocitame
    // tedy "kolik hlasu jeste plnohodnotne zni" a fade-ujeme jen pres ten limit.
    auto liveCount = [this]() {
        int n = 0;
        for (const auto& slot : voices_)
            if (slot && slot->active() && !slot->fadingOut()) ++n;
        return n;
    };
    const int cap = max_voices_.load(std::memory_order_relaxed);
    while (liveCount() > cap) {
        int   victim_idx   = -1;
        float victim_level = 1e30f;
        for (int N = 0; N < 128; ++N) {
            const auto& slot = voices_[(size_t)N];
            if (!slot || !slot->active() || slot->fadingOut()) continue;
            const float lvl = slot->currentLevel();
            if (lvl < victim_level) { victim_level = lvl; victim_idx = N; }
        }
        if (victim_idx < 0) break;  // nic ne-fadujiciho — nic vic neudelame
        voices_[(size_t)victim_idx]->fadeOut(engine_sr);
        excite_state_[victim_idx].last_excite = 0.f;
    }
}

void ResonanceEngine::panForNote(int midi, float& pan_l, float& pan_r) {
    // Stejny vzorec jako VoicePool::panForNote(midi, kResonanceKeyboardSpread, ...).
    constexpr float kPi = 3.14159265f;
    const float angle = (kPi / 4.f)
                      + ((float)midi - 64.5f) / 87.f
                        * kResonanceKeyboardSpread * 0.5f;
    pan_l = std::cos(angle);
    pan_r = std::sin(angle);
}

} // namespace ithaca
