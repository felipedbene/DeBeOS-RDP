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
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)

add_library(haiku_remote_core
    src/connect_flow.cpp
    src/connect_screen.cpp
    src/desktop_shell.cpp
    src/desktop_session.cpp
    src/test_script.cpp
    src/connection_profile.cpp
    src/input_encoder.cpp
    src/json.cpp
    src/known_brokers.cpp
    src/launch_arguments.cpp
    src/library_screen.cpp
    src/managed_transport.cpp
    src/png_writer.cpp
    src/profile_launch.cpp
    src/profile_library.cpp
    src/profile_store.cpp
    src/protocol.cpp
    src/wire_decoder.cpp
    src/reconnect.cpp
    src/session.cpp
    src/surface.cpp
    src/tcp_socket.cpp
    src/text_engine.cpp
    src/transport.cpp
)
target_include_directories(haiku_remote_core PUBLIC include)
# managed_transport.cpp drains each child's stderr on a std::thread.
find_package(Threads REQUIRED)
target_link_libraries(haiku_remote_core
    PUBLIC PNG::PNG Freetype::Freetype PkgConfig::HARFBUZZ Threads::Threads
)

# WebSocket-over-TLS transport for the DeBeOS remote-desktop broker. Without
# OpenSSL the client still builds with the raw TCP transport only.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND OR TARGET OpenSSL::SSL)
    target_sources(haiku_remote_core PRIVATE src/websocket.cpp
        src/broker_handshake.cpp src/quic.cpp)
    target_compile_definitions(haiku_remote_core PUBLIC HAIKU_REMOTE_HAVE_WSS)
    target_link_libraries(haiku_remote_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
    message(STATUS "OpenSSL not found: building without ws:// and wss:// support")
endif()
# zstd stream compression (RP_CAP_COMPRESS_ZSTD) is in every build (#23): the
# system libzstd when pkg-config finds one, else the vendored single-file decoder
# in third_party/zstd (zstd 1.5.7, see its README). NONE builds the no-decoder
# client, which never offers the bit; it exists only to test that refusal path.
set(HAIKU_REMOTE_ZSTD "AUTO" CACHE STRING "zstd decoder: AUTO, SYSTEM, VENDORED or NONE")
set_property(CACHE HAIKU_REMOTE_ZSTD PROPERTY STRINGS AUTO SYSTEM VENDORED NONE)
pkg_check_modules(ZSTD QUIET IMPORTED_TARGET libzstd)
set(_zstd_source "${HAIKU_REMOTE_ZSTD}")
if(_zstd_source STREQUAL "AUTO")
    if(ZSTD_FOUND)
        set(_zstd_source "SYSTEM")
    else()
        set(_zstd_source "VENDORED")
    endif()
endif()
if(_zstd_source STREQUAL "SYSTEM")
    if(NOT ZSTD_FOUND)
        message(FATAL_ERROR "HAIKU_REMOTE_ZSTD=SYSTEM, but pkg-config finds no libzstd")
    endif()
    # The system library also has the encoder, which the tests use.
    target_compile_definitions(haiku_remote_core PUBLIC
        HAIKU_REMOTE_HAVE_ZSTD HAIKU_REMOTE_HAVE_ZSTD_ENCODER)
    target_link_libraries(haiku_remote_core PUBLIC PkgConfig::ZSTD)
elseif(_zstd_source STREQUAL "VENDORED")
    enable_language(C)
    add_library(haiku_remote_zstd STATIC third_party/zstd/zstddeclib.c)
    target_include_directories(haiku_remote_zstd PUBLIC third_party/zstd)
    set_target_properties(haiku_remote_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(haiku_remote_core PUBLIC HAIKU_REMOTE_HAVE_ZSTD)
    target_link_libraries(haiku_remote_core PUBLIC haiku_remote_zstd)
elseif(_zstd_source STREQUAL "NONE")
    # The one build allowed to lack the decoder; see tests.cpp.
    target_compile_definitions(haiku_remote_core PUBLIC HAIKU_REMOTE_ZSTD_NONE)
    message(FATAL_ERROR "HAIKU_REMOTE_ZSTD must be AUTO, SYSTEM, VENDORED or NONE")
endif()
message(STATUS "zstd stream compression: ${_zstd_source} decoder")
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

    # RP_CAP_FONT_IDENTITY: the block's wire format, resolution by name and by
    # file identity, and text drawn exactly as app_server lays it out.
    add_executable(haiku-remote-font-tests tests/font_tests.cpp)
    target_link_libraries(haiku-remote-font-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-font-tests COMMAND haiku-remote-font-tests)

    # Connection library: profile validation, the JSON seam, and the store.
    add_executable(haiku-remote-profile-tests tests/profile_tests.cpp)
    target_link_libraries(haiku-remote-profile-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-profile-tests COMMAND haiku-remote-profile-tests)

    # The library model (CRUD/order/search), the launch seam, and the screen's
    # event-to-action logic.
    add_executable(haiku-remote-library-tests tests/library_tests.cpp)
    target_link_libraries(haiku-remote-library-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-library-tests COMMAND haiku-remote-library-tests)

    # Part 2: managed connection transports -- SSH argv, broker/cookie fetch,
    # route selection and fallback, and the process-reap lifecycle guarantee.
    add_executable(haiku-remote-tunnel-tests tests/tunnel_tests.cpp)
    target_link_libraries(haiku-remote-tunnel-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-tunnel-tests COMMAND haiku-remote-tunnel-tests)

    # Part 3: the connection-progress state machine, the cause->message/
    # affordance classifier, connect_with_progress()'s failure path, and the
    # headless render of the progress and error screens.
    add_executable(haiku-remote-connect-tests tests/connect_tests.cpp)
    target_link_libraries(haiku-remote-connect-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-connect-tests COMMAND haiku-remote-connect-tests)

    # Part 4: cross-platform profile-format parity and per-OS config-path
    # resolution -- each OS's rule exercised from any host via a fake env, plus
    # the on-disk golden and byte-for-byte round-trip of Windows-style paths.
    add_executable(haiku-remote-xplatform-tests tests/xplatform_tests.cpp)
    target_link_libraries(haiku-remote-xplatform-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-xplatform-tests COMMAND haiku-remote-xplatform-tests)

    # Broker trust on first use: the known_brokers store, the decision table,
    # the terminal prompt and the trust-prompt screen, plus (with OpenSSL) an
    # in-process TLS broker exercising unknown -> trust -> known -> changed.
    find_package(Threads REQUIRED)
    add_executable(haiku-remote-trust-tests tests/trust_tests.cpp)
    target_link_libraries(haiku-remote-trust-tests PRIVATE haiku_remote_core
        Threads::Threads)
    add_test(NAME haiku-remote-trust-tests COMMAND haiku-remote-trust-tests)

    # Desktop shell: the in-session toolbar, the repaint lever, the shortcut
    # wire bytes and the GUI's time-driven reconnect schedule.
    add_executable(haiku-remote-shell-tests tests/shell_tests.cpp)
    target_link_libraries(haiku-remote-shell-tests PRIVATE haiku_remote_core)
    add_test(NAME haiku-remote-shell-tests COMMAND haiku-remote-shell-tests)

    # Issue #1 end to end: a saved profile against tools/rp_mock_server.py
    # through progress, handshake, desktop and disconnect -- back to the
    # library, when the GUI is built -- with a fake ssh on PATH
    # (tests/e2e/fake-ssh). POSIX only: the fake ssh is a #! script.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND NOT WIN32)
        set(HAIKU_REMOTE_E2E_ARGS --client $<TARGET_FILE:haiku-remote>)
        if(TARGET haiku-remote-gui)
            list(APPEND HAIKU_REMOTE_E2E_ARGS --gui $<TARGET_FILE:haiku-remote-gui>)
        endif()
        # The QUIC route's broker stand-in, on OpenSSL >= 3.5's QUIC server
        # API: built exactly when quic.hpp compiles quic:// into the client,
        # and --quic-bridge is how the harness learns the client has QUIC.
        if(TARGET OpenSSL::SSL AND OPENSSL_VERSION VERSION_GREATER_EQUAL 3.5)
            add_executable(quic_bridge_mock tools/quic_bridge_mock.cpp)
            target_link_libraries(quic_bridge_mock PRIVATE OpenSSL::SSL
                OpenSSL::Crypto Threads::Threads)
            list(APPEND HAIKU_REMOTE_E2E_ARGS
                --quic-bridge $<TARGET_FILE:quic_bridge_mock>)
        endif()
        add_test(NAME haiku-remote-e2e
            COMMAND Python3::Interpreter
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/profile_e2e.py
                ${HAIKU_REMOTE_E2E_ARGS})
    else()
        message(WARNING "haiku-remote-e2e: SKIPPED (needs python3 and a POSIX"
                        " host for the fake ssh)")
    endif()
endif()
