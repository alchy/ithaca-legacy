# third-party/CMakeLists.txt
# --------------------------
# Vendored deps jako CMake targety. Zadny FetchContent — vse source-vendored
# pres tools/fetch-third-party.sh. Targety:
#   doctest         INTERFACE  (header-only, jen pro testy)
#   nlohmann_json   INTERFACE  (header-only, config + banky v dalsich fazich)
#   miniaudio_inc   INTERFACE  (header-only, audio out v dalsich fazich)
#   rtmidi          STATIC     (MIDI in v dalsich fazich)
#   glfw            STATIC     (okno + GL context pro F8 GUI; vlastni CMake)
#   imgui           STATIC     (Dear ImGui core + GLFW/OpenGL3 backendy)

# -- doctest (header-only) --
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/doctest/doctest.h")
    add_library(doctest INTERFACE)
    target_include_directories(doctest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/doctest)
else()
    message(FATAL_ERROR
        "doctest/doctest.h chybi. Spust: ./tools/fetch-third-party.sh")
endif()

# -- nlohmann/json (header-only) --
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json/nlohmann/json.hpp")
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json)
else()
    message(FATAL_ERROR
        "nlohmann_json/nlohmann/json.hpp chybi. Spust: ./tools/fetch-third-party.sh")
endif()

# -- miniaudio (header-only) --
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/miniaudio/miniaudio.h")
    add_library(miniaudio_inc INTERFACE)
    target_include_directories(miniaudio_inc INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio)
endif()

# -- RtMidi (static) — platform backend defs --
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rtmidi/RtMidi.cpp")
    add_library(rtmidi STATIC rtmidi/RtMidi.cpp)
    target_include_directories(rtmidi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/rtmidi)
    if(APPLE)
        target_compile_definitions(rtmidi PRIVATE __MACOSX_CORE__)
        target_link_libraries(rtmidi PRIVATE
            "-framework CoreMIDI" "-framework CoreFoundation" "-framework CoreAudio")
    elseif(UNIX)
        target_compile_definitions(rtmidi PRIVATE __LINUX_ALSA__)
        target_link_libraries(rtmidi PRIVATE asound pthread)
    elseif(WIN32)
        target_compile_definitions(rtmidi PRIVATE __WINDOWS_MM__)
        target_link_libraries(rtmidi PRIVATE winmm)
    endif()
endif()

# -- GLFW (vendored source, vlastni CMakeLists) --
# Vypneme docs/tests/examples PRED add_subdirectory aby se nestavely.
# GLFW je optional — staveni jen kdyz je adresar dostupny (pro fazi 8 GUI).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/glfw/CMakeLists.txt")
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(glfw)
endif()

# -- Dear ImGui (static lib: core + GLFW/OpenGL3 backendy) --
# Definujeme target tady (ne v gitignored adresari) aby CMakeLists nebyl
# soucasti vendored stromu a neztratil se pri re-fetchi.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui.cpp"
   AND TARGET glfw)
    add_library(imgui STATIC
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_demo.cpp
        imgui/backends/imgui_impl_glfw.cpp
        imgui/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
    )
    target_link_libraries(imgui PUBLIC glfw)
    target_compile_features(imgui PUBLIC cxx_std_17)
    # Vendored kod — potlacit nase -Wall/-Wextra/-Wpedantic, jinak hluk.
    if(NOT MSVC)
        target_compile_options(imgui PRIVATE -w)
    endif()
    # macOS: legacy OpenGL je deprecated v 10.14+. ImGui opengl3 backend
    # ta volani pouziva — silence pres define.
    if(APPLE)
        target_compile_definitions(imgui PRIVATE GL_SILENCE_DEPRECATION)
    endif()
endif()
