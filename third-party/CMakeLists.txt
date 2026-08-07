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

# -- SDL3 (vendored source, vlastni CMakeLists) --
# Nahradilo GLFW. Duvod je jediny, ale zasadni: SDL umi backend KMSDRM, tedy
# kreslit primo na framebuffer BEZ X11 a Waylandu. GLFW okno z hole konzole
# nevytvori, a prave takhle ma zabudovany panel bezet.
#
# Podsystemy, ktere nepouzivame, se vypinaji PRED add_subdirectory — zkrati to
# build (na Pi znatelne) a zmensi binarku. Joystick/gamepad se NEVYPINA:
# imgui_impl_sdl3 sahá na SDL_GetGamepads() a bez toho by se nesestavil.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdl3/CMakeLists.txt")
    set(SDL_SHARED        OFF CACHE BOOL "" FORCE)   # linkujeme staticky
    set(SDL_STATIC        ON  CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS         OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES      OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL       OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA        OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR        OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC        OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG        OFF CACHE BOOL "" FORCE)
    # Audio i MIDI si nastroj resi sam (miniaudio + RtMidi); SDL audio by jen
    # otevrelo druhou cestu k temuz zarizeni.
    set(SDL_AUDIO         OFF CACHE BOOL "" FORCE)
    add_subdirectory(sdl3 EXCLUDE_FROM_ALL)
endif()

# -- Dear ImGui (static lib: core + GLFW/OpenGL3 backendy) --
# Definujeme target tady (ne v gitignored adresari) aby CMakeLists nebyl
# soucasti vendored stromu a neztratil se pri re-fetchi.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui.cpp"
   AND TARGET SDL3::SDL3-static)
    add_library(imgui STATIC
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_demo.cpp
        imgui/backends/imgui_impl_sdl3.cpp
        imgui/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
    )
    target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
    # OpenGL ES 3 na Pi: musi byt definovano i pro imgui, protoze prave tady
    # se prekladá imgui_impl_opengl3.cpp. Definovat to jen na ithaca-gui by
    # znamenalo, ze se backend prelozi pro desktopove GL a hlavicky by se
    # rozesly s tim, co ocekava aplikace.
    if(ITHACA_GUI_GLES)
        target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
    endif()
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
