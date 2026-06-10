// app/gui/app_context.h - drzi Engine + AudioDevice + MidiInput + log buffer
//                          + persistovany GuiState. Lifecycle = jeden ithaca-gui run.
//
// Vlastnictvi: AppContext je single-owner vsech tezkych objektu (engine, audio
// device, midi). main() vola initFromState() pred render loopem a shutdown()
// pred destrukci ImGui/GLFW. Pripadne dalsi panely v dalsich tascich budou cist
// ctx.engine / ctx.log_buf / ctx.state pres const&.
#pragma once

#include "engine.h"
#include "io/audio_device.h"
#include "midi/midi_input.h"
#include "sample/sample_store.h"   // BankLoadProgress

#include "log_subscriber.h"
#include "persistence.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace ithaca::gui {

struct AppContext {
    ithaca::Engine                       engine;
    std::unique_ptr<ithaca::AudioDevice> audio;
    ithaca::MidiInput                    midi;
    LogRingBuffer                        log_buf;
    GuiState                             state;

    // Init: subscriber pripoji, engine init z state, audio start, optional
    // bank load + MIDI open. Vraci true pri uspechu (engine init musi projit;
    // bank/MIDI failures jsou jen warning).
    bool initFromState(const GuiState& s);

    // Runtime zmena audio bufferu (BUFFER combo). Zastavi audio device, prenastavi
    // engine block size, znovu nastartuje device a aktualizuje state.audio_block_size.
    // Kratky audio gap je ocekavany (uzivatelska akce). Volat z GUI threadu.
    void setAudioBlockSize(int n);

    // Cisty shutdown: midi close, audio stop, subscriber clear. Volat pred
    // destrukci ImGui/GLFW. Engine destruktor sam uvolni voice/stream/resonance.
    void shutdown();

    // -- Async reload banky (spec 2026-06-10, cast A) --
    // requestBankReload: spusti worker thread, ktery vola engine.reloadBank
    // (engine ochrany bank_loading_/epoch zustavaji). Druhe volani behem behu
    // = no-op (modalni overlay stejne blokuje UI). Completion zpracuje
    // pollReloadCompletion() na GUI vlakne (layer heuristika, truncated, log).
    void requestBankReload(const std::string& dir);
    bool reloadInProgress() const {
        return reload_in_progress_.load(std::memory_order_acquire);
    }
    void pollReloadCompletion();
    const ithaca::BankLoadProgress& loadProgress() const { return load_progress_; }

    std::thread              reload_thread_;
    std::atomic<bool>        reload_in_progress_{false};
    std::atomic<bool>        reload_done_pending_{false};
    std::atomic<bool>        reload_ok_{false};
    std::string              reload_dir_;        // psano PRED spawnem threadu
    ithaca::BankLoadProgress load_progress_;
    // Posledni load prekrocil RAM budget → banka NEUPLNA (badge v BANK panelu;
    // detail je v LOG stripu z loggeru). Cte/pise jen GUI vlakno.
    bool                     bank_truncated_ = false;
};

} // namespace ithaca::gui
