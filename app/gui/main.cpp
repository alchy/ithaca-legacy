// app/gui/main.cpp - F8 GUI entry point. AppContext + ImGui skeleton.
// Panely: top bar, keyboard viz, diag, params, log strip.
// Lifecycle: load state → GLFW okno → ImGui init → AppContext init → render
// loop (panely + debounced state save) → save state → cisty shutdown.
#include "app_context.h"
#include "panel_topbar.h"
#include "panel_keyboard.h"
#include "panel_bank.h"
#include "panel_indicators.h"
#include "panel_dsp.h"
#include "panel_params.h"
#include "panel_log.h"
#include "persistence.h"
#include "theme.h"
#include "layout.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

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
    GLFWwindow* w = glfwCreateWindow(st.window_w, st.window_h,
                                     "ithaca-gui", nullptr, nullptr);
    if (!w) { glfwTerminate(); return 1; }
    glfwSetWindowPos(w, st.window_x, st.window_y);
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
        st.window_x = 100;
        st.window_y = 100;
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
        std::string ttf = ithaca::gui::theme::find_asset_path("cormorant/Cormorant-Medium.ttf");
        if (ttf.empty())
            std::fprintf(stderr, "WARN: Cormorant TTF nenalezen — default font.\n");
        const float s = ithaca::gui::layout::g_scale;
        ithaca::gui::theme::load_fonts(ttf, s);   // raster ve fyzickem rozliseni
        ImGuiIO& io = ImGui::GetIO();
        // Font rasterizovan na size*scale → zobraz v logicke velikosti (1/scale)
        // = ostre, spravna velikost. Viz load_fonts.
        io.FontGlobalScale = (s > 0.f) ? 1.f / s : 1.f;
        if (ithaca::gui::theme::Fonts::body) io.FontDefault = ithaca::gui::theme::Fonts::body;
    }
    ImGui_ImplGlfw_InitForOpenGL(w, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    // 4. AppContext: engine init z GuiState + audio + midi + log subscriber.
    //    Pri failu vse hezky shodime nez vratime 1.
    AppContext ctx;
    if (!ctx.initFromState(st)) {
        std::fprintf(stderr, "AppContext init failed\n");
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
    //    Persistence debounce: sleduj zmeny GuiState, ulozi po 1s idle (Task 12).
    std::optional<std::chrono::steady_clock::time_point> dirty_since;
    GuiState last_saved = ctx.state;
    while (!glfwWindowShouldClose(w)) {
        glfwPollEvents();
        // Drz ctx.state.window_* aktualni kazdy frame, aby panely mohly
        // pocitat layout pri resize. Predtim se aktualizovalo jen pri shutdown.
        glfwGetWindowSize(w, &ctx.state.window_w, &ctx.state.window_h);
        glfwGetWindowPos(w, &ctx.state.window_x, &ctx.state.window_y);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Layout: TOP BAR (full) → INDICATOR STRIP (full) → MAIN ROW
        // (BANK 230 | VOICE flex | DSP 280) → KEYBOARD (full) → LOG (full).
        namespace L = ithaca::gui::layout;
        const float W = (float)ctx.state.window_w;
        const float H = (float)ctx.state.window_h;
        const float COL1 = L::Dims::col_bank, COL3 = L::Dims::col_dsp;
        const float PAD  = L::Dims::pad_outer;
        const float topbar_h = L::Dims::topbar_h, strip_h = L::Dims::strip_h;
        const float kbd_h = L::Dims::kbd_h, log_h = L::Dims::log_h;

        ImGui::SetNextWindowPos({0,0});
        ImGui::SetNextWindowSize({W,H});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PAD, PAD));
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|
            ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoScrollbar);

        const float content_w = ImGui::GetContentRegionAvail().x;  // = W - 2*PAD

        ImGui::BeginChild("##topbar", {content_w, topbar_h}, false); renderTopBar(ctx); ImGui::EndChild();
        ImGui::Dummy({0, 2.f});   // topbar↔strip tesne (zbytek mezery = item spacing)
        ImGui::BeginChild("##strip", {content_w, strip_h}, false); renderIndicatorStrip(ctx, COL1, COL3); ImGui::EndChild();
        ImGui::Dummy({0, L::Dims::row_gap});

        const float main_h = H - 2.f*PAD - topbar_h - strip_h - kbd_h - log_h - 4.f*L::Dims::row_gap;
        ImGui::BeginChild("##bank",  {COL1, main_h}, false); renderBankPanel(ctx);   ImGui::EndChild();
        ImGui::SameLine(0,0);
        ImGui::BeginChild("##voice", {content_w-COL1-COL3, main_h}, false); renderParamsPanel(ctx); ImGui::EndChild();
        ImGui::SameLine(0,0);
        ImGui::BeginChild("##dsp",   {COL3, main_h}, false); renderDspRack(ctx);     ImGui::EndChild();

        ImGui::Dummy({0, L::Dims::row_gap});
        ImGui::BeginChild("##kbd", {content_w, kbd_h}, false); renderKeyboardPanel(ctx); ImGui::EndChild();
        ImGui::Dummy({0, L::Dims::row_gap});
        ImGui::BeginChild("##log", {content_w, log_h}, false); renderLogPanel(ctx);      ImGui::EndChild();

        ImGui::End();
        ImGui::PopStyleVar();

        // Persistence debounce: zaznamenat zmenu, ulozi az po 1s ticha. Pri
        // tahani slideru se nezbytecne neulozi kazdy frame; jen 1s po dokonceni.
        bool changed =
            last_saved.bank_path           != ctx.state.bank_path ||
            last_saved.midi_port_name      != ctx.state.midi_port_name ||
            last_saved.master_gain_db      != ctx.state.master_gain_db ||
            last_saved.resonance_strength  != ctx.state.resonance_strength ||
            last_saved.release_ms          != ctx.state.release_ms ||
            last_saved.excite_decay_ms     != ctx.state.excite_decay_ms ||
            last_saved.log_level           != ctx.state.log_level ||
            last_saved.midi_channel        != ctx.state.midi_channel;
        if (changed && !dirty_since) {
            dirty_since = std::chrono::steady_clock::now();
        }
        if (dirty_since) {
            auto now = std::chrono::steady_clock::now();
            if (now - *dirty_since > std::chrono::seconds(1)) {
                saveState(defaultStatePath(), ctx.state);
                last_saved = ctx.state;
                dirty_since.reset();
            }
        }

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
