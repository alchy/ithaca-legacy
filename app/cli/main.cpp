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

#include <cstdio>
#include <cstring>
#include <string>

using namespace ithaca;

static void printUsage(const char* argv0) {
    std::printf(
        "ithaca-cli %s\n"
        "\n"
        "Pouziti:\n"
        "  %s --version\n"
        "  %s --selftest [--log-level <lvl>]\n"
        "  %s --inspect <dir> [--log-level <lvl>]\n"
        "\n"
        "Volby:\n"
        "  --version            vypise verzi a skonci\n"
        "  --log-level <lvl>    debug | info | warn | error | fatal (default info)\n"
        "  --selftest           self-test loggeru (exit 0 = OK)\n"
        "  --inspect <dir>      nacti banku a vypis prehled\n"
        "  --help, -h           tato napoveda\n",
        ITHACA_VERSION, argv0, argv0, argv0);
}

int main(int argc, char* argv[]) {
    bool do_selftest = false;
    std::string inspect_dir;
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
        } else {
            std::fprintf(stderr, "Neznama volba: %s\n\n", a.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }

    auto& L = log::Logger::default_();
    L.setMinSeverity(level);
    L.setOutputMode(/*console=*/true, /*file=*/false);

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
                 bank.loaded_samples, bank.total_frames,
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
