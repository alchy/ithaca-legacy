// app/gui/main.cpp - F8 GUI entry point. AppContext + ImGui skeleton.
// Panely (top bar, keyboard viz, diag, params, log) pribydou v tascich 8-11.
// Ted: load state → GLFW okno → ImGui init → AppContext init → render loop
// s placeholder diag oknem → save state → cisty shutdown.
#include "app_context.h"
#include "panel_topbar.h"
#include "panel_keyboard.h"
#include "panel_diag.h"
#include "panel_params.h"
#include "persistence.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include <cstdio>
#include <thread>
#include <atomic>
#include <chrono>

// GLFW error callback — bez nej se chyby ztracene loguji jen ad-hoc do stderr.
static void glfwErrorCb(int err, const char* desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

int main() {
    using namespace ithaca::gui;

    // 1. Load state (nebo defaults pri prvnim spusteni / corrupt JSON).
    GuiState st;
    if (auto loaded = loadState(defaultStatePath()); loaded.has_value()) {
        st = *loaded;
    }

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
    glfwMakeContextCurrent(w);
    glfwSwapInterval(1); // vsync

    // 3. ImGui init.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
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

    // 6. Render loop. Placeholder: jedno diag okno s engine metrikami.
    //    Panely (Task 8-11) pribydou postupne.
    while (!glfwWindowShouldClose(w)) {
        glfwPollEvents();
        // Drz ctx.state.window_* aktualni kazdy frame, aby panely mohly
        // pocitat layout pri resize. Predtim se aktualizovalo jen pri shutdown.
        glfwGetWindowSize(w, &ctx.state.window_w, &ctx.state.window_h);
        glfwGetWindowPos(w, &ctx.state.window_x, &ctx.state.window_y);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Layout: top bar (36px), keyboard viz (180px), diag+params (zbytek
        // minus log strip 96px, Task 11), log strip (96px).
        const float W = (float)ctx.state.window_w;
        const float H = (float)ctx.state.window_h;
        const float topbar_h   = 36.f;
        const float keyboard_h = 180.f;
        const float log_h      = 96.f;  // zatim placeholder vysky (Task 11)
        renderTopBar       (ctx);
        renderKeyboardPanel(ctx, 0, topbar_h, W, keyboard_h);
        const float panels_y = topbar_h + keyboard_h;
        const float panels_h = H - panels_y - log_h;
        renderDiagPanel    (ctx, 0,        panels_y, W * 0.5f, panels_h);
        renderParamsPanel  (ctx, W * 0.5f, panels_y, W * 0.5f, panels_h);

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
