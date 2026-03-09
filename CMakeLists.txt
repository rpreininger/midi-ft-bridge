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
    src/ft_sender.cpp
    src/status_server.cpp
)

# Cross-compilation detection
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Cross-compiling for ${CMAKE_SYSTEM_PROCESSOR}")

    # ALSA (for MIDI input)
    set(ALSA_LIBRARIES asound)
    set(ALSA_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/../libs")
    message(STATUS "Using ALSA: ${ALSA_LIBRARIES}")

    # FFmpeg libraries (cross-compiled .so files)
    # These need to be copied from the Pi:
    #   sudo apt install libavformat-dev libavcodec-dev libswscale-dev libavutil-dev
    #   scp /usr/lib/arm-linux-gnueabihf/libav*.so* to libs/arm/ (or libs/aarch64/)
    #   scp -r /usr/include/arm-linux-gnueabihf/libav* to libs/ffmpeg/
    set(FFMPEG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/../libs/ffmpeg")

    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(LIBS_ARCH_DIR ${CMAKE_SOURCE_DIR}/../libs/aarch64)
    else()
        set(LIBS_ARCH_DIR ${CMAKE_SOURCE_DIR}/../libs/arm)
    endif()

    link_directories(${LIBS_ARCH_DIR})

    # Allow unresolved symbols from shared libs (resolved at runtime on Pi)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")

else()
    # Native compilation - use pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
    pkg_check_modules(LIBAV REQUIRED libavformat libavcodec libswscale libavutil)
    set(FFMPEG_INCLUDE_DIRS ${LIBAV_INCLUDE_DIRS})
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${ALSA_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
)

# Create executable
add_executable(midi_ft_bridge ${SOURCES})

# Link libraries
if(CMAKE_CROSSCOMPILING)
    target_link_libraries(midi_ft_bridge
        ${ALSA_LIBRARIES}
        avformat
        avcodec
        swscale
        avutil
        pthread
        m
    )
else()
    target_link_libraries(midi_ft_bridge
        ${ALSA_LIBRARIES}
        ${LIBAV_LIBRARIES}
        pthread
        m
    )
endif()

# Install
install(TARGETS midi_ft_bridge DESTINATION bin)
