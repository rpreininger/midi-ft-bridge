cmake_minimum_required(VERSION 3.10)
project(midi_ft_bridge VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# Source files
set(SOURCES
    src/main.cpp
    src/midi_input.cpp
    src/video_player.cpp
    src/audio_player.cpp
    src/clip_player.cpp
    src/ft_sender.cpp
    src/ble_sender.cpp
    src/lodepng.cpp
    src/status_server.cpp
)

# Cross-compilation detection
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Cross-compiling for ${CMAKE_SYSTEM_PROCESSOR}")

    # Cross-compile libs directory (sibling 'raspi' project holds Pi libs)
    set(XLIBS_DIR "${CMAKE_SOURCE_DIR}/../raspi/libs")
    message(STATUS "Cross-compile libs: ${XLIBS_DIR}")

    # ALSA (for MIDI input)
    set(ALSA_LIBRARIES asound)
    set(ALSA_INCLUDE_DIRS "${XLIBS_DIR}")
    message(STATUS "Using ALSA: ${ALSA_LIBRARIES}")

    # D-Bus (for BLE via BlueZ)
    set(DBUS_LIBRARIES dbus-1)
    set(DBUS_INCLUDE_DIRS "${XLIBS_DIR}/dbus")

    # FFmpeg libraries (cross-compiled .so files)
    # Needs: libavformat, libavcodec, libswscale, libswresample, libavutil headers
    set(FFMPEG_INCLUDE_DIRS "${XLIBS_DIR}/ffmpeg")

    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(LIBS_ARCH_DIR ${XLIBS_DIR}/aarch64)
    else()
        set(LIBS_ARCH_DIR ${XLIBS_DIR}/arm)
    endif()

    link_directories(${LIBS_ARCH_DIR})

    # Allow unresolved symbols from shared libs (resolved at runtime on Pi)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")

else()
    # Native compilation - use pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
    pkg_check_modules(LIBAV REQUIRED libavformat libavcodec libswscale libswresample libavutil)
    pkg_check_modules(DBUS REQUIRED dbus-1)
    set(FFMPEG_INCLUDE_DIRS ${LIBAV_INCLUDE_DIRS})
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${ALSA_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS}
)

# Create executable
add_executable(midi_ft_bridge ${SOURCES})

# Link libraries
if(CMAKE_CROSSCOMPILING)
    target_link_libraries(midi_ft_bridge
        ${ALSA_LIBRARIES}
        ${DBUS_LIBRARIES}
        avformat
        avcodec
        swscale
        swresample
        avutil
        pthread
        m
    )
else()
    target_link_libraries(midi_ft_bridge
        ${ALSA_LIBRARIES}
        ${LIBAV_LIBRARIES}
        ${DBUS_LIBRARIES}
        pthread
        m
    )
endif()

# Install
install(TARGETS midi_ft_bridge DESTINATION bin)
