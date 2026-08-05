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
#include "motion.h"
#include "persistence.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ithaca::gui {

// Jedna banka nabidnuta v prohlizeci. `dir` je plna cesta, `name` jen posledni
// slozka (to, co se zobrazuje).
struct BankEntry {
    std::string dir;
    std::string name;
};

// Stav panelu, ktery musi prezit mezi framy: cache seznamu (jejich porizeni je
// drahe), animace a sample-and-hold citace indikatoru. Drive to byly
// function-local `static` promenne primo v render funkcich — skryty globalni
// stav, ktery nesel ani otestovat, ani resetovat pri reloadu.
struct PanelState {
    // -- Navigace --
    int page      = 0;   // PLAY BANK TONE RESO DSP SYS LOG
    int dsp_stage = 0;   // podzalozka na strance DSP

    // -- MIDI porty --
    // listPorts() konstruuje RtMidi klienta (OS IPC), takze per-frame volani
    // bylo nejdrazsi operace celeho GUI. Rescan jen pri prvnim frame a RESCANem.
    std::vector<std::string> midi_ports;
    bool                     midi_ports_scanned = false;

    // -- Prohlizec bank --
    // Kdyz neni nastaveny bank_search_dir, prochazi se adresare od startovniho.
    // V seznamu jsou JEN adresare, ktere vypadaji jako banka (levna sonda),
    // ne cely obsah filesystemu.
    std::string              browse_dir;
    std::vector<BankEntry>   banks;
    bool                     banks_valid = false;

    // -- Vytah v PLAY --
    // Posun je ve VIRTUALNICH pixelech: index * row_h. Klepnuti nastavi cil,
    // tah hybe primo a po pusteni se dojede k nejblizsimu radku. Nacteni banky
    // se spusti az v okamziku USTALENI, ne behem rolovani — jinak by projeti
    // seznamu spustilo desitky loadu za sebou.
    motion::Settle reel;
    int   reel_sel      = 0;
    bool  reel_dragging = false;
    float reel_grab0    = 0.f;   // posun na zacatku tahu
    bool  reel_armed    = false; // ceka se na ustaleni, pak nacist

    // -- LOG --
    bool log_unseen = false;     // kontrolka sviti, dokud se stranka neotevre

    // -- Sample-and-hold pro ciselne indikatory (max za 400ms okno) --
    // Bez toho by cisla pri 60 fps necitelne blikala.
    struct Hold { float shown = 0.f, winmax = 0.f, t0 = 0.f; };
    Hold h_voices, h_reso, h_main_rings, h_reso_rings, h_load;

    // Scratch pro snapshot LOG stranky. Predalokovany, aby se LogEntry
    // (kazdy 2x std::string) nealokovaly kazdy frame; snapshot se dela do nej,
    // aby se mutex ring bufferu nedrzel po celou dobu renderu.
    static constexpr int       kLogSnapshot = 64;
    std::vector<log::LogEntry> log_scratch = std::vector<log::LogEntry>(kLogSnapshot);
};

struct AppContext {
    ithaca::Engine                       engine;
    std::unique_ptr<ithaca::AudioDevice> audio;
    ithaca::MidiInput                    midi;
    LogRingBuffer                        log_buf;
    GuiState                             state;
    PanelState                           panels;

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
    bool bankLicenseInvalid() const { return bank_license_invalid_; }
    void clearBankLicenseInvalid() { bank_license_invalid_ = false; }

    std::thread              reload_thread_;
    std::atomic<bool>        reload_in_progress_{false};
    std::atomic<bool>        reload_done_pending_{false};
    std::atomic<bool>        reload_ok_{false};
    std::string              reload_dir_;        // psano PRED spawnem threadu
    ithaca::BankLoadProgress load_progress_;
    // Posledni load prekrocil RAM budget → banka NEUPLNA (badge v BANK panelu;
    // detail je v LOG stripu z loggeru). Cte/pise jen GUI vlakno.
    bool                     bank_truncated_ = false;
    // Posledni load licencovane banky selhal (license/MAC) → overlay ukaze
    // anglicke varovani + "Click to continue" dokud uzivatel neklikne.
    // GUI-thread only (jako bank_truncated_).
    bool                     bank_license_invalid_ = false;
};

} // namespace ithaca::gui
