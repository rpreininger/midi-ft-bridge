cmake_minimum_required(VERSION 3.10)
project(midi_ft_bridge VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# Common source files (platform-independent)
set(COMMON_SOURCES
    src/main.cpp
    src/video_player.cpp
    src/clip_player.cpp
    src/ft_sender.cpp
    src/lodepng.cpp
    src/status_server.cpp
)

# ---- Platform detection ----

if(CMAKE_CROSSCOMPILING)
    # ----- Cross-compilation for Raspberry Pi -----
    message(STATUS "Cross-compiling for ${CMAKE_SYSTEM_PROCESSOR}")

    set(PLATFORM_SOURCES
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
    # ----- macOS native build -----
    message(STATUS "Building for macOS (stubs for ALSA/BlueZ)")

    set(PLATFORM_SOURCES
        src/midi_input_stub.cpp
        src/audio_player_stub.cpp
        src/ble_sender_stub.cpp
    )

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBAV REQUIRED libavformat libavcodec libswscale libswresample libavutil)
    set(FFMPEG_INCLUDE_DIRS ${LIBAV_INCLUDE_DIRS})

    set(PLATFORM_INCLUDE_DIRS "")
    set(PLATFORM_LIBRARIES ${LIBAV_LIBRARIES} pthread)

    # SDL2 panel viewer (macOS only)
    pkg_check_modules(SDL2 sdl2)
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
        message(STATUS "Panel viewer: disabled (install SDL2: brew install sdl2)")
    endif()

else()
    # ----- Linux native build -----
    message(STATUS "Building for Linux (native)")

    set(PLATFORM_SOURCES
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

if(APPLE)
    target_link_directories(midi_ft_bridge PRIVATE ${LIBAV_LIBRARY_DIRS})
endif()

target_link_libraries(midi_ft_bridge ${PLATFORM_LIBRARIES})

install(TARGETS midi_ft_bridge DESTINATION bin)
