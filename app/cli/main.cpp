// app/cli/main.cpp — ithaca-cli
// ------------------------------
// Tenky headless konzument libithaca_core. Ve fazi 1 jen overuje, ze se
// knihovna linkuje a logger funguje:
//   --version           vypise verzi a skonci
//   --log-level <lvl>    nastavi min severity (debug/info/warn/error/fatal)
//   --selftest           provede self-test loggeru, vrati 0 pri uspechu
//
// V dalsich fazich sem pribyde nacteni banky, prehravani not, atd.
#include "sample/sample_store.h"
#include "util/log.h"
#include "util/version.h"
#include "engine.h"
#include "render/batch_renderer.h"
#include "io/audio_device.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ithaca;

namespace {
struct PlayCtx { ithaca::Engine* eng; };
void playAudioCb(void* userdata, float* output, uint32_t frames) {
    auto* ctx = static_cast<PlayCtx*>(userdata);
    // Rozdel interleaved výstup na docasne L/R? Jednodussi: engine renderuje
    // do docasnych L/R bufferu a pak interleave. Pro jednoduchost pouzij
    // staticky scratch (audio thread, jeden konzument).
    static std::vector<float> L, R;
    if ((int)L.size() < (int)frames) { L.resize(frames); R.resize(frames); }
    std::fill(L.begin(), L.begin() + frames, 0.f);
    std::fill(R.begin(), R.begin() + frames, 0.f);
    ctx->eng->processBlock(L.data(), R.data(), (int)frames);
    for (uint32_t i = 0; i < frames; ++i) { output[i*2] = L[i]; output[i*2+1] = R[i]; }
}
} // namespace

