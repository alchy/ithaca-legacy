// app/gui/main.cpp — F8 GUI entry point. Skeleton: GLFW okno + ImGui frame.
// V dalsich tasich pribydou panely (top bar, keyboard viz, diag, params, log)
// a wiring na ithaca_core Engine. Ted jen verifikace ze ImGui+GLFW build a
// vykresluji.
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <cstdio>

// GLFW error callback — bez nej se chyby ztracene loguji jen do stderr ad-hoc.
// Predani pres glfwSetErrorCallback hned na zacatku (jeste pred glfwInit).
static void glfwErrorCb(int err, const char* desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

int main() {
    glfwSetErrorCallback(glfwErrorCb);
    if (!glfwInit()) return 1;

    // OpenGL 3.3 core profile — minimalka pro ImGui's opengl3 backend.
    // Forward compat na macOS je nutny, jinak GLFW vytvori legacy kontext.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* w = glfwCreateWindow(1024, 768, "ithaca-gui", nullptr, nullptr);
    if (!w) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(w);
    glfwSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(w, true);
    // GLSL 150 = OpenGL 3.2+ core; konzistentni s 3.3 contextem vyse.
    ImGui_ImplOpenGL3_Init("#version 150");

    while (!glfwWindowShouldClose(w)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Placeholder: ImGui demo okno aby bylo videt ze backendy fungujou.
        // V dalsich tasich nahradi nase panely.
        ImGui::ShowDemoWindow();

        ImGui::Render();
        int fbw, fbh;
        glfwGetFramebufferSize(w, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.1f, 0.1f, 0.1f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(w);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}
