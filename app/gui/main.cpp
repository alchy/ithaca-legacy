// app/gui/main.cpp - vstupni bod celniho panelu Ithaca Legacy.
//
// Lifecycle: nacti stav → GLFW okno → ImGui + zabudovana pisma → AppContext
// (engine, audio, MIDI; banka se nacita asynchronne) → render loop
// (screen.cpp kresli panel, sem patri jen debounce a overlay) → uloz → shutdown.
#include "app_context.h"
#include "pages.h"
#include "splash.h"
#include "master_page.h"
#include "resonance_page.h"
#include "dsp_state.h"
#include "persistence.h"
#include "theme.h"
#include "widgets.h"
#include "layout.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
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
        "  --fullscreen       bez dekoraci pres celou obrazovku (rezim panelu);\n"
        "                     na zabudovanem displeji nema byt videt titulek okna.\n"
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

// Modalni overlay pres celou plochu. Kresli se ve dvou rezimech: prubeh
// nacitani banky, nebo hlaseni o neplatne licenci (to drzi dokud uzivatel
// neklikne).
//
// DULEZITE — proc se prolina a proc ma zpozdeni: pakovana banka se nacte
// za necelou vterinu a overlay pak jen problikl pres cely panel. Nastroj
// takhle blikat nema. Proto se overlay vubec neobjevi u loadu kratsich nez
// ~350 ms a nabiha i mizi prolnutim.
void drawLoadingOverlay(ithaca::gui::AppContext& ctx, float W, float H,
                        bool license_bad, float alpha) {
    using namespace ithaca::gui;
    namespace L = ithaca::gui::layout;
    using theme::Colors;
    using theme::Fonts;
    if (alpha <= 0.004f) return;

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({W, H});
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##loading", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto A = [&](ImU32 c, float m = 1.f) {
        return (c & 0x00FFFFFF)
             | ((ImU32)(((c >> 24) & 0xFF) * alpha * m) << 24);
    };

    // Zatemneni panelu, ne prekryti — pod nim je porad videt, co se deje.
    dl->AddRectFilled({0, 0}, {W, H}, A(IM_COL32(0x06, 0x16, 0x3c, 225)));

    const float bw = 560.f;
    const float bx = (W - bw) * 0.5f;
    float y = H * 0.38f;
    const float px_u = wdg::fontPx(Fonts::ui);
    const float px_s = wdg::fontPx(Fonts::small);

    const std::string bank =
        std::filesystem::path(ctx.state.bank_path).filename().string();

    if (license_bad) {
        wdg::invField(dl, ImVec2(bx, y), Fonts::small, "LICENSE", 10.f, 0.f,
                      A(Colors::inv_bg), A(Colors::inv_fg));
        y += px_s * 1.45f + 16.f;
        dl->AddText(Fonts::ui, px_u, ImVec2(bx, y), A(Colors::ink), bank.c_str());
        y += px_u + 14.f;
        dl->AddText(Fonts::small, px_s, ImVec2(bx, y), A(Colors::dim),
                    "Soundbank is corrupted or license file is invalid.");
        y += px_s + 6.f;
        dl->AddText(Fonts::small, px_s, ImVec2(bx, y), A(Colors::dim),
                    "Sampler is unable to load the bank.");
        y += px_s + 22.f;
        ImGui::SetCursorScreenPos(ImVec2(bx, y));
        if (wdg::button("##lic_ok", "CONTINUE")) ctx.clearBankLicenseInvalid();
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }

    const auto& p = ctx.loadProgress();
    const int    phase  = p.phase.load(std::memory_order_relaxed);
    const int    done   = p.done.load(std::memory_order_relaxed);
    const int    total  = p.total.load(std::memory_order_relaxed);
    const size_t mb     = p.bytes_loaded.load(std::memory_order_relaxed) / (1024 * 1024);
    const size_t bud_mb = p.budget_bytes.load(std::memory_order_relaxed) / (1024 * 1024);
    const bool   trunc  = p.truncated.load(std::memory_order_relaxed);
    const float  frac   = ithaca::bankLoadFraction(phase, done, total);

    wdg::invField(dl, ImVec2(bx, y), Fonts::small, "LOADING", 10.f, 0.f,
                  A(Colors::inv_bg), A(Colors::inv_fg));
    y += px_s * 1.45f + 16.f;
    dl->AddText(Fonts::ui, px_u, ImVec2(bx, y), A(Colors::ink), bank.c_str());
    y += px_u + 16.f;

    // Progres jako retez bloku — znakovy displej nema plynuly pruh.
    constexpr int kSeg = 40;
    const float seg_w = bw / (float)kSeg;
    const int lit = (int)(frac * kSeg + 0.5f);
    for (int i = 0; i < kSeg; ++i) {
        const ImVec2 a(bx + i * seg_w + 1.f, y);
        const ImVec2 b(bx + (i + 1) * seg_w - 1.f, y + 16.f);
        if (i < lit) dl->AddRectFilled(a, b, A(Colors::inv_bg));
        else         dl->AddRect(a, b, A(Colors::line));
    }
    y += 16.f + 12.f;

    char line[96];
    if (phase == 2)      std::snprintf(line, sizeof(line), "BUILDING RESONANCE CACHE  %d/%d", done, total);
    else if (phase == 1) std::snprintf(line, sizeof(line), "LOADING SAMPLES  %d/%d", done, total);
    else                 std::snprintf(line, sizeof(line), "SCANNING BANK");
    dl->AddText(Fonts::small, px_s, ImVec2(bx, y), A(Colors::dim), line);
    y += px_s + 6.f;

    char memline[96];
    if (bud_mb > 0) std::snprintf(memline, sizeof(memline), "RAM  %zu / %zu MB", mb, bud_mb);
    else            std::snprintf(memline, sizeof(memline), "RAM  %zu MB", mb);
    dl->AddText(Fonts::small, px_s, ImVec2(bx, y), A(Colors::dimmer), memline);

    if (trunc) {
        y += px_s + 12.f;
        wdg::invField(dl, ImVec2(bx, y), Fonts::small,
                      "INCOMPLETE - EXCEEDED RAM BUDGET", 10.f, 0.f,
                      A(Colors::inv_bg), A(Colors::inv_fg));
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace ithaca::gui;

    // 0. CLI parse: jen --bank-dir a --help.
    std::string cli_bank_dir;
    std::string cli_log_level;
    bool cli_fullscreen = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { printUsage(argv[0]); return 0; }
        if (a == "--bank-dir" && i + 1 < argc) {
            cli_bank_dir = argv[++i];
        } else if (a == "--log-level" && i + 1 < argc) {
            cli_log_level = argv[++i];
        } else if (a == "--fullscreen") {
            cli_fullscreen = true;
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
    // Rezim panelu: bez dekoraci pres celou obrazovku. Na displeji zabudovanem
    // v nastroji nema byt videt titulek okna ani ramecek okenniho manazera —
    // ramecek panelu si kreslime sami (a je to mrtva zona pro dotyk).
    GLFWwindow* w = nullptr;
    if (cli_fullscreen) {
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = mon ? glfwGetVideoMode(mon) : nullptr;
        if (mode) {
            // Bez dekoraci a pres celou plochu, ale NE exkluzivni fullscreen:
            // ten prepina rezim obrazovky a na Pi zbytecne komplikuje prepnuti
            // na konzoli, kdyz se neco pokazi.
            glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            glfwWindowHint(GLFW_RED_BITS, mode->redBits);
            glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
            glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
            glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
            w = glfwCreateWindow(mode->width, mode->height,
                                 "Ithaca Legacy", nullptr, nullptr);
            if (w) {
                int mx = 0, my = 0;
                glfwGetMonitorPos(mon, &mx, &my);
                glfwSetWindowPos(w, mx, my);
                st.window.w = mode->width;
                st.window.h = mode->height;
            }
        }
    }
    if (!w) {
        w = glfwCreateWindow(st.window.w, st.window.h,
                             "Ithaca Legacy", nullptr, nullptr);
        if (!w) { glfwTerminate(); return 1; }
        glfwSetWindowPos(w, st.window.x, st.window.y);
    }
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
    if (!cli_fullscreen && !isWindowOnAnyMonitor()) {
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
    // Rozmery pro layout. V rezimu panelu se lisi od persistovanych.
    int win_w = ctx.state.window.w, win_h = ctx.state.window.h;

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
        // V rezimu panelu geometrii NEpersistujeme: ulozilo by se rozliseni
        // panelu a pri pristim okennim spusteni by se okno otevrelo obri.
        // Rozmery pro layout ale potrebujeme tak jako tak.
        if (cli_fullscreen) {
            glfwGetWindowSize(w, &win_w, &win_h);
        } else {
            glfwGetWindowSize(w, &ctx.state.window.w, &ctx.state.window.h);
            glfwGetWindowPos(w, &ctx.state.window.x, &ctx.state.window.y);
            win_w = ctx.state.window.w;
            win_h = ctx.state.window.h;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const float W = (float)win_w;
        const float H = (float)win_h;

        // Panel se kresli vzdy — splash se pres nej jen prolne, takze na konci
        // animace uz je pod nim hotovy panel a neni videt zadny skok.
        renderScreen(ctx, pages, kPages, W, H);
        const bool splash = renderSplash(ctx, W, H);

        // Zrcadli aktualni DSP stage hodnoty do ctx.state (pro persistenci) —
        // panely meni stage primo, takze bez tohoto by je saveState nevidel.
        // Genericke: nova stage/parametr se zrcadli sam, viz dsp_state.h.
        dspStateFromChain(ctx.state, ctx.engine.dspChain());

        // Async bank reload: completion (GUI vlakno) + modalni overlay.
        ctx.pollReloadCompletion();
        const bool license_bad = ctx.bankLicenseInvalid();
        if (!splash) {
            // Behem uvodni obrazovky se modal nekresli — prvni load uz je
            // videt na ni samotne a dva prekryvajici se progresy by byly zmatek.
            //
            // Prodleva + prolnuti. Licence se ukaze hned (je to chyba, ktera
            // ceka na potvrzeni), prubeh nacitani az kdyz opravdu trva —
            // jinak by kratky load jen problikl pres cely panel.
            auto& ps = ctx.panels;
            const float dt = ImGui::GetIO().DeltaTime;
            const bool busy = ctx.reloadInProgress();
            ps.overlay_t = busy ? (ps.overlay_t + dt) : 0.f;
            const bool show = license_bad || (busy && ps.overlay_t > 0.35f);
            const float k = 1.f - std::exp(-dt / 0.12f);
            ps.overlay_a += ((show ? 1.f : 0.f) - ps.overlay_a) * k;
            drawLoadingOverlay(ctx, W, H, license_bad, ps.overlay_a);
        }

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
