cmake_minimum_required(VERSION 3.20)
project(ithaca_legacy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json pro clangd (LSP) — editor pak vidi stejne
# include paths jako build a nehlasi spurious "file not found".
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX _USE_MATH_DEFINES)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Sanitizery (opt-in, jednorazovy audit): -DITHACA_SANITIZE=address,undefined
# nebo =thread. Prazdne = vypnuto. Pridava -g + frame pointery pro citelne stacky.
set(ITHACA_SANITIZE "" CACHE STRING "Sanitizer: address | thread | undefined | kombinace (carkou)")
if(ITHACA_SANITIZE)
    message(STATUS "Sanitizer: ${ITHACA_SANITIZE}")
    add_compile_options(-fsanitize=${ITHACA_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${ITHACA_SANITIZE})
endif()

# Default build type = Release (RT audio engine — bez optimalizaci nepouzitelny).
# Pri zapnutem sanitizeru radeji Debug (citelne stacky, bez LTO).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    if(ITHACA_SANITIZE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    else()
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
endif()

# ARM (Raspberry Pi 5 / aarch64): tune na nativni CPU (Cortex-A76 + NEON).
# Jen na Linux ARM, aby to nezasahlo do macOS/x86 buildu. Predpoklada se native
# build na cilovem zarizeni; pri cross-compile flag uprav/vynech.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|armv8|armv7")
    add_compile_options($<$<CONFIG:Release>:-mcpu=native>)
endif()

# LTO pro Release, pokud toolchain podporuje (mensi + rychlejsi binarka).
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
if(_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    message(STATUS "IPO/LTO nepodporovano: ${_ipo_msg}")
endif()

# -- vendored deps --
add_subdirectory(third-party)

# -- libithaca_core (headless knihovna) --
# Ve fazi 1 obsahuje jen logger. V dalsich fazich pribydou sample/voice/dsp moduly.
add_library(ithaca_core STATIC
    engine/util/log.cpp
    engine/util/rt_priority.cpp
    engine/io/wav_reader.cpp
    engine/io/wav_writer.cpp
    engine/io/audio_device.cpp
    engine/sample/bank_index.cpp
    engine/sample/sample_loader.cpp
    engine/sample/sample_store.cpp
    engine/stream/stream_engine.cpp
    engine/voice/patch_manager.cpp
    engine/voice/voice.cpp
    engine/voice/voice_pool.cpp
    engine/voice/resonance_voice.cpp
    engine/midi/midi_input.cpp
    engine/pedal/pedal_state.cpp
    engine/resonance/harmonic_proximity.cpp
    engine/resonance/resonance_layer_select.cpp
    engine/resonance/resonance_engine.cpp
    engine/engine.cpp
    engine/dsp/limiter.cpp
    engine/dsp/enhancer.cpp
    engine/dsp/agc.cpp
    engine/dsp/dsp_chain.cpp
    engine/dsp/ir_wav.cpp
    engine/dsp/ir_modal.cpp
    engine/dsp/convolver.cpp
    engine/render/batch_renderer.cpp
    engine/miniaudio_impl.cpp
)
target_include_directories(ithaca_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)
target_compile_features(ithaca_core PUBLIC cxx_std_20)

# Convolver: nejdrazsi per-sample FIR. -ffast-math (jen tento TU) odemkne
# autovektorizaci float-redukce v hot loopu (NEON/SSE/AVX). Denormaly resi
# globalni FTZ/DAZ (engine processBlock), IR/vstup jsou konecne → bezpecne.
# Sluchove identicke (reassociace ~ -120 dB). MSVC ekvivalent /fp:fast.
if(NOT ITHACA_SANITIZE)   # fast-math + sanitizer spolu nedavaji smysl
    if(MSVC)
        set_source_files_properties(engine/dsp/convolver.cpp PROPERTIES COMPILE_OPTIONS /fp:fast)
    else()
        set_source_files_properties(engine/dsp/convolver.cpp PROPERTIES COMPILE_OPTIONS -ffast-math)
    endif()
endif()
target_link_libraries(ithaca_core PUBLIC miniaudio_inc rtmidi)
if(APPLE)
    target_link_libraries(ithaca_core PUBLIC
        "-framework CoreAudio" "-framework AudioToolbox"
        "-framework CoreFoundation")
elseif(WIN32)
    # avrt = MMCSS API (AvSetMmThreadCharacteristicsW) pro RT audio thread —
    # viz engine/util/rt_priority.cpp.
    target_link_libraries(ithaca_core PUBLIC avrt)
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(ithaca_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# -- ithaca-cli (headless konzument) --
add_executable(ithaca-cli app/cli/main.cpp)
target_link_libraries(ithaca-cli PRIVATE ithaca_core)
set_target_properties(ithaca-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# -- ithaca-gui (F8: Dear ImGui + GLFW) --
# Staveni jen kdyz jsou ImGui/GLFW vendorovany (fetch-third-party.sh).
# Bez nich CLI a tests jdou stavet dal — GUI je optional.
if(TARGET imgui AND TARGET glfw)
    add_executable(ithaca-gui
        app/gui/main.cpp
        app/gui/app_context.cpp
        app/gui/log_subscriber.cpp
        app/gui/panel_topbar.cpp
        app/gui/panel_keyboard.cpp
        app/gui/panel_bank.cpp
        app/gui/panel_indicators.cpp
        app/gui/panel_config.cpp
        app/gui/panel_params.cpp
        app/gui/panel_log.cpp
        app/gui/persistence.cpp)
    target_include_directories(ithaca-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app/gui)
    target_link_libraries(ithaca-gui PRIVATE ithaca_core imgui glfw)
    set_target_properties(ithaca-gui PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    # ImGui headery delaji memset(this, 0, sizeof(*this)) v ImDrawList ctorech;
    # clang -Wnontrivial-memcall to flagne i pri pouhem #include. Vendored kod,
    # nemuzeme upravit — potlacit jen pro tento target.
    if(NOT MSVC)
        target_compile_options(ithaca-gui PRIVATE -Wno-nontrivial-memcall)
    endif()
    if(APPLE)
        # OpenGL je na macOS 10.14+ deprecated, ale stale plne funkcni.
        # ImGui i nase main.cpp pouzivaji legacy GL volani — silence.
        target_compile_definitions(ithaca-gui PRIVATE GL_SILENCE_DEPRECATION)
        target_link_libraries(ithaca-gui PRIVATE
            "-framework OpenGL" "-framework Cocoa" "-framework IOKit"
            "-framework CoreVideo")
    elseif(WIN32)
        target_link_libraries(ithaca-gui PRIVATE opengl32)
    elseif(UNIX)
        target_link_libraries(ithaca-gui PRIVATE GL)
    endif()
endif()

# -- testy (doctest pres CTest) --
enable_testing()
add_subdirectory(tests)
