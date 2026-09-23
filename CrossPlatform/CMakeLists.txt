cmake_minimum_required(VERSION 3.20)
project(HaikuRemoteCrossPlatform VERSION 0.1.0 LANGUAGES CXX)

option(HAIKU_REMOTE_FETCH_SDL2
    "Download a pinned SDL2 release when no system SDL2 package is available" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PNG REQUIRED)
find_package(Freetype REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)

add_library(haiku_remote_core
    src/input_encoder.cpp
    src/png_writer.cpp
    src/protocol.cpp
    src/reconnect.cpp
    src/session.cpp
    src/surface.cpp
    src/tcp_socket.cpp
    src/text_engine.cpp
    src/transport.cpp
)
target_include_directories(haiku_remote_core PUBLIC include)
target_link_libraries(haiku_remote_core
    PUBLIC PNG::PNG Freetype::Freetype PkgConfig::HARFBUZZ
)

# WebSocket-over-TLS transport for the DeBeOS remote-desktop broker. Without
# OpenSSL the client still builds with the raw TCP transport only.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND OR TARGET OpenSSL::SSL)
    target_sources(haiku_remote_core PRIVATE src/websocket.cpp)
    target_compile_definitions(haiku_remote_core PUBLIC HAIKU_REMOTE_HAVE_WSS)
    target_link_libraries(haiku_remote_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
    message(STATUS "OpenSSL not found: building without ws:// and wss:// support")
endif()
if(WIN32)
    target_link_libraries(haiku_remote_core PUBLIC ws2_32)
endif()
if(MSVC)
    target_compile_options(haiku_remote_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(haiku_remote_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(haiku-remote src/main.cpp)
target_link_libraries(haiku-remote PRIVATE haiku_remote_core)

find_package(SDL2 CONFIG QUIET)
if(NOT TARGET SDL2::SDL2)
    pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
endif()
if(NOT TARGET SDL2::SDL2 AND NOT TARGET PkgConfig::SDL2
   AND HAIKU_REMOTE_FETCH_SDL2)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-2.30.9
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL2)
endif()
if(TARGET SDL2::SDL2 OR TARGET PkgConfig::SDL2)
    add_executable(haiku-remote-gui src/sdl_main.cpp)
    if(TARGET SDL2::SDL2)
        target_link_libraries(haiku-remote-gui PRIVATE haiku_remote_core SDL2::SDL2)
    else()
        target_link_libraries(
            haiku-remote-gui PRIVATE haiku_remote_core PkgConfig::SDL2)
    endif()
else()
    # A silently-skipped target reads exactly like a passing one (issue #26).
    # Warn loudly so a configure log makes plain that the SDL frontend is not
    # being built, rather than leaving its absence to be discovered later.
    message(WARNING
        "haiku-remote-gui: SKIPPED (SDL2 not found; install SDL2 or configure "
        "with -DHAIKU_REMOTE_FETCH_SDL2=ON to build the SDL frontend)")
endif()

find_package(X11 QUIET)
if(X11_FOUND)
    add_executable(haiku-remote-x11 src/x11_main.cpp)
    target_link_libraries(haiku-remote-x11 PRIVATE haiku_remote_core X11::X11)
else()
    message(WARNING
        "haiku-remote-x11: SKIPPED (X11 not found; install X11 to build the "
        "X11 frontend)")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(haiku-remote-tests tests/tests.cpp)
    target_link_libraries(haiku-remote-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-tests COMMAND haiku-remote-tests)

    # Line caps and the RP_SET_FONT shear/rotation/false-bold attributes.
    add_executable(haiku-remote-render-tests tests/render_tests.cpp)
    target_link_libraries(haiku-remote-render-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-render-tests COMMAND haiku-remote-render-tests)
endif()
