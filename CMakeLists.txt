cmake_minimum_required(VERSION 3.10)
project(midi_ft_bridge VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# Common source files (platform-independent).
# NOTE: the FFmpeg video/clip players are NOT common — the native macOS build
# replaces them with AVFoundation (clip_player_macos.mm), so they are added
# per-platform below.
set(COMMON_SOURCES
    src/main.cpp
    src/engine.cpp
    src/ft_sender.cpp
    src/lodepng.cpp
    src/status_server.cpp
)

# ---- Platform detection ----

if(CMAKE_CROSSCOMPILING)
    # ----- Cross-compilation for Raspberry Pi -----
    message(STATUS "Cross-compiling for ${CMAKE_SYSTEM_PROCESSOR}")

    set(PLATFORM_SOURCES
        src/video_player.cpp
        src/clip_player.cpp
        src/midi_input.cpp
        src/audio_player.cpp
        src/ble_sender.cpp
    )

    set(XLIBS_DIR "${CMAKE_SOURCE_DIR}/../raspi/libs")
    message(STATUS "Cross-compile libs: ${XLIBS_DIR}")

    set(ALSA_LIBRARIES asound)
    set(ALSA_INCLUDE_DIRS "${XLIBS_DIR}")
    set(DBUS_LIBRARIES dbus-1)
    set(DBUS_INCLUDE_DIRS "${XLIBS_DIR}/dbus")
    set(FFMPEG_INCLUDE_DIRS "${XLIBS_DIR}/ffmpeg")

    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(LIBS_ARCH_DIR ${XLIBS_DIR}/aarch64)
    else()
        set(LIBS_ARCH_DIR ${XLIBS_DIR}/arm)
    endif()

    link_directories(${LIBS_ARCH_DIR})
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")

    set(PLATFORM_INCLUDE_DIRS ${ALSA_INCLUDE_DIRS} ${DBUS_INCLUDE_DIRS})
    set(PLATFORM_LIBRARIES ${ALSA_LIBRARIES} ${DBUS_LIBRARIES}
        avformat avcodec swscale swresample avutil pthread m)

elseif(APPLE)
    # ----- macOS native build (AVFoundation, no FFmpeg / no SDL2) -----
    message(STATUS "Building for macOS (native: AVFoundation decode + audio, CoreMIDI)")

    # Tell the shared headers (clip_player.h) to use the AVFoundation pimpl
    # variant instead of the FFmpeg VideoPlayer/AudioPlayer members.
    add_compile_definitions(MFB_NATIVE_MACOS)

    set(PLATFORM_SOURCES
        src/clip_player_macos.mm
        src/midi_input_macos.cpp
        src/audio_player_macos_native.cpp
        src/ble_sender_stub.cpp
    )

    # AVFoundation .mm files use ARC (manages the ObjC members inside the
    # C++ pimpl struct automatically).
    set_source_files_properties(src/clip_player_macos.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")

    set(PLATFORM_INCLUDE_DIRS "")
    set(PLATFORM_LIBRARIES pthread
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework QuartzCore"
        "-framework AudioToolbox"
        "-framework CoreMIDI"
        "-framework CoreFoundation"
        "-framework Foundation")

    # Optional SDL2 panel viewer dev tool (not part of the bridge; uses no
    # FFmpeg). Built only if SDL2 happens to be installed.
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

else()
    # ----- Linux native build -----
    message(STATUS "Building for Linux (native)")

    set(PLATFORM_SOURCES
        src/video_player.cpp
        src/clip_player.cpp
        src/midi_input.cpp
        src/audio_player.cpp
        src/ble_sender.cpp
    )

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
    pkg_check_modules(LIBAV REQUIRED libavformat libavcodec libswscale libswresample libavutil)
    pkg_check_modules(DBUS REQUIRED dbus-1)
    set(FFMPEG_INCLUDE_DIRS ${LIBAV_INCLUDE_DIRS})

    set(PLATFORM_INCLUDE_DIRS ${ALSA_INCLUDE_DIRS} ${DBUS_INCLUDE_DIRS})
    set(PLATFORM_LIBRARIES ${ALSA_LIBRARIES} ${LIBAV_LIBRARIES} ${DBUS_LIBRARIES} pthread m)

endif()

# ---- Build midi_ft_bridge ----

set(SOURCES ${COMMON_SOURCES} ${PLATFORM_SOURCES})

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIRS}
    ${PLATFORM_INCLUDE_DIRS}
)

add_executable(midi_ft_bridge ${SOURCES})

target_link_libraries(midi_ft_bridge ${PLATFORM_LIBRARIES})

install(TARGETS midi_ft_bridge DESTINATION bin)
