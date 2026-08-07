// app/gui/main.cpp - vstupni bod celniho panelu Ithaca Legacy.
//
// Lifecycle: nacti stav → GLFW okno → ImGui + zabudovana pisma → AppContext
// (engine, audio, MIDI; banka se nacita asynchronne) → render loop
// (screen.cpp kresli panel, sem patri jen debounce a overlay) → uloz → shutdown.
#include "app_context.h"
#include "frame_stats.h"
#include "pages.h"
#include "splash.h"
#include "master_page.h"
#include "resonance_page.h"
#include "dsp_state.h"
#include "persistence.h"
#include "theme.h"
#include "widgets.h"
#include "layout.h"

#include "util/log.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"

// Vlastni main(), takze SDL nesmi svym makrem prepsat vstupni bod. Pri
// SDL_MAIN_HANDLED da SDL_main.h jen deklaraci SDL_SetMainReady().
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

// GL hlavicky si tahame sami: kresli se pres ImGui, ale viewport a clear
// volame primo.
#if defined(ITHACA_GLES)
    #include <GLES3/gl3.h>
#elif defined(__APPLE__)
    #include <OpenGL/gl3.h>
#else
    #if defined(_WIN32)
        // <GL/gl.h> na Windows potrebuje par maker z windows.h, ale tahat sem
        // cely windows.h NEJDE: definuje makro `small` (z RPC hlavicek) a to
        // rozbije theme::Fonts::small. Definujeme proto jen to nutne — stejne
        // to delalo i glfw3.h, ktere tu bylo predtim.
        #ifndef APIENTRY
            #define APIENTRY __stdcall
        #endif
        #ifndef WINGDIAPI
            #define WINGDIAPI __declspec(dllimport)
        #endif
        #ifndef CALLBACK
            #define CALLBACK __stdcall
        #endif
    #endif
    #include <GL/gl.h>
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <optional>

// Verze GLSL pro ImGui OpenGL3 backend. Jedina vec v celem GUI, ktera se lisi
// podle platformy.
//
// Na desktopovem Linuxu se ZAMERNE nedava 130: kontext je CORE profil 3.3 a
// GLSL 1.30 v core profilu neni platny (core vyzaduje 1.40 a vys). Ovladace to
// odmitaji ruzne ochotne, takze 150 je jedina bezpecna volba.
#if defined(ITHACA_GLES)
    inline constexpr const char* kGlslVersion = "#version 300 es";
    inline constexpr bool kPanelOnly = true;    // KMSDRM: zadny okenni manazer
#else
    inline constexpr const char* kGlslVersion = "#version 150";
    inline constexpr bool kPanelOnly = false;
#endif

