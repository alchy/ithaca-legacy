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

# 64-bit off_t i na 32-bit Unix cilech (armv7 RPi): pakovana banka je jeden
# multi-GB soubor, pread offsety musi byt 64-bit. Na 64-bit/macOS no-op.
if(UNIX)
    add_compile_definitions(_FILE_OFFSET_BITS=64)
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

# -- GUI: OpenGL ES misto desktopoveho GL --------------------------------
# Mesa ovladac V3D na Raspberry Pi poskytuje OpenGL ES, ne desktopove GL 3.3.
# Zapina se automaticky na Linuxu/ARM, protoze tam je to jedina spravna volba;
# jinde jde vynutit rucne (-DITHACA_GUI_GLES=ON) pro zkousku.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|armv8|armv7")
    set(_gles_default ON)
else()
    set(_gles_default OFF)
endif()
option(ITHACA_GUI_GLES "GUI pouzije OpenGL ES 3 misto desktopoveho GL 3.3"
       ${_gles_default})
if(ITHACA_GUI_GLES)
    message(STATUS "GUI: OpenGL ES 3 (KMSDRM/V3D)")
endif()

# -- vendored deps --
add_subdirectory(third-party)

# -- Master secret pro pakovanou banku v2 (gitignored, build-time) --
# Idempotentni: gen-bank-secret.py vytvori jen kdyz chybi. Z nej se vygeneruje
# bank_secret_generated.h (32 B jako C pole) do build adresare.
set(ITHACA_BANK_SECRET_FILE "${CMAKE_SOURCE_DIR}/secret/bank_secret.key")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen-bank-secret.py
            ${ITHACA_BANK_SECRET_FILE}
    RESULT_VARIABLE _sk_rc)
if(NOT _sk_rc EQUAL 0)
    message(FATAL_ERROR "gen-bank-secret.py selhal (rc=${_sk_rc})")
endif()
file(READ "${ITHACA_BANK_SECRET_FILE}" _sk_hex HEX)
string(REGEX MATCHALL "([0-9a-f][0-9a-f])" _sk_bytes "${_sk_hex}")
string(JOIN ",0x" _sk_join ${_sk_bytes})
set(_sk_arr "0x${_sk_join}")
file(WRITE "${CMAKE_BINARY_DIR}/generated/bank_secret_generated.h"
"#pragma once\n// AUTO-GENEROVANO z secret/bank_secret.key — NEcommitovat.\n"
"#include <cstdint>\nnamespace ithaca {\n"
"inline constexpr uint8_t kBankSecret[32] = {${_sk_arr}};\n} // namespace ithaca\n")

