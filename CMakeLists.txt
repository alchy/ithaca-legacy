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
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# -- vendored deps --
add_subdirectory(third-party)

# -- libithaca_core (headless knihovna) --
# Ve fazi 1 obsahuje jen logger. V dalsich fazich pribydou sample/voice/dsp moduly.
add_library(ithaca_core STATIC
    engine/util/log.cpp
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
    engine/resonance/resonance_engine.cpp
    engine/engine.cpp
    engine/render/batch_renderer.cpp
    engine/miniaudio_impl.cpp
)
target_include_directories(ithaca_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)
target_compile_features(ithaca_core PUBLIC cxx_std_20)
target_link_libraries(ithaca_core PUBLIC miniaudio_inc rtmidi)
if(APPLE)
    target_link_libraries(ithaca_core PUBLIC
        "-framework CoreAudio" "-framework AudioToolbox"
        "-framework CoreFoundation")
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(ithaca_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# -- ithaca-cli (headless konzument) --
add_executable(ithaca-cli app/cli/main.cpp)
target_link_libraries(ithaca-cli PRIVATE ithaca_core)
set_target_properties(ithaca-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# -- testy (doctest pres CTest) --
enable_testing()
add_subdirectory(tests)
