// engine/voice/voice_pool.cpp — viz voice_pool.h.
#include "voice/voice_pool.h"

#include "pedal/pedal_state.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ithaca {

namespace {
// Pan z MIDI noty: stred + rozprostreni dle vzdalenosti od stredu klaviatury
// (~midi 64.5).
void panForNote(int midi, float spread, float& pan_l, float& pan_r) {
    constexpr float kPi = 3.14159265f;
    float angle = (kPi / 4.f) + ((float)midi - 64.5f) / 87.f * spread * 0.5f;
    pan_l = std::cos(angle);
    pan_r = std::sin(angle);
}
} // namespace

VoicePool::VoicePool(int pool_size) {
    int n = (std::max)(1, (std::min)(pool_size, kMaxPoolSize));
    voices_.resize((size_t)n);
}

void VoicePool::setStreamEngine(StreamEngine* se) {
    for (auto& v : voices_) v.setStreamEngine(se);
}

void VoicePool::reset() noexcept {
    for (auto& v : voices_) v.hardStop();
    std::memset(note_active_count_, 0, sizeof(note_active_count_));
}

int VoicePool::findSlot(const PedalState* pedal) {
    // 1. Volny slot — vzdy preferovany.
    for (int i = 0; i < (int)voices_.size(); ++i)
        if (!voices_[i].active()) return i;
    // 2. Pool je plny. Preferuj kradez RELEASING hlasu (= po note-off, mizi
    //    sami od sebe). Mezi releasing vyber nejtissi.
    int best_rel = -1;
    float best_rel_level = 1e30f;
    for (int i = 0; i < (int)voices_.size(); ++i) {
        if (!voices_[i].releasing()) continue;
        float lvl = voices_[i].currentLevel();
        if (lvl < best_rel_level) { best_rel_level = lvl; best_rel = i; }
    }
    if (best_rel >= 0) return best_rel;
    // 3. Vsechny hlasy jsou ne-releasing. Pri DRZENEM pedalu pravdepodobne
    //    plno tonu, ktere uzivatel uz pustil ale pedal je drzi v sustainu.
    //    Kdyz mame PedalState, krad tise NE-DRZENE (pedal-sustained) hlasy
    //    pred HELD hlasy (uzivatel je drzi → uziva je). Bez PedalState
    //    spadne na puvodni "nejtissi z celeho poolu".
    if (pedal) {
        int best_sus = -1;
        float best_sus_level = 1e30f;
        for (int i = 0; i < (int)voices_.size(); ++i) {
            // ne-releasing + ne-held uzivatelem → sustained pedalem.
            if (pedal->isHeld(voices_[i].midi())) continue;
            float lvl = voices_[i].currentLevel();
            if (lvl < best_sus_level) { best_sus_level = lvl; best_sus = i; }
        }
        if (best_sus >= 0) return best_sus;
    }
    // 4. Vsechny hlasy jsou HELD (uzivatel drzi vsechny klavesy a pool je
    //    presto plny — extrem) — krad nejtissi z celeho poolu.
    int best = 0;
    float best_level = voices_[0].currentLevel();
    for (int i = 1; i < (int)voices_.size(); ++i) {
        float lvl = voices_[i].currentLevel();
        if (lvl < best_level) { best_level = lvl; best = i; }
    }
    return best;
}

void VoicePool::noteOn(int midi, const VoiceSpec& spec, float engine_sr,
                       float keyboard_spread, const PedalState* pedal) {
    if (!spec.asset) return;

    // Retrigger: pokud uz nektery hlas hraje tuto notu (V JAKEMKOLI STAVU
    // vc. releasing a pending_release), damp ho (click-free). Invariant 5.5.1:
    // NIKDY 2 hlavni hlasy pro stejnou midi. Drive zde byl filtr `!releasing()`,
    // ktery povolil koexistenci releasing voice + retrigger voice → akumulace
    // hlasu + ring leak pri opakovanem retriggeru pod pedalem.
    for (auto& v : voices_)
        if (v.active() && v.midi() == midi) {
            v.prepareDamp(engine_sr);                  // deaktivuje hlas
            if (note_active_count_[midi]) note_active_count_[midi]--;
        }

    int slot = findSlot(pedal);
    Voice& v = voices_[slot];

    // DEBUG diagnostika stealu/noteOn frekvence. RT-safe: LOG_RT_* do lock-free
    // ringu (audio thread nesmi zamykat log mutex — priority inversion).
    {
        int active = activeCount();
        int releasing = 0, held = 0;
        for (const auto& vv : voices_) {
            if (!vv.active()) continue;
            if (vv.releasing()) releasing++;
            if (pedal && pedal->isHeld(vv.midi())) held++;
        }
        if (v.active() && v.midi() != midi) {
            LOG_RT_WARN("voice_steal",
                "STEAL victim_midi=%d victim_lvl=%.3f victim_releasing=%d "
                "victim_held=%d → new_midi=%d new_vel=%.2f pool=%d/%d "
                "rel=%d held=%d",
                v.midi(), v.currentLevel(), (int)v.releasing(),
                pedal ? (int)pedal->isHeld(v.midi()) : -1,
                midi, spec.vel_gain, active, (int)voices_.size(),
                releasing, held);
        } else {
            LOG_RT_INFO("voice_on",
                "noteOn midi=%d vel=%.2f slot=%d pool=%d/%d rel=%d held=%d",
                midi, spec.vel_gain, slot, active, (int)voices_.size(),
                releasing, held);
        }
    }

    // Kdyz krademe aktivni hlas, damp i jeho (jiny ton) → bez lupnuti.
    if (v.active()) {
        const int om = v.midi();
        v.prepareDamp(engine_sr);                      // deaktivuje obet
        if (om >= 0 && om < 128 && note_active_count_[om])
            note_active_count_[om]--;
    }

    float pl, pr;
    panForNote(midi, keyboard_spread, pl, pr);
    v.start(spec.asset, spec.pitch_ratio, spec.vel_gain, pl, pr, engine_sr);
    v.setMidi(midi);
    if (v.active()) note_active_count_[midi]++;        // start mohl selhat (bez micu)
}

