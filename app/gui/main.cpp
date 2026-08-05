// app/gui/main.cpp - vstupni bod celniho panelu Ithaca Legacy.
//
// Lifecycle: nacti stav → GLFW okno → ImGui + zabudovana pisma → AppContext
// (engine, audio, MIDI; banka se nacita asynchronne) → render loop
// (screen.cpp kresli panel, sem patri jen debounce a overlay) → uloz → shutdown.
#include "app_context.h"
#include "pages.h"
#include "master_page.h"
#include "resonance_page.h"
#include "dsp_state.h"
#include "persistence.h"
#include "theme.h"
#include "layout.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <optional>

// GLFW error callback — bez nej se chyby ztracene loguji jen ad-hoc do stderr.
static void glfwErrorCb(int err, const char* desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

static void printUsage(const char* argv0) {
    std::fprintf(stderr,
        "Pouziti: %s [--bank-dir <path>] [--log-level <lvl>] [--help]\n"
        "  --bank-dir <path>  adresar s bankami (dropdown bude scanovat odtud);\n"
        "                     persistovano v state.json, staci zadat jednou.\n"
        "  --log-level <lvl>  debug | info | warn | error | fatal (default info);\n"
        "                     persistovano v state.json, menitelne i za behu v UI.\n"
        "  --help, -h         tato napoveda\n", argv0);
}

namespace {

// Debounce hodin. Render loop ho pouziva 2x a KAZDY s jinou semantikou:
//  - persistence: ulozit ~1 s od PRVNI zmeny (touch) — pri tazeni slideru se
//    tedy pravidelne uklada, misto aby se cekalo na uplne dotazeni,
//  - rezonancni cache: prestavet po 400 ms od POSLEDNI zmeny (retouch) —
//    prestavba je draha, behem tazeni se spoustet nesmi.
struct Debouncer {
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> since;

    void touch()   { if (!since) since = Clock::now(); }   // od prvni zmeny
    void retouch() { since = Clock::now(); }               // od posledni zmeny
    bool expired(std::chrono::milliseconds delay) {
        if (!since || Clock::now() - *since <= delay) return false;
        since.reset();
        return true;
    }
};

// Fullscreen modalni overlay pres celou plochu: pohlti vsechen vstup (topmost
// + SetNextWindowFocus). Kresli se ve dvou rezimech — prubeh nacitani banky,
// nebo hlaseni o neplatne licenci, ktere drzi dokud uzivatel neklikne.
void drawLoadingOverlay(ithaca::gui::AppContext& ctx, float W, float H,
                        bool license_bad) {
    using namespace ithaca::gui;
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({W, H});
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0x0a, 0x23, 0x59, 235));
    ImGui::Begin("##loading", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar);

    const std::string bank_name =
        std::filesystem::path(ctx.state.bank_path).filename().string();
    const float cw = 420.f;
    ImGui::SetCursorPos({(W - cw) * 0.5f, H * 0.40f});
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors::v(theme::Colors::ink));
    ImGui::TextUnformatted(bank_name.c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 8});

    if (license_bad) {
        // Licencovana banka selhala (license/MAC) — anglicky text bez progressu
        // a RAM info; overlay drzi dokud uzivatel nepotvrdi klikem.
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors::v(theme::Colors::error));
        ImGui::TextUnformatted("Soundbank is corrupted or license file is invalid.");
        ImGui::TextUnformatted("Sampler is unable to load the bank.");
        ImGui::PopStyleColor();
        ImGui::Dummy({0, 12});
        if (ImGui::Button("Click to continue", ImVec2(cw, 0)))
            ctx.clearBankLicenseInvalid();
    } else {
        const auto& p = ctx.loadProgress();
        const int    phase  = p.phase.load(std::memory_order_relaxed);
        const int    done   = p.done.load(std::memory_order_relaxed);
        const int    total  = p.total.load(std::memory_order_relaxed);
        const size_t mb     = p.bytes_loaded.load(std::memory_order_relaxed) / (1024 * 1024);
        const size_t bud_mb = p.budget_bytes.load(std::memory_order_relaxed) / (1024 * 1024);
        const bool   trunc  = p.truncated.load(std::memory_order_relaxed);
        const float  frac   = ithaca::bankLoadFraction(phase, done, total);

        char line[96];
        if (phase == 2)
            std::snprintf(line, sizeof(line), "Building resonance cache (%d/%d)", done, total);
        else if (phase == 1)
            std::snprintf(line, sizeof(line), "Loading samples (%d/%d)", done, total);
        else
            std::snprintf(line, sizeof(line), "Scanning bank...");

        char memline[96];
        if (bud_mb > 0)
            std::snprintf(memline, sizeof(memline), "RAM: %zu / %zu MB (budget)", mb, bud_mb);
        else
            std::snprintf(memline, sizeof(memline), "RAM: %zu MB", mb);

        ImGui::ProgressBar(frac, ImVec2(cw, 14));
        ImGui::Dummy({0, 4});
        ImGui::TextUnformatted(line);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors::v(theme::Colors::dim));
        ImGui::TextUnformatted(memline);
        ImGui::PopStyleColor();
        if (trunc) {
            ImGui::Dummy({0, 6});
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors::v(theme::Colors::error));
            ImGui::TextUnformatted(
                "Bank exceeded RAM budget - loaded INCOMPLETE (see LOG)");
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndGroup();
    ImGui::End();
    ImGui::PopStyleColor();
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace ithaca::gui;

    // 0. CLI parse: jen --bank-dir a --help.
    std::string cli_bank_dir;
    std::string cli_log_level;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { printUsage(argv[0]); return 0; }
        if (a == "--bank-dir" && i + 1 < argc) {
            cli_bank_dir = argv[++i];
        } else if (a == "--log-level" && i + 1 < argc) {
            cli_log_level = argv[++i];
        } else {
            std::fprintf(stderr, "Neznama volba: %s\n", a.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }

    // 1. Load state (nebo defaults pri prvnim spusteni / corrupt JSON).
    GuiState st;
    if (auto loaded = loadState(defaultStatePath()); loaded.has_value()) {
        st = *loaded;
    }
    // CLI override: --bank-dir nahrad persistovany bank_search_dir.
    if (!cli_bank_dir.empty()) st.bank_search_dir = cli_bank_dir;
    // CLI override: --log-level nahrad persistovany log_level (aplikuje se
    // v AppContext::initFromState pres setMinSeverity).
    if (!cli_log_level.empty()) st.log_level = cli_log_level;

    // 2. GLFW window. Pozice nastavime az po vytvoreni (GLFW nema
    //    GLFW_POSITION_X hint v 3.3; v 3.4+ ano, ale my vendorujeme starsi).
    glfwSetErrorCallback(glfwErrorCb);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* w = glfwCreateWindow(st.window.w, st.window.h,
                                     "Ithaca Legacy", nullptr, nullptr);
    if (!w) { glfwTerminate(); return 1; }
    glfwSetWindowPos(w, st.window.x, st.window.y);
    // Off-screen clamp: pokud byl pred ulozenim pripojeny extra monitor a po
    // restartu uz neni, restorovana pozice muze byt mimo viditelne plochy.
    // Spocteme prekryv okna s kazdym pripojenym monitorem; pokud nikde neni
    // alespon 100×100 px viditelnych, fallback na (100, 100). Persistujeme
    // i do st aby se ulozila spravna hodnota pri pristim shutdown.
    auto isWindowOnAnyMonitor = [&]() -> bool {
        int x, y, w_size, h_size;
        glfwGetWindowPos(w, &x, &y);
        glfwGetWindowSize(w, &w_size, &h_size);
        int count = 0;
        GLFWmonitor** mons = glfwGetMonitors(&count);
        for (int i = 0; i < count; ++i) {
            int mx, my; glfwGetMonitorPos(mons[i], &mx, &my);
            const GLFWvidmode* mode = glfwGetVideoMode(mons[i]);
            if (!mode) continue;
            // Overlap test: okno musi mit alespon 100x100 px viditelnych.
            const int ox1 = (x > mx) ? x : mx;
            const int oy1 = (y > my) ? y : my;
            const int ox2 = (x + w_size < mx + mode->width)  ? x + w_size : mx + mode->width;
            const int oy2 = (y + h_size < my + mode->height) ? y + h_size : my + mode->height;
            if (ox2 - ox1 >= 100 && oy2 - oy1 >= 100) return true;
        }
        return false;
    };
    if (!isWindowOnAnyMonitor()) {
        glfwSetWindowPos(w, 100, 100);
        st.window.x = 100;
        st.window.y = 100;
    }
    glfwMakeContextCurrent(w);
    glfwSwapInterval(1); // vsync

    // 3. ImGui init.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    {
        float xs = 1.f, ys = 1.f;
        glfwGetWindowContentScale(w, &xs, &ys);
        ithaca::gui::layout::g_scale = (xs > 0.f) ? xs : 1.f;   // DPI scale (Retina ~2.0)
        ithaca::gui::theme::apply_theme();
        const float s = ithaca::gui::layout::g_scale;
        // Font je zabudovany v binarce (embedded_font.cpp) — zadny asset se
        // za behu nehleda. Raster ve fyzickem rozliseni, viz load_fonts.
        ithaca::gui::theme::load_fonts(s);
        ImGuiIO& io = ImGui::GetIO();
        // Font rasterizovan na size*scale → zobraz v logicke velikosti (1/scale)
        // = ostre, spravna velikost. Viz load_fonts.
        io.FontGlobalScale = (s > 0.f) ? 1.f / s : 1.f;
        if (ithaca::gui::theme::Fonts::ui) io.FontDefault = ithaca::gui::theme::Fonts::ui;
    }
    ImGui_ImplGlfw_InitForOpenGL(w, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    // 4. AppContext: engine init z GuiState + audio + midi + log subscriber.
    //    Pri failu vse hezky shodime nez vratime 1.
    AppContext ctx;
    if (!ctx.initFromState(st)) {
        std::fprintf(stderr, "AppContext init failed\n");
        // Odregistruj log subscriber (lambda drzi this) — jinak by Logger
        // singleton po zaniku ctx volal use-after-free pri pozdnim logu.
        ctx.shutdown();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(w);
        glfwTerminate();
        return 1;
    }

    // 5. Background thread pro pravidelny flush RT log ringu. Audio thread
    //    pouziva LOG_RT_* (lock-free SPSC), ktery vyzaduje pravidelny flush
    //    z non-RT threadu jinak ring pretece a zpravy se zahazuji. 100 Hz
    //    je rozumny kompromis. (Stejny vzor jako app/cli/main.cpp.)
    std::atomic<bool> log_run{true};
    std::thread log_thr([&log_run]() {
        using namespace std::chrono_literals;
        while (log_run.load(std::memory_order_relaxed)) {
            ithaca::log::Logger::default_().flushRTBuffer();
            std::this_thread::sleep_for(10ms);
        }
        // Posledni flush pred ukoncenim, aby se neztratily konecne zpravy.
        ithaca::log::Logger::default_().flushRTBuffer();
    });

    // 6. Render loop. Panely top bar + keyboard + diag + params + log strip.
    Debouncer save_db;                 // persistence: 1 s od prvni zmeny
    Debouncer layer_db;                // rezonancni cache: 400 ms od posledni
    float prev_layer_db = ctx.state.resonance_layer_db;
    GuiState last_saved = ctx.state;

    // CONFIG stranky (6): MASTER + RESONANCE + 4 DSP stage z chainu.
    MasterPage    master_page(ctx);
    ResonancePage resonance_page(ctx);
    ithaca::dsp::IParamPage* pages[] = {
        &master_page,
        &resonance_page,
        &ctx.engine.dspChain().stage(0),   // CONVOLVER
        &ctx.engine.dspChain().stage(1),   // AGC
        &ctx.engine.dspChain().stage(2),   // ENHANCER
        &ctx.engine.dspChain().stage(3),   // LIMITER
    };
    constexpr int kPages = (int)IM_ARRAYSIZE(pages);
    if (ctx.state.config_page < 0 || ctx.state.config_page >= kPages)
        ctx.state.config_page = 0;

    while (!glfwWindowShouldClose(w)) {
        glfwPollEvents();
        // Drz ctx.state.window_* aktualni kazdy frame, aby panely mohly
        // pocitat layout pri resize. Predtim se aktualizovalo jen pri shutdown.
        glfwGetWindowSize(w, &ctx.state.window.w, &ctx.state.window.h);
        glfwGetWindowPos(w, &ctx.state.window.x, &ctx.state.window.y);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const float W = (float)ctx.state.window.w;
        const float H = (float)ctx.state.window.h;
        renderScreen(ctx, pages, kPages);

        // Zrcadli aktualni DSP stage hodnoty do ctx.state (pro persistenci) —
        // panely meni stage primo, takze bez tohoto by je saveState nevidel.
        // Genericke: nova stage/parametr se zrcadli sam, viz dsp_state.h.
        dspStateFromChain(ctx.state, ctx.engine.dspChain());

        // Async bank reload: completion (GUI vlakno) + modalni overlay.
        ctx.pollReloadCompletion();
        const bool license_bad = ctx.bankLicenseInvalid();
        if (ctx.reloadInProgress() || license_bad)
            drawLoadingOverlay(ctx, W, H, license_bad);

        // Persistence debounce. Porovnava se CELY GuiState (operator== =
        // default) — drive tu byl rucni retezec 30 poli, ktery vynechaval
        // bank_search_dir a pri pridani pole se na nej tise zapominalo.
        // Geometrii okna z porovnani vyradime: meni se pri kazdem posunu okna
        // a spustila by ukladani porad dokola (uklada se stejne pri kazdem
        // saveState vc. finalniho pred shutdownem).
        last_saved.window = ctx.state.window;
        if (ctx.state != last_saved) save_db.touch();
        if (save_db.expired(std::chrono::seconds(1))) {
            saveState(defaultStatePath(), ctx.state);
            last_saved = ctx.state;
        }

        // Resonance Layer: po 400 ms ticha prestavet RAM cache na pozadi.
        if (ctx.state.resonance_layer_db != prev_layer_db) {
            prev_layer_db = ctx.state.resonance_layer_db;
            layer_db.retouch();
        }
        if (layer_db.expired(std::chrono::milliseconds(400)))
            ctx.engine.rebuildResonanceCache(ctx.state.resonance_layer_db);

        ImGui::Render();
        int fbw, fbh; glfwGetFramebufferSize(w, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.1f, 0.1f, 0.1f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(w);
    }

    // 7. Save state pred shutdown. ctx.state.window_* uz je aktualni z render
    //    loopu (per-frame update), nemusime znovu volat glfwGetWindow*.
    saveState(defaultStatePath(), ctx.state);

    // 8. Shutdown — RT flush thread → AppContext → ImGui → GLFW.
    log_run.store(false, std::memory_order_relaxed);
    log_thr.join();
    ctx.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}
