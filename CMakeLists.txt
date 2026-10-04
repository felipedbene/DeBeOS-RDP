cmake_minimum_required(VERSION 3.20)
project(HaikuRemote VERSION 0.1.0 LANGUAGES CXX)

option(HAIKU_REMOTE_FETCH_SDL2
    "Download a pinned SDL2 release when no system SDL2 package is available" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PNG REQUIRED)
find_package(Freetype REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)

add_library(haiku_remote_core
    src/application_state.cpp
    src/connection_coordinator.cpp
    src/connection_profile.cpp
    src/input_encoder.cpp
    src/json.cpp
    src/launch_options.cpp
    src/library_controller.cpp
    src/png_writer.cpp
    src/profile_store.cpp
    src/protocol.cpp
    src/reconnect.cpp
    src/session.cpp
    src/ssh_tunnel.cpp
    src/surface.cpp
    src/tcp_socket.cpp
    src/text_engine.cpp
    src/text_field_model.cpp
    src/transport.cpp
)
if(WIN32)
    target_sources(haiku_remote_core PRIVATE src/ssh_tunnel_windows.cpp)
else()
    target_sources(haiku_remote_core PRIVATE src/ssh_tunnel_posix.cpp)
endif()
target_include_directories(haiku_remote_core PUBLIC include)
target_link_libraries(haiku_remote_core
    PUBLIC PNG::PNG Freetype::Freetype PkgConfig::HARFBUZZ Threads::Threads
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
if(HAIKU)
    target_link_libraries(haiku_remote_core PUBLIC network)
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
    # The SDL frontend is now a view-switching shell (issue #1): the connection
    # library, the profile editor, and the connecting/failed screens front the
    # desktop view, which is the old framebuffer loop lifted into ui/. These
    # units are the ONLY place SDL is linked; the state-driven flow they render
    # lives SDL-free in haiku_remote_core (LibraryController) and is unit-tested
    # without a window.
    add_executable(haiku-remote-gui
        src/sdl_app.cpp
        src/ui/widgets.cpp
        src/ui/library_view.cpp
        src/ui/profile_editor.cpp
        src/ui/connecting_view.cpp
        src/ui/desktop_view.cpp
    )
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

    # Connection library: profile validation, the JSON seam, and the store.
    add_executable(haiku-remote-profile-tests tests/profile_tests.cpp)
    target_link_libraries(haiku-remote-profile-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-profile-tests COMMAND haiku-remote-profile-tests)

    # Managed SSH tunnel: argv builder, free-port/readiness, orchestration via a
    # fake ssh child, and the fail-closed pidfile reaper.
    add_executable(haiku-remote-tunnel-tests tests/tunnel_tests.cpp)
    target_link_libraries(haiku-remote-tunnel-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-tunnel-tests COMMAND haiku-remote-tunnel-tests)

    # Connection coordinator: the worker-thread state machine, the handshake
    # timeout, SSH-failure classification, launch-flag parsing, and the app-mode
    # mapping. Driven by a fake tunnel and an injected transport -- no sockets,
    # no ssh, no python.
    add_executable(haiku-remote-coordinator-tests tests/coordinator_tests.cpp)
    target_link_libraries(haiku-remote-coordinator-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-coordinator-tests COMMAND haiku-remote-coordinator-tests)

    # GUI connection-library flow: the LibraryController state machine and the
    # text-field edit model that back the SDL shell, driven with a temp store and
    # a fake-coordinator factory -- no SDL, no window, no ssh. This is the
    # state-driven flow the SDL views only render.
    add_executable(haiku-remote-gui-flow-tests tests/gui_flow_tests.cpp)
    target_link_libraries(haiku-remote-gui-flow-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-gui-flow-tests COMMAND haiku-remote-gui-flow-tests)
endif()
