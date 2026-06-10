// app/gui/app_context.cpp - viz app_context.h.
//
// Audio callback: AudioDevice predava free-funkci + userdata. Pouzivame Engine*
// jako userdata, scratch L/R buffery jsou static uvnitr cb (audio thread je
// jediny konzument). Stejny vzor jako app/cli/main.cpp::playAudioCb.
#include "app_context.h"

#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ithaca::gui {

namespace {

// Audio thread callback. userdata = Engine*. Engine renderuje do L/R scratch,
// my interleavujem do miniaudio bufferu.
void audioCallback(void* userdata, float* output, uint32_t frames) {
    auto* eng = static_cast<ithaca::Engine*>(userdata);
    // Predalokovano na engine max (8192) — zadna alokace na audio threadu.
    // Guard zustava jen defenzivne (nemelo by nastat).
    static std::vector<float> L(8192), R(8192);
    if ((uint32_t)L.size() < frames) { L.resize(frames); R.resize(frames); }
    std::fill(L.begin(), L.begin() + frames, 0.f);
    std::fill(R.begin(), R.begin() + frames, 0.f);
    eng->processBlock(L.data(), R.data(), (int)frames);
    for (uint32_t i = 0; i < frames; ++i) {
        output[i * 2]     = L[i];
        output[i * 2 + 1] = R[i];
    }
}

} // namespace

bool AppContext::initFromState(const GuiState& s) {
    state = s;

    // Min severity z perzistovaneho/CLI-overridnuteho log_level. Nastavit PRED
    // engine.init(), aby i bank-load logy ctily zvolenou uroven.
    log::Logger::default_().setMinSeverity(
        log::severity_from_string(state.log_level.c_str(), log::Severity::Info));

    // Subscriber loggeru → ring buffer. Pripojit PRED engine.init(), aby
    // GUI strip videla i init logy (bank load, stream threads spawn, atd.).
    // Pozn.: Logger drzi callback by-value; lambda zachycuje `this` — AppContext
    // musi prezit do shutdown(), kde subscribery clearneme.
    log::Logger::default_().addSubscriber(
        [this](const log::LogEntry& e) { log_buf.push(e); });

    // Engine config z GuiState. master_gain je v dB v GUI, prevod na linear.
    ithaca::EngineConfig cfg;
    // Validace perzistovanych audio hodnot (JSON muze byt rucne editovany).
    // SR <= 0 → fallback 48000 (jinak deleni nulou v load metru / pos_inc).
    // block_size clamp na [32, 8192] (stejne meze jako Engine::setBlockSize).
    cfg.sample_rate          = state.audio_sample_rate > 0 ? state.audio_sample_rate : 48000;
    cfg.block_size           = std::clamp(state.audio_block_size, 32, 8192);
    state.audio_sample_rate  = cfg.sample_rate;   // promitni validovane zpet do state
    state.audio_block_size   = cfg.block_size;
    cfg.master_gain          = std::pow(10.f, state.master_gain_db / 20.f);
    cfg.resonance_enabled    = state.resonance_enabled;
    cfg.resonance_gain_db    = state.resonance_gain_db;
    cfg.resonance_layer_db   = state.resonance_layer_db;
    cfg.release_ms           = state.release_ms;
    cfg.excite_decay_ms      = state.excite_decay_ms;
    cfg.max_resonance_voices = state.max_resonance_voices;
    cfg.resonance_window_ms  = state.resonance_window_ms;  // jen JSON, ne GUI
    cfg.preload_ms           = state.preload_ms;           // jen JSON, ne GUI
    cfg.cache_budget_mb      = state.cache_budget_mb;      // jen JSON, ne GUI (0=auto)
    cfg.rt_priority          = true;   // realna audio aplikace → RT priorita audio threadu
    if (!engine.init(cfg)) {
        log::Logger::default_().log("gui", log::Severity::Error,
            "Engine init selhal");
        return false;
    }

    // Aplikuj perzistovane DSP parametry na chain (poradi parametru = Param
    // tabulky stage: AGC[target,release,floor], ENHANCER[process,contour,mid],
    // LIMITER[threshold_db,release_ms]).
    {
        auto& ch = engine.dspChain();
        auto& cv  = ch.stage(0);   // CONVOLVER
        auto& agc = ch.stage(1);   // AGC
        auto& enh = ch.stage(2);   // ENHANCER
        auto& lim = ch.stage(3);   // LIMITER
        cv.set(0, state.convolver_mix);
        cv.set(1, state.convolver_decay);
        cv.set(2, state.convolver_tone);
        cv.set(3, state.convolver_size);
        if (state.convolver_choice > 0) cv.selectChoice(state.convolver_choice);
        cv.setEnabled(state.convolver_enabled);
        agc.set(0, state.agc_target); agc.set(1, state.agc_release_ms); agc.set(2, state.agc_floor);
        agc.setEnabled(state.agc_enabled);
        enh.set(0, state.enhancer_process); enh.set(1, state.enhancer_contour); enh.set(2, state.enhancer_mid);
        enh.setEnabled(state.enhancer_enabled);
        lim.set(0, state.limiter_threshold_db); lim.set(1, state.limiter_release_ms);
        lim.setEnabled(state.limiter_enabled);
    }
    // Zivý strop rezonancni polyfonie (uz predano pres cfg, ale explicitne
    // volame setter aby cesta setMaxResonanceVoices byla vzdy exercisovana).
    engine.setMaxResonanceVoices(state.max_resonance_voices);
    engine.setResonanceEnabled(state.resonance_enabled);
    engine.setResonanceGainDb(state.resonance_gain_db);

    // Bank se nacita ASYNC (requestBankReload na konci initu) — okno se ukaze
    // hned, prubeh kryje modalni overlay. Layer heuristika "1/3 rozsahu banky"
    // se aplikuje v pollReloadCompletion (po dokonceni loadu).
    engine.setResonanceLayerDb(state.resonance_layer_db);

    // Audio device start. AudioCallback je free funkce + userdata (Engine*).
    // Musi byt AZ po engine.init() (voice pool / stream / ringy uz priprazene).
    audio = std::make_unique<ithaca::AudioDevice>();
    if (!audio->start(&audioCallback, &engine, cfg.sample_rate, cfg.block_size)) {
        log::Logger::default_().log("gui", log::Severity::Warning,
            "Nelze otevrit audio device");
        // Bez audio device GUI stale funguje (uzivatel uvidi engine metriky a
        // logy); nevracime false, ale logujem.
    }

    // MIDI: substring match nad listPorts(). Bez matche jen warning; uzivatel
    // muze MIDI port vybrat pozdeji v Settings panelu (Task 8+).
    if (!state.midi_port_name.empty()) {
        bool opened = false;
        const auto ports = ithaca::MidiInput::listPorts();
        for (size_t i = 0; i < ports.size(); ++i) {
            if (ports[i].find(state.midi_port_name) != std::string::npos) {
                // Kanal nastavit PRED open — callback muze bezet hned po
                // otevreni portu a filtroval by podle stareho kanalu.
                midi.setChannel(state.midi_channel);
                if (midi.open(engine, (int)i)) {
                    state.midi_port_name = ports[i];   // ulozit presnou jmenovku
                    opened = true;
                    break;
                }
            }
        }
        if (!opened) {
            log::Logger::default_().log("gui", log::Severity::Warning,
                "MIDI port nenalezen: %s", state.midi_port_name.c_str());
        }
    }

    // Startovni load banky jede async stejnou cestou jako reload — okno se
    // ukaze hned, prubeh kryje modalni overlay v render smycce.
    if (!state.bank_path.empty()) requestBankReload(state.bank_path);
    return true;
}

