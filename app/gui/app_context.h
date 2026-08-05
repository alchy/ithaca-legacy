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

    // -- Vlna v pozadi --
    // Tvar je PARAMETRICKY (soucet pomalych sinusovek), zvuk mu jen moduluje
    // amplitudu. Kreslit primo prubeh vzorku bylo pri pomalem tempu prilis
    // neklidne: pozadi ma indikovat, ze zvuk hraje, ne aby se z nej dal cist
    // tvar vlny. Proto tu nejsou stopy, ale jen dve obalky a normalizace.
    struct Wave {
        float env_l = 0.f, env_r = 0.f;   // vyhlazena hlasitost kanalu 0..1
        float env_p = 0.f;                // vyhlazena poloha pedalu 0..1
        // DVE nezavisle reference. Obalka se meri v RMS, historie ve spickach —
        // a spicka je u hudby nekolikanasobek RMS, takze delit jednu druhou
        // znamena drzet modulaci trvale na dorazu (vypadalo to jako clipping).
        float norm_rms  = 0.f;            // pro obalku
        float norm_peak = 0.f;            // pro posuvnou historii
        // Viditelnost cele vizualizace. Kdyz se nehraje, pozvolna vyhasne —
        // vcetne nosne vlny — aby pri prochazeni menu nerusila. Rizeno
        // ABSOLUTNIM prahem, ne normalizovanou urovni: auto-rozsah v tichu
        // zesili sum a vizualizace by nikdy nezhasla.
        float vis = 0.f;
        // Blizkost clippingu 0..1 (od -9 dB do 0 dB). Barvi vlnu do cervena.
        float clip = 0.f;
        // Posuvna historie hlasitosti (0..1, NE znamenkova spicka): kazdy frame
        // vstoupi zleva jedna nova hodnota a starsi se odsouvaji doprava — vlna
        // tim PLYNE, protoze se prehrava. Pri 60 fps trva pruchod sirkou ~2 s.
        //
        // Zamerne hlasitost a ne prubeh: znamenkova spicka preskakuje mezi
        // + a - kazdy frame, coz delalo zubatou caru se schody. Hlasitost
        // MODULUJE AMPLITUDU nosne vlny, takze tvar zustava hladky a zvuk se
        // projevi nabyvanim a splaskavanim podel toku.
        static constexpr int kHist = 128;
        float hist_l[kHist]{}, hist_r[kHist]{};
        // Pedal ma vlastni historii, aby jeho vlna plynula doprava stejne jako
        // zvukove. Neni to audio signal, ale pomalu se menici hodnota — vyjde
        // z nej dlouha klidna vlna, opticky odlisna od zivych L/R.
        float hist_p[kHist]{};
        int   head = 0;                   // pozice nejnovejsiho vzorku
    };
    Wave wave;
    // Svisla osa vlny. Nastavuje ji stranka PLAY na stred vybraneho patche,
    // aby vlna protekala prave jmenem nactene banky. 0 = jeste neznama,
    // pozadi pak vezme stred plochy.
    float scope_center_y = 0.f;

    // Vyhlazene rozsviceni MIDI lamp 0..1. Engine dava jen ano/ne s oknem
    // 120 ms; bez vyhlazeni by lampa cvakala. Casova konstanta ~200 ms.
    float lamp_note = 0.f, lamp_off = 0.f;

    // Prolnuti modalniho overlaye 0..1 + jak dlouho uz load bezi.
    // Kratke loady overlay VUBEC neukaze — jinak pri prepnuti banky panel
    // znatelne problikne, coz nastroj delat nema.
    float overlay_a = 0.f;
    float overlay_t = 0.f;

    // Uvodni obrazovka: cas od startu, prolnuti pri odchodu, a jestli uz dobehla.
    float splash_t    = 0.f;
    float splash_fade = 0.f;
    bool  splash_done = false;

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

    // Povedlo se otevrit audio device? Drive se vysledek jen zalogoval a zahodil;
    // uvodni obrazovka ho ukazuje jako krok initu, takze si ho musime pamatovat.
    bool audioOk() const { return audio_ok_; }
    bool audio_ok_ = false;
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
