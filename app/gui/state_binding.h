#pragma once
// app/gui/state_binding.h — most mezi GuiState a Engine.
// ----------------------------------------------------------------------------
// Vydeleno z AppContext::initFromState, ktere bylo netestovatelne: konstrukce
// EngineConfigu a validace persistovanych hodnot byly zamotane dohromady se
// startem audio device, otevrenim MIDI a nactenim banky. Tyhle dve funkce jsou
// ciste (resp. potrebuji jen Engine) a maji vlastni testy.
#include "persistence.h"
#include "engine.h"

#include <algorithm>
#include <cmath>

namespace ithaca::gui {

// GuiState -> EngineConfig. state.json muze byt rucne editovany, takze se
// hodnoty VALIDUJI a validovane se zapisou ZPET do `s` — jinak by GUI ukazovalo
// neco jineho, nez cim engine skutecne jede, a nesmysl by se pri pristim
// ulozeni zapsal znovu.
inline ithaca::EngineConfig engineConfigFromState(GuiState& s) {
    ithaca::EngineConfig cfg;

    // SR <= 0 by delilo nulou v load metru / pos_inc → fallback 48000.
    // block_size clamp na stejne meze jako Engine::setBlockSize.
    cfg.sample_rate = (s.audio_sample_rate > 0) ? s.audio_sample_rate : 48000;
    cfg.block_size  = std::clamp(s.audio_block_size, 32, 8192);
    s.audio_sample_rate = cfg.sample_rate;    // validovane zpet do state
    s.audio_block_size  = cfg.block_size;

    cfg.master_gain          = std::pow(10.f, s.master_gain_db / 20.f);  // dB → lin
    cfg.release_ms           = s.release_ms;
    cfg.resonance_enabled    = s.resonance_enabled;
    cfg.resonance_gain_db    = s.resonance_gain_db;
    cfg.resonance_layer_db   = s.resonance_layer_db;
    cfg.excite_decay_ms      = s.excite_decay_ms;
    cfg.max_resonance_voices = s.max_resonance_voices;
    cfg.resonance_window_ms  = s.resonance_window_ms;   // jen JSON, ne GUI
    cfg.preload_ms           = s.preload_ms;            // jen JSON, ne GUI
    cfg.cache_budget_mb      = s.cache_budget_mb;       // jen JSON, ne GUI (0=auto)
    // GUI je realna audio aplikace → audio thread si zvedne RT prioritu.
    // Default v EngineConfig je false, aby ji testy a offline render nedostaly.
    cfg.rt_priority          = true;
    return cfg;
}

// Runtime settery, ktere nejsou soucasti EngineConfig (nebo je chceme
// explicitne projet i pri startu, at je setter cesta vzdy exercisovana).
inline void applyStateToEngine(ithaca::Engine& e, const GuiState& s) {
    e.setMaxResonanceVoices(s.max_resonance_voices);
    e.setResonanceEnabled(s.resonance_enabled);
    e.setResonanceGainDb(s.resonance_gain_db);
    // Perzistovana hodnota. Heuristika "1/3 rozsahu banky" se aplikuje az
    // v pollReloadCompletion() — tady banka jeste nactena neni.
    e.setResonanceLayerDb(s.resonance_layer_db);
}

} // namespace ithaca::gui
