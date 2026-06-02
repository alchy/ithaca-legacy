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
    static std::vector<float> L, R;
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
    cfg.sample_rate          = state.audio_sample_rate;
    cfg.block_size           = state.audio_block_size;
    cfg.master_gain          = std::pow(10.f, state.master_gain_db / 20.f);
    cfg.resonance_strength   = state.resonance_strength;
    cfg.release_ms           = state.release_ms;
    cfg.excite_decay_ms      = state.excite_decay_ms;
    cfg.max_resonance_voices = state.max_resonance_voices;
    if (!engine.init(cfg)) {
        log::Logger::default_().log("gui", log::Severity::Error,
            "Engine init selhal");
        return false;
    }

    // Aplikuj perzistovane DSP parametry na chain (poradi parametru = Param
    // tabulky stage: AGC[target,release,floor], BBE[definition,bass],
    // LIMITER[threshold_db,release_ms]).
    {
        auto& ch = engine.dspChain();
        auto& agc = ch.stage(0); auto& bbe = ch.stage(1); auto& lim = ch.stage(2);
        agc.set(0, state.agc_target); agc.set(1, state.agc_release_ms); agc.set(2, state.agc_floor);
        agc.setEnabled(state.agc_enabled);
        bbe.set(0, state.bbe_definition); bbe.set(1, state.bbe_bass);
        bbe.setEnabled(state.bbe_enabled);
        lim.set(0, state.limiter_threshold_db); lim.set(1, state.limiter_release_ms);
        lim.setEnabled(state.limiter_enabled);
    }
    // Zivý strop rezonancni polyfonie (uz predano pres cfg, ale explicitne
    // volame setter aby cesta setMaxResonanceVoices byla vzdy exercisovana).
    engine.setMaxResonanceVoices(state.max_resonance_voices);

    // Bank: best-effort. Pokud cesta neni nebo nejde nacist, jen warning a
    // engine bezi prazdny (uzivatel muze banku vybrat pozdeji pres UI).
    if (!state.bank_path.empty()) {
        if (!engine.loadBank(state.bank_path)) {
            log::Logger::default_().log("gui", log::Severity::Warning,
                "Nelze nacist banku: %s", state.bank_path.c_str());
        }
    }

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
                if (midi.open(engine, (int)i)) {
                    state.midi_port_name = ports[i];   // ulozit presnou jmenovku
                    midi.setChannel(state.midi_channel);
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
    return true;
}

void AppContext::shutdown() {
    // Poradi: nejdrive MIDI (zavre callback thread, aby uz necpel noty do
    // engine), pak audio (zastavi miniaudio callback, uz nikdo nesahne na
    // engine), nakonec subscribery (zadne log eventy se nepokusi sahnout na
    // nase log_buf).
    if (midi.isOpen()) midi.close();
    if (audio) audio->stop();
    log::Logger::default_().clearSubscribers();
}

void AppContext::setAudioBlockSize(int n) {
    const int applied = engine.setBlockSize(n);   // clamp 32..8192 + re-prepare
    state.audio_block_size = applied;
    if (audio) {
        audio->stop();   // joinne miniaudio callback → zadny RT race s restart
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