static void printUsage(const char* argv0) {
    std::fprintf(stderr,
        "Pouziti: %s [--bank-dir <path>] [--log-level <lvl>] [--fullscreen] [--help]\n"
        "  --bank-dir <path>  adresar, od ktereho zacina prochazeni bank na strance\n"
        "                     BANK; persistovano v state.json, staci zadat jednou.\n"
        "  --log-level <lvl>  debug | info | warn | error | fatal (default info);\n"
        "                     persistovano v state.json, menitelne i za behu v UI.\n"
        "  --wave-glow <f>    dosah zare vlny v pozadi jako nasobitel (default 1):\n"
        "                     0 = hola cara bez zare (nejlevnejsi), 1 = vychozi,\n"
        "                     >1 = sirsi rozostreni. Zar je jedina vec na panelu,\n"
        "                     ktera roste s vyplni — na slabsi grafice ubirat tady.\n"
        "                     Persistovano v state.json.\n"
        "  --wave-glow-budget <ms>\n"
        "                     strop PERIODY snimku; nad nim regulator dosah sam\n"
        "                     snizi (0 = vypnuto). Na panelu 60 Hz je nominal\n"
        "                     16,7 ms, takze ~25 znamena zmesknuty snimek.\n"
        "  --frame-divider <n> delitel snimkove frekvence panelu (1..4, default 1):\n"
        "                     1 = kazdy vsync, 2 = kazdy druhy (30 fps na 60 Hz).\n"
        "                     Panel je pristroj, ne hra — polovicni tempo je\n"
        "                     polovicni prace. Persistovano v state.json.\n"
        "  --frame-divider-idle <n>\n"
        "                     delitel v KLIDU (0..8, 0 = nezpomalovat). Kdyz\n"
        "                     nastroj mlci a nikdo se ho nedotyka, tempo klesne;\n"
        "                     prvni dotek nebo nota ho vrati okamzite.\n"
        "  --video-driver <n> vynuti SDL video driver (kmsdrm, x11, wayland,\n"
        "                     windows, cocoa). Na Pi je vychozi kmsdrm; pri ladeni\n"
        "                     na Pi s desktopem se hodi prepnout na x11.\n"
        "  --fullscreen       rezim panelu: okno bez dekoraci pres celou obrazovku.\n"
        "                     Na displeji zabudovanem v nastroji nema byt videt\n"
        "                     titulek okna. Na KMSDRM je to vychozi stav a prepinac\n"
        "                     nema co delat.\n"
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

#ifndef ITHACA_GLES
// Posadi okno na persistovanou pozici a osetri, ze uz tam nemusi byt displej.
//
// Off-screen clamp: pokud byl pred ulozenim pripojeny extra monitor a po
// restartu uz neni, restorovana pozice muze byt mimo viditelne plochy. Okno by
// pak existovalo, ale nebylo videt a neslo chytit. Spocte se prekryv s kazdym
// pripojenym displejem; kdyz nikde nezbyde alespon 100x100 px, spadne se na
// (100, 100). Zapisuje se i do `st`, aby se ulozila uz opravena hodnota.
void placeWindow(SDL_Window* w, ithaca::gui::GuiState& st, bool fullscreen) {
    if (fullscreen) return;

    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    bool visible = false;
    for (int i = 0; i < count && !visible; ++i) {
        SDL_Rect r;
        if (!SDL_GetDisplayBounds(ids[i], &r)) continue;
        const int ox1 = std::max(st.window.x, r.x);
        const int oy1 = std::max(st.window.y, r.y);
        const int ox2 = std::min(st.window.x + st.window.w, r.x + r.w);
        const int oy2 = std::min(st.window.y + st.window.h, r.y + r.h);
        visible = (ox2 - ox1 >= 100) && (oy2 - oy1 >= 100);
    }
    SDL_free(ids);

    if (!visible) { st.window.x = 100; st.window.y = 100; }
    SDL_SetWindowPosition(w, st.window.x, st.window.y);
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    SDL_SetMainReady();   // vlastni main(), viz SDL_MAIN_HANDLED vyse
    using namespace ithaca::gui;

    // 0. CLI parse: jen --bank-dir a --help.
    std::string cli_bank_dir;
    std::string cli_log_level;
    std::optional<float> cli_glow, cli_glow_budget;
    std::optional<int>   cli_divider, cli_divider_idle;
    std::string cli_video_driver;
    bool cli_fullscreen = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { printUsage(argv[0]); return 0; }
        if (a == "--bank-dir" && i + 1 < argc) {
            cli_bank_dir = argv[++i];
        } else if (a == "--log-level" && i + 1 < argc) {
            cli_log_level = argv[++i];
        } else if (a == "--wave-glow" && i + 1 < argc) {
            cli_glow = std::strtof(argv[++i], nullptr);
        } else if (a == "--wave-glow-budget" && i + 1 < argc) {
            cli_glow_budget = std::strtof(argv[++i], nullptr);
        } else if (a == "--frame-divider" && i + 1 < argc) {
            cli_divider = std::atoi(argv[++i]);
        } else if (a == "--frame-divider-idle" && i + 1 < argc) {
            cli_divider_idle = std::atoi(argv[++i]);
        } else if (a == "--video-driver" && i + 1 < argc) {
            cli_video_driver = argv[++i];
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
    // CLI override: dosah zare + strop periody. Tataz sanitizace jako
    // v persistenci — strtof vrati NaN treba pro "--wave-glow nan".
    if (cli_glow)        st.wave_glow = sanitizeGlow(*cli_glow, kWaveGlowMax);
    if (cli_glow_budget) st.wave_glow_budget_ms = sanitizeGlow(*cli_glow_budget,
                                                               kWaveBudgetMax);
    if (cli_divider)      st.frame_divider = std::clamp(*cli_divider, 1, 4);
    if (cli_divider_idle) st.frame_divider_idle = std::clamp(*cli_divider_idle, 0, 8);
    // Kdyz nastroj nejede na vychozim vzhledu, ma to byt videt v logu — jinak
    // se "proc je vlna jina" hleda hodne blbe.
    if (st.wave_glow != 1.f || st.wave_glow_budget_ms > 0.f) {
        LOG_INFO("gui", "Wave glow: %.2f%s", (double)st.wave_glow,
                 st.wave_glow_budget_ms > 0.f ? " (auto)" : "");
    }
    if (st.frame_divider != 1 || st.frame_divider_idle > 0) {
        LOG_INFO("gui", "Frame divider: %d (v klidu %d)", st.frame_divider,
                 st.frame_divider_idle > st.frame_divider ? st.frame_divider_idle
                                                          : st.frame_divider);
    }

    // 2. SDL3 + okno.
    //
    // Na Pi se video driver VYNUCUJE na kmsdrm. Autodetekce by tam byla past:
    // kdyz je nainstalovana desktopova varianta systemu a nekdo je prihlaseny,
    // SDL by sahlo po Waylandu — a panel by se otevrel do okna na plose misto
    // na displej pristroje.
    if (!cli_video_driver.empty())
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, cli_video_driver.c_str());
#ifdef ITHACA_GLES
    else
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "kmsdrm");
#endif
    // Dotyk se ma chovat jako mys: cely panel je stavěny na jeden prst a ImGui
    // zadny vlastni dotykovy vstup nepotrebuje.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    LOG_INFO("gui", "Video driver: %s", SDL_GetCurrentVideoDriver());

#ifdef ITHACA_GLES
    // V3D na Pi poskytuje OpenGL ES, ne desktopove GL. FORWARD_COMPATIBLE se
    // v ES profilu ZAMERNE nenastavuje — nema tam smysl a implementace ho muze
    // odmitnout.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Rezim panelu: okno bez dekoraci pres celou obrazovku. Na displeji
    // zabudovanem v nastroji nema byt videt titulek okna ani ramecek okenniho
    // manazera — ramecek panelu si kreslime sami (a je to mrtva zona pro dotyk).
    //
    // Na KMSDRM zadny okenni manazer neexistuje a okno je vzdy jedno pres celou
    // obrazovku, takze se cela vetev s pozici, monitory a off-screen clampem
    // stava bezpredmetnou. `--fullscreen` se tam proto jen zaloguje.
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN;
    int win_w0 = st.window.w, win_h0 = st.window.h;
#ifdef ITHACA_GLES
    if (cli_fullscreen)
        LOG_INFO("gui", "--fullscreen: na KMSDRM je okno pres celou obrazovku vzdy");
    flags |= SDL_WINDOW_FULLSCREEN;
#else
    if (cli_fullscreen) {
        // Bez dekoraci pres celou plochu, ale NE exkluzivni rezim: ten prepina
        // mod obrazovky a na Pi komplikuje prepnuti na konzoli, kdyz se neco
        // pokazi.
        flags |= SDL_WINDOW_BORDERLESS;
        const SDL_DisplayID disp = SDL_GetPrimaryDisplay();
        if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(disp)) {
            win_w0 = m->w;
            win_h0 = m->h;
            st.window.w = m->w;
            st.window.h = m->h;
        }
    }
#endif

    SDL_Window* w = SDL_CreateWindow("Ithaca Legacy", win_w0, win_h0, flags);
    if (!w) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    // Okno vznika skryte a ukaze se az na spravnem miste — jinak by na okamzik
    // probliklo tam, kam ho posadil okenni manazer.
#ifndef ITHACA_GLES
    placeWindow(w, st, cli_fullscreen);
#endif
    SDL_ShowWindow(w);

    SDL_GLContext gl = SDL_GL_CreateContext(w);
    if (!gl) {
        std::fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(w);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(w, gl);
    // Vsync s delitelem. Delitel 2 = prekresluje se kazdy druhy snimek panelu,
    // tedy 30 fps na 60 Hz — a to je presne polovicni prace.
    //
    // Zamerne pres swap interval, ne pres vlastni casovac se sleepem: obraz
    // tak zustava synchronizovany s panelem a netrha se. Vlastni tempovani by
    // muselo cekat mimo vsync a driv nebo pozdeji by se rozeslo.
    //
    // Bezpecne je to teprve od C0: vyhlazovani vlny je vazane na cas, takze
    // polovicni tempo nezmeni rychlost animace. Pred tim by se vlna zpomalila.
    SDL_GL_SetSwapInterval(st.frame_divider);

    // 3. ImGui init.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    {
        const float xs = SDL_GetWindowDisplayScale(w);
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
    ImGui_ImplSDL3_InitForOpenGL(w, gl);
    ImGui_ImplOpenGL3_Init(kGlslVersion);

    // 4. AppContext: engine init z GuiState + audio + midi + log subscriber.
    //    Pri failu vse hezky shodime nez vratime 1.
    AppContext ctx;
    if (!ctx.initFromState(st)) {
        std::fprintf(stderr, "AppContext init failed\n");
        // Odregistruj log subscriber (lambda drzi this) — jinak by Logger
        // singleton po zaniku ctx volal use-after-free pri pozdnim logu.
        ctx.shutdown();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_GL_DestroyContext(gl);
        SDL_DestroyWindow(w);
        SDL_Quit();
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

    // Statistika snimku. Sype se do logu jednou za minutu na urovni `debug`,
    // takze se zapina prepnutim urovne na strance LOG — bez rekompilace, i na
    // hotovem pristroji. Viz frame_stats.h, proc percentily a proc geometrie.
    FrameStats stats;

    // Aktualne nastavene tempo. Swap interval se prestavuje JEN pri zmene:
    // volat ho kazdy snimek je zbytecne a na nekterych ovladacich to skube.
    int applied_divider = ctx.state.frame_divider;

    bool running = true;
    while (running) {
        const auto t_frame0 = std::chrono::steady_clock::now();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                ev.window.windowID == SDL_GetWindowID(w))
                running = false;
        }
        // Drz ctx.state.window_* aktualni kazdy frame, aby panely mohly
        // pocitat layout pri resize. Predtim se aktualizovalo jen pri shutdown.
        // V rezimu panelu geometrii NEpersistujeme: ulozilo by se rozliseni
        // panelu a pri pristim okennim spusteni by se okno otevrelo obri.
        // Rozmery pro layout ale potrebujeme tak jako tak.
        // Na KMSDRM se geometrie NEPERSISTUJE nikdy: okno je vzdy pres cely
        // panel, takze by se do state.json ulozilo rozliseni pristroje a pri
        // pristim spusteni na PC by se okno otevrelo obri.
        const bool keep_geometry = !cli_fullscreen && !kPanelOnly;
        if (keep_geometry) {
            SDL_GetWindowSize(w, &ctx.state.window.w, &ctx.state.window.h);
            SDL_GetWindowPosition(w, &ctx.state.window.x, &ctx.state.window.y);
            win_w = ctx.state.window.w;
            win_h = ctx.state.window.h;
        } else {
            SDL_GetWindowSize(w, &win_w, &win_h);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
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

        // Hranice mereni. Vsechno VYSE je nase prace: udalosti, logika panelu
        // a stavba draw listu (tam vznikaji vertexy stuh, ktere optimalizujeme).
        // Nic z toho neblokuje. Vsechno NIZE je ovladac — a vcetne cekani na
        // vsync, ktere na Windows nespadne do SwapBuffers, ale uz do drivejsiho
        // GL volani. Proto se to nedeli jemneji: bylo by to deleni sumu.
        const auto t_cpu1 = std::chrono::steady_clock::now();

        int fbw = 0, fbh = 0; SDL_GetWindowSizeInPixels(w, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.1f, 0.1f, 0.1f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        // Adaptivni tempo. Zrychleni je odpoved na dotek, takze musi platit
        // OKAMZITE — proto se dotaz dela jeste pred swapem, ne az po nem.
        {
            const int want = ctx.pace.step(ctx.busy(), ImGui::GetIO().DeltaTime,
                                           ctx.state.frame_divider,
                                           ctx.state.frame_divider_idle);
            if (want != applied_divider) {
                // Zmena tempa se loguje: na hotovem pristroji je to jediny
                // zpusob, jak poznat, ze usporny rezim vubec nabehl (a hlavne
                // ze se z nej vraci vcas).
                LOG_DEBUG("gui", "Pace: delitel %d -> %d", applied_divider, want);
                SDL_GL_SetSwapInterval(want);
                applied_divider = want;
            }
        }

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(w);
        const auto t_present1 = std::chrono::steady_clock::now();

        const auto ms = [](auto a, auto b) {
            return std::chrono::duration<float, std::milli>(b - a).count();
        };
        const ImDrawData* dd = ImGui::GetDrawData();
        stats.add(ms(t_frame0, t_cpu1), ms(t_cpu1, t_present1),
                  dd ? dd->TotalVtxCount : 0, dd ? dd->TotalIdxCount : 0,
                  dd ? dd->CmdListsCount : 0);
        char statline[192];
        if (stats.maybeReport(ImGui::GetTime(), statline, sizeof(statline)))
            LOG_DEBUG("gui", "%s", statline);
    }

    // 7. Save state pred shutdown. ctx.state.window_* uz je aktualni z render
    //    loopu (per-frame update), nemusime znovu volat glfwGetWindow*.
    //
    //    Aktualni hodnoty se pritom ulozi jako USER profil. Uzivatel tak nemusi
    //    na nic klikat: cim nastroj vypnul, s tim ho zase zapne. Tlacitko
    //    SET CURRENT AS USER PROFILE na SYS dela tedy jen to, co se stejne
    //    stane pri ukonceni — jen hned a bez cekani na vypnuti.
    snapshotPages(ctx.state.defaults, pages, kPages);
    saveState(defaultStatePath(), ctx.state);

    // 8. Shutdown — RT flush thread → AppContext → ImGui → SDL.
    log_run.store(false, std::memory_order_relaxed);
    log_thr.join();
    ctx.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
