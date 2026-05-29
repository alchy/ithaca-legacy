# third-party/CMakeLists.txt
# --------------------------
# Vendored deps jako CMake targety. Zadny FetchContent — vse source-vendored
# pres tools/fetch-third-party.sh. Targety:
#   doctest         INTERFACE  (header-only, jen pro testy)
#   nlohmann_json   INTERFACE  (header-only, config + banky v dalsich fazich)
#   miniaudio_inc   INTERFACE  (header-only, audio out v dalsich fazich)
#   rtmidi          STATIC     (MIDI in v dalsich fazich)

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