void AppContext::requestBankReload(const std::string& dir) {
    bool expected = false;
    if (!reload_in_progress_.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel))
        return;                                       // uz bezi (modal blokuje UI)
    if (reload_thread_.joinable()) reload_thread_.join();   // predchozi dobehl
    reload_dir_ = dir;
    load_progress_.phase.store(0);
    load_progress_.done.store(0);
    load_progress_.total.store(0);
    load_progress_.bytes_loaded.store(0);
    load_progress_.truncated.store(false);
    reload_thread_ = std::thread([this] {
        const bool ok = engine.reloadBank(reload_dir_, &load_progress_);
        reload_ok_.store(ok, std::memory_order_release);
        reload_done_pending_.store(true, std::memory_order_release);
        reload_in_progress_.store(false, std::memory_order_release);
    });
}

void AppContext::pollReloadCompletion() {
    if (!reload_done_pending_.exchange(false, std::memory_order_acq_rel)) return;
    bank_truncated_ = load_progress_.truncated.load(std::memory_order_relaxed);
    if (!reload_ok_.load(std::memory_order_acquire)) {
        log::Logger::default_().log("gui", log::Severity::Warning,
            "Nelze nacist banku: %s", reload_dir_.c_str());
        return;
    }
    // Default Resonance Layer = 1/3 rozsahu banky, kdyz uzivatel drzi default
    // (-30 dB). Zmena state.resonance_layer_db spusti existujici 400ms layer
    // debounce v main.cpp → engine.rebuildResonanceCache (vyresi i nalez
    // H-stredni z revize: heuristika driv bezela bez rebuilu cache).
    const float lo = engine.bankPeakRmsMinDb(), hi = engine.bankPeakRmsMaxDb();
    if (hi > lo && state.resonance_layer_db == -30.f)
        state.resonance_layer_db = lo + (hi - lo) / 3.f;
    log::Logger::default_().log("gui", log::Severity::Info,
        "Banka nactena: %s (%d not, %d samplu)%s", reload_dir_.c_str(),
        engine.recordedNotes(), engine.loadedSamples(),
        bank_truncated_ ? " — NEUPLNA (RAM budget)" : "");
}

void AppContext::shutdown() {
    // Reload nelze prerusit (drzi bank_loading_) — pockej na dobeh PRED
    // zavrenim MIDI/audia (quiesce handshake vyuzije bezici audio).
    if (reload_thread_.joinable()) reload_thread_.join();
    // Poradi: nejdrive MIDI (zavre callback thread, aby uz necpel noty do
    // engine), pak audio (zastavi miniaudio callback, uz nikdo nesahne na
    // engine), nakonec subscribery (zadne log eventy se nepokusi sahnout na
    // nase log_buf).
    if (midi.isOpen()) midi.close();
    if (audio) audio->stop();
    log::Logger::default_().clearSubscribers();
}

void AppContext::setAudioBlockSize(int n) {
    // Poradi je kriticke: stop() PRVNI (joinne miniaudio callback), teprve pak
    // setBlockSize() mutuje engine stav (cfg, DSP koeficienty, rezonance decay,
    // refill prah). Jinak by re-prepare bezel souběžne s in-flight processBlock
    // na audio threadu = data race.
    if (audio) audio->stop();
    const int applied = engine.setBlockSize(n);   // clamp 32..8192 + re-prepare
    state.audio_block_size = applied;
    if (audio) {
        if (!audio->start(&audioCallback, &engine, engine.sampleRate(), applied)) {
            log::Logger::default_().log("gui", log::Severity::Warning,
                "Restart audio device s block=%d selhal", applied);
        } else {
            log::Logger::default_().log("gui", log::Severity::Info,
                "Audio buffer zmenen na %d framu", applied);
        }
    }
}

} // namespace ithaca::gui