static void printUsage(const char* argv0) {
    std::printf(
        "ithaca-cli %s\n"
        "\n"
        "Pouziti:\n"
        "  %s --version\n"
        "  %s --selftest [--log-level <lvl>]\n"
        "  %s --inspect <dir> [--log-level <lvl>]\n"
        "  %s --render <dir> --out <wav> [--log-level <lvl>]\n"
        "  %s --play <dir> [--log-level <lvl>]\n"
        "\n"
        "Volby:\n"
        "  --version            vypise verzi a skonci\n"
        "  --log-level <lvl>    debug | info | warn | error | fatal (default info)\n"
        "  --selftest           self-test loggeru (exit 0 = OK)\n"
        "  --inspect <dir>      nacti banku a vypis prehled\n"
        "  --render <dir>       nacti banku a renderuj test noty do --out WAV\n"
        "  --out <wav>          vystupni WAV pro --render\n"
        "  --play <dir>         nacti banku, otevri audio device a zahraj akord\n"
        "  --help, -h           tato napoveda\n",
        ITHACA_VERSION, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char* argv[]) {
    bool do_selftest = false;
    std::string inspect_dir;
    std::string render_dir, render_out;
    std::string play_dir;
    log::Severity level = log::Severity::Info;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version") {
            std::printf("ithaca-cli %s\n", ITHACA_VERSION);
            return 0;
        } else if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (a == "--log-level" && i + 1 < argc) {
            level = log::severity_from_string(argv[++i], log::Severity::Info);
        } else if (a == "--selftest") {
            do_selftest = true;
        } else if (a == "--inspect" && i + 1 < argc) {
            inspect_dir = argv[++i];
        } else if (a == "--render" && i + 1 < argc) {
            render_dir = argv[++i];
        } else if (a == "--out" && i + 1 < argc) {
            render_out = argv[++i];
        } else if (a == "--play" && i + 1 < argc) {
            play_dir = argv[++i];
        } else {
            std::fprintf(stderr, "Neznama volba: %s\n\n", a.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }

    auto& L = log::Logger::default_();
    L.setMinSeverity(level);
    L.setOutputMode(/*console=*/true, /*file=*/false);

    if (!play_dir.empty()) {
        Engine eng;
        EngineConfig cfg;
        cfg.midi_from = 21; cfg.midi_to = 108;   // cela klaviatura (POZOR: velka RAM!)
        if (!eng.init(cfg) || !eng.loadBank(play_dir)) {
            LOG_ERROR("play", "Nelze nacist banku: %s", play_dir.c_str());
            return 1;
        }
        PlayCtx ctx{&eng};
        AudioDevice dev;
        if (!dev.start(playAudioCb, &ctx, cfg.sample_rate, cfg.block_size)) {
            LOG_ERROR("play", "Nelze otevrit audio device");
            return 1;
        }
        LOG_INFO("play", "Hraje. Spoustim testovaci akord C-E-G, pak Enter pro konec.");
        eng.noteOn(60, 100); eng.noteOn(64, 100); eng.noteOn(67, 100);
        std::getchar();                          // ceka na Enter
        dev.stop();
        return 0;
    }

    if (!render_dir.empty()) {
        if (render_out.empty()) {
            LOG_ERROR("render", "--render vyzaduje --out <wav>");
            return 1;
        }
        Engine eng;
        EngineConfig cfg;
        // Pro rychly render nacti jen par not kolem stredu klaviatury.
        cfg.midi_from = 57; cfg.midi_to = 72;
        if (!eng.init(cfg) || !eng.loadBank(render_dir)) {
            LOG_ERROR("render", "Nelze nacist banku: %s", render_dir.c_str());
            return 1;
        }
        std::vector<BatchNote> notes = {
            {60, 100, 1.0f}, {64, 100, 1.0f}, {67, 100, 1.0f},   // C-E-G akord po sobe
        };
        int n = renderNotes(eng, notes, render_out, /*tail_s=*/0.5f);
        LOG_INFO("render", "Hotovo: %d not → %s", n, render_out.c_str());
        return n > 0 ? 0 : 1;
    }

    if (!inspect_dir.empty()) {
        LOG_INFO("inspect", "Nacitam banku: %s", inspect_dir.c_str());

        Bank bank = loadLegacyBank(inspect_dir, L);
        if (bank.loaded_samples == 0) {
            LOG_ERROR("inspect", "Zadne samply nenacteny z %s", inspect_dir.c_str());
            return 1;
        }

        int notes_with_samples = 0;
        for (int n = 0; n < 128; ++n)
            if (bank.notes[n].recorded) notes_with_samples++;

        LOG_INFO("inspect", "Format: %s", bankFormatName(bank.format));
        LOG_INFO("inspect", "Not se samply: %d", notes_with_samples);
        LOG_INFO("inspect", "Celkem samplu: %d, frames: %zu, RAM ~%zu MB",
                 bank.loaded_samples, bank.resident_frames,
                 bank.total_bytes / (1024 * 1024));

        // Vypis detail pro prvni nalezenou notu se samply (vzorek).
        for (int n = 0; n < 128; ++n) {
            if (!bank.notes[n].recorded) continue;
            LOG_INFO("inspect", "Nota %d: %zu slotu", n, bank.notes[n].slots.size());
            for (size_t s = 0; s < bank.notes[n].slots.size(); ++s)
                LOG_INFO("inspect", "  slot %zu: RMS %.1f dBFS, attack_end %d",
                         s, bank.notes[n].slots[s].rms_db,
                         bank.notes[n].slots[s].variants[0].attack_end_frame);
            break;   // jen prvni nota jako vzorek
        }
        return 0;
    }

    if (!do_selftest) {
        printUsage(argv[0]);
        return 0;
    }

    // -- Self-test: par log volani na obou kanalech + flush ring bufferu --
    LOG_INFO("selftest", "ithaca-cli %s startuje self-test", ITHACA_VERSION);
    LOG_DEBUG("selftest", "debug zprava (viditelna jen pri --log-level debug)");
    LOG_WARN("selftest", "warning zprava jde na stderr");
    LOG_RT_INFO("selftest", "RT zprava cislo %d z audio-like kontextu", 1);
    int flushed = L.flushRTBuffer();
    LOG_INFO("selftest", "flushnuto %d RT zprav", flushed);

    if (flushed < 1) {
        LOG_ERROR("selftest", "ocekaval jsem aspon 1 RT zpravu, dostal %d", flushed);
        return 1;
    }
    LOG_INFO("selftest", "self-test OK");
    return 0;
}