# -- libithaca_core (headless knihovna) --
# Ve fazi 1 obsahuje jen logger. V dalsich fazich pribydou sample/voice/dsp moduly.
add_library(ithaca_core STATIC
    engine/util/log.cpp
    engine/util/rt_priority.cpp
    engine/util/sha256.cpp
    engine/util/ithaca_crypto.cpp
    engine/io/wav_reader.cpp
    engine/io/wav_writer.cpp
    engine/io/file_handle.cpp
    engine/io/sample_read.cpp
    engine/io/audio_device.cpp
    engine/sample/bank_index.cpp
    engine/sample/ithaca_format.cpp
    engine/sample/ithaca_bank.cpp
    engine/sample/sample_loader.cpp
    engine/sample/sample_store.cpp
    engine/sample/resonance_layer_select.cpp
    engine/stream/stream_engine.cpp
    engine/stream/streamed_reader.cpp
    engine/voice/patch_manager.cpp
    engine/voice/voice.cpp
    engine/voice/voice_pool.cpp
    engine/voice/resonance_voice.cpp
    engine/midi/midi_input.cpp
    engine/pedal/pedal_state.cpp
    engine/resonance/harmonic_proximity.cpp
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
target_include_directories(ithaca_core PUBLIC ${CMAKE_BINARY_DIR}/generated)
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

# -- ithaca-gui (Dear ImGui + SDL3) --
# Staveni jen kdyz jsou ImGui/SDL3 vendorovany (fetch-third-party.sh).
# Bez nich CLI a tests jdou stavet dal — GUI je optional.
if(TARGET imgui AND TARGET SDL3::SDL3-static)
    # -- Pisma zabudovana primo do binarky -------------------------------------
    # Zadny font se NEnacita ze souboru: ImGui nastroj binary_to_compressed_c
    # (vendorovany s ImGui) prevede kazde TTF na komprimovane C pole, ktere se
    # prilinkuje. Vysledek: ithaca-gui za behu nepotrebuje zadny asset, jen svuj
    # state.json. .inl se generuji do build adresare, do gitu se necommituji.
    #
    # Rozhrani ma JEDNU vahu (JetBrains Mono Regular) — na skutecnych znakovych
    # displejich zadny tucny rez neexistuje, duraz se dela inverzi pole.
    # Dve vahy Barlow Condensed jsou jen pro wordmark na uvodni obrazovce.
    #
    # Pozn.: nastroj se stavi pro HOST. Pri cross-compilaci je potreba pouzit
    # uz vygenerovane .inl nebo nastroj postavit zvlast nativne.
    add_executable(binary_to_compressed_c
        third-party/imgui/misc/fonts/binary_to_compressed_c.cpp)

    # "<soubor>|<symbol>" — symbol se objevi jako <symbol>_compressed_data/_size.
    set(ITHACA_FONTS
        "fonts/JetBrainsMono-Regular.ttf|JetBrainsMono"
        "fonts/BarlowCondensed-Bold.ttf|BarlowCondensedBold"
        "fonts/BarlowCondensed-Light.ttf|BarlowCondensedLight")
    set(FONT_INLS "")
    foreach(spec IN LISTS ITHACA_FONTS)
        string(REPLACE "|" ";" parts "${spec}")
        list(GET parts 0 rel)
        list(GET parts 1 sym)
        set(ttf ${CMAKE_CURRENT_SOURCE_DIR}/third-party/${rel})
        set(inl ${CMAKE_BINARY_DIR}/generated/${sym}.inl)
        add_custom_command(
            OUTPUT ${inl}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
            COMMAND binary_to_compressed_c ${ttf} ${sym} > ${inl}
            DEPENDS binary_to_compressed_c ${ttf}
            COMMENT "Generuji zabudovane pismo ${sym}"
            VERBATIM)
        list(APPEND FONT_INLS ${inl})
    endforeach()
    add_custom_target(ithaca_fonts DEPENDS ${FONT_INLS})

    add_executable(ithaca-gui
        app/gui/main.cpp
        app/gui/app_context.cpp
        app/gui/embedded_fonts.cpp
        app/gui/log_subscriber.cpp
        app/gui/screen.cpp
        app/gui/splash.cpp
        app/gui/page_play.cpp
        app/gui/page_bank.cpp
        app/gui/page_params.cpp
        app/gui/page_sys.cpp
        app/gui/page_log.cpp
        app/gui/persistence.cpp)
    target_include_directories(ithaca-gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/app/gui
        ${CMAKE_BINARY_DIR}/generated)
    add_dependencies(ithaca-gui ithaca_fonts)
    target_link_libraries(ithaca-gui PRIVATE ithaca_core imgui SDL3::SDL3-static)
    if(ITHACA_GUI_GLES)
        target_compile_definitions(ithaca-gui PRIVATE ITHACA_GLES)
    endif()
    set_target_properties(ithaca-gui PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    # ImGui headery delaji memset(this, 0, sizeof(*this)) v ImDrawList ctorech;
    # clang -Wnontrivial-memcall to flagne i pri pouhem #include. Vendored kod,
    # nemuzeme upravit — potlacit jen pro tento target.
    if(NOT MSVC)
        target_compile_options(ithaca-gui PRIVATE -Wno-nontrivial-memcall)
    endif()
    # GL knihovna. SDL3 si systemove zavislosti (Cocoa, Win32, DRM/GBM/EGL)
    # pritahne samo pres svuj target; tady se resi jen samotne GL/GLES.
    if(APPLE)
        # OpenGL je na macOS 10.14+ deprecated, ale stale plne funkcni.
        # ImGui i nase main.cpp pouzivaji legacy GL volani — silence.
        target_compile_definitions(ithaca-gui PRIVATE GL_SILENCE_DEPRECATION)
        target_link_libraries(ithaca-gui PRIVATE "-framework OpenGL")
    elseif(WIN32)
        target_link_libraries(ithaca-gui PRIVATE opengl32)
    elseif(ITHACA_GUI_GLES)
        target_link_libraries(ithaca-gui PRIVATE GLESv2 EGL)
    elseif(UNIX)
        target_link_libraries(ithaca-gui PRIVATE GL)
    endif()
endif()

# -- testy (doctest pres CTest) --
enable_testing()
add_subdirectory(tests)