void VoicePool::noteOff(int midi, float release_ms, float engine_sr) {
    for (auto& v : voices_)
        if (v.active() && v.midi() == midi && !v.releasing())
            v.release(release_ms, engine_sr);
}

void VoicePool::noteOffWithPedal(int midi, const PedalState& pedal,
                                 float release_ms, float engine_sr) {
    // Pokud pedal sustainuje danou strunu, oznac vsechny aktivni (ne-releasing)
    // hlasy te noty jako pending_release — sample hraje dal, az pedal pustis,
    // releasePendingNotes na ne spusti release.
    if (pedal.isUndamped(midi)) {
        for (auto& v : voices_)
            if (v.active() && v.midi() == midi && !v.releasing())
                v.markPendingRelease();
        return;
    }
    // Pedal nesustainuje → normalni release ihned.
    for (auto& v : voices_)
        if (v.active() && v.midi() == midi && !v.releasing())
            v.release(release_ms, engine_sr);
}

void VoicePool::releasePendingNotes(const PedalState& pedal,
                                    float release_ms, float engine_sr) {
    // Pedal byl pusten. Pro kazdy hlas s pending_release: pokud uzivatel
    // klavesu nedrzi (pedal.isHeld == false), spust release ted. Drzene
    // klavesy zustanou hrat — uzivatel je drzi, tlumitka nedosednou.
    for (auto& v : voices_) {
        if (!v.active() || v.releasing()) continue;
        if (!v.isPendingRelease()) continue;
        if (pedal.isHeld(v.midi())) {
            // Nota se mezitim znovu drzi — pending zrusit, sample hraje dal.
            v.clearPendingRelease();
            continue;
        }
        v.clearPendingRelease();
        v.release(release_ms, engine_sr);
    }
}

void VoicePool::allNotesOff(float release_ms, float engine_sr) {
    for (auto& v : voices_)
        if (v.active() && !v.releasing())
            v.release(release_ms, engine_sr);
}

bool VoicePool::processBlock(float* out_l, float* out_r, int n_samples,
                             float engine_sr) noexcept {
    (void)engine_sr;
    bool any = false;
    for (auto& v : voices_) {
        // Dampujici hlas po prepareDamp je !active(), ale crossfade ocas MUSI
        // doznit i kdyz novy hlas dostal jiny slot (jinak tvrdy strih = lupnuti
        // + "duch" zdedeny pozdejsim noteOn na tomto slotu).
        if (!v.active() && !v.isDamping()) continue;
        const bool was = v.active();
        if (v.process(out_l, out_r, n_samples)) any = true;
        if (was && !v.active()) {                      // deaktivace v process
            const int m = v.midi();
            if (m >= 0 && m < 128 && note_active_count_[m])
                note_active_count_[m]--;
        }
    }
    return any;
}

int VoicePool::activeCount() const noexcept {
    int n = 0;
    for (const auto& v : voices_) if (v.active()) n++;
    return n;
}

bool VoicePool::hasActiveMainVoice(int midi) const noexcept {
    // Eligibility filter 5.5.1 (1): rezonance N je ineligible, dokud existuje
    // hlavni hlas N v jakemkoli stavu (HELD / RELEASING / pedal-sustained
    // dozvuk). O(1) pres per-nota citac (drive O(pool) scan volany az 127x
    // na jeden note-on z rezonancni eligibility).
    return midi >= 0 && midi < 128 && note_active_count_[midi] > 0;
}

} // namespace ithaca
