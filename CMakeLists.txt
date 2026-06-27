cmake_minimum_required(VERSION 3.10)
project(midi_ft_bridge VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# ====================================================================
#  macOS-only build (native AVFoundation decode + CoreAudio + CoreMIDI).
#
#  The Raspberry Pi / Linux build (ALSA, FFmpeg, BlueZ) was frozen at git
#  tag `raspi-final` and removed from this tree; restore it with
#  `git checkout raspi-final` if ever needed.
#
#  This CMake build produces a headless CLI binary. The shipping macOS app
#  is built from the Xcode project in mac-app/ (which adds real CoreBluetooth
#  BLE output; the CLI binary here uses the BLE stub, since CoreBluetooth
#  needs an app bundle for the Bluetooth permission prompt).
# ====================================================================

if(NOT APPLE)
    message(FATAL_ERROR
        "This tree is macOS-only. The Raspberry Pi / Linux build lives at "
        "git tag `raspi-final` (git checkout raspi-final).")
endif()

message(STATUS "Building for macOS (native: AVFoundation decode + audio, CoreMIDI)")

# Tell the shared headers (clip_player.h) to use the AVFoundation pimpl
# variant instead of the FFmpeg VideoPlayer/AudioPlayer members.
add_compile_definitions(MFB_NATIVE_MACOS)

set(SOURCES
    src/main.cpp
    src/engine.cpp
    src/ft_sender.cpp
    src/lodepng.cpp
    src/status_server.cpp
    src/clip_player_macos.mm
    src/midi_input_macos.cpp
    src/audio_player_macos_native.cpp
    src/audio_output_macos.mm
    src/ble_sender_stub.cpp
)

# AVFoundation .mm files use ARC (manages the ObjC members inside the
# C++ pimpl struct automatically).
set_source_files_properties(src/clip_player_macos.mm src/audio_output_macos.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")

set(PLATFORM_LIBRARIES pthread
    "-framework AVFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework QuartzCore"
    "-framework AudioToolbox"
    "-framework CoreAudio"
    "-framework CoreMIDI"
    "-framework CoreFoundation"
    "-framework Foundation")

# Optional SDL2 panel viewer dev tool (not part of the bridge).
# Built only if SDL2 happens to be installed.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 sdl2)
endif()
if(SDL2_FOUND)
    add_executable(panel_viewer tools/panel_viewer.cpp)
    target_include_directories(panel_viewer PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SDL2_INCLUDE_DIRS}
    )
    target_link_directories(panel_viewer PRIVATE ${SDL2_LIBRARY_DIRS})
    target_link_libraries(panel_viewer ${SDL2_LIBRARIES})
    message(STATUS "Panel viewer: enabled (SDL2 found)")
else()
    message(STATUS "Panel viewer: disabled (optional dev tool; brew install sdl2)")
endif()

# ---- Build midi_ft_bridge ----

include_directories(${CMAKE_SOURCE_DIR}/src)

add_executable(midi_ft_bridge ${SOURCES})

target_link_libraries(midi_ft_bridge ${PLATFORM_LIBRARIES})

install(TARGETS midi_ft_bridge DESTINATION bin)
