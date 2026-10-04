#pragma once

// Command-line parsing for the application shell, with no UI and no SDL. It
// lives in haiku_remote_core so the SDL front end and any future one agree on
// exactly what the flags mean, and so the parsing is unit-tested without a
// window.
//
// Two outcomes: with no endpoint flag the app opens its connection library;
// with endpoint flags it builds an EPHEMERAL, never-saved ConnectionProfile and
// connects immediately, preserving the automation/debug workflow the old
// framebuffer client had. Every flag the SDL client already accepted is kept;
// the SSH/profile flags issue #1 asks for are added.

#include "haiku_remote/connection_coordinator.hpp"
#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/reconnect.hpp"
#include "haiku_remote/transport.hpp"

#include <string>
#include <string_view>

namespace haiku_remote {

struct LaunchOptions {
    // True when no endpoint was given: open the connection library rather than
    // connect. False when an endpoint flag (or --profile) was supplied.
    bool open_library = true;
    // A saved profile named by --profile <id-or-name>, resolved by the caller
    // against a ProfileStore. Empty when not given.
    std::string profile_ref;
    // The ephemeral connection described by the endpoint flags. Never written to
    // disk. Meaningful only when open_library is false and profile_ref is empty.
    ConnectionProfile profile;
    // Credentials and TLS posture (cookie/token/pin/ca/insecure) plus the
    // optional broker URL. profile_ref aside, this is the credential source; the
    // cookie is copied onto the coordinator config.
    TransportOptions transport;
    // --url was given: the coordinator uses `transport` verbatim (broker path).
    bool uses_url = false;
    // Reconnect policy from the shared --reconnect* flags.
    ReconnectConfig reconnect;
    int width = 1280;
    int height = 800;
    bool size_explicit = false;
};

struct LaunchResult {
    bool ok = true;          // false: print `error` and exit `exit_code`
    bool show_help = false;  // true: print launch_usage() and exit 0
    int exit_code = exit_status::ok;
    std::string error;       // field-named validation or parse error
    LaunchOptions options;
};

// Parse argv. Never throws: a bad value or an invalid ephemeral profile comes
// back as ok == false with exit_code == exit_status::usage and a field-named
// message. --help/-h sets show_help.
[[nodiscard]] LaunchResult parse_launch_options(int argc, char** argv);

// The usage block for --help, covering the preserved and the new flags.
[[nodiscard]] std::string_view launch_usage();

// Assemble a CoordinatorConfig from parsed options. For a --profile launch the
// caller must first copy the resolved saved profile into options.profile; this
// uses options.profile unconditionally.
[[nodiscard]] CoordinatorConfig coordinator_config_from(const LaunchOptions& options);

} // namespace haiku_remote
