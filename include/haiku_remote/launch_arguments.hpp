#pragma once

// The command-line launch arguments every front end shares (DeBeOS-RDP issue
// #1, "Backward compatibility"): what to connect to, decided once, the same way,
// by haiku-remote and haiku-remote-gui.
//
// Four launch targets, in the order resolve_launch_target() checks them:
//
//   saved_profile -- --profile NAME|ID [--profiles FILE]. Launched through the
//                    managed path (plan_launch + connect_with_progress), exactly
//                    as a pick in the GUI's library is. Bypasses the library.
//   ssh_profile   -- --ssh-host HOST [--ssh-user U] [--ssh-port N]
//                    [--identity PATH] [--remote-port N] [--local-port N]
//                    [--cookie-file PATH]. An unsaved ssh-mode profile (quick
//                    connect), launched through the same managed path: an owned
//                    `ssh -L` forward, the cookie fetched over SSH unless
//                    --cookie-file names it. Bypasses the library.
//   endpoint      -- any of the transport arguments (--host, --port, --url,
//                    --cookie-file, ...), or --direct. The original raw
//                    connection, byte-for-byte unchanged: a plain TcpTransport
//                    (or ws/wss for --url) and no SSH. Bypasses the library.
//   library       -- nothing above: the GUI opens its connection library; the
//                    headless client, which has none, connects to the default
//                    endpoint as it always has.
//
// Mixing targets is a usage error rather than a silent precedence rule: an
// --ssh-host next to a --host, or a --profile next to a --port, has no single
// obvious meaning, and guessing is how an automation script ends up somewhere
// it did not ask for.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/transport.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

enum class LaunchTarget {
    library,
    endpoint,
    saved_profile,
    ssh_profile,
};

struct LaunchArguments {
    // The transport arguments, parsed by parse_transport_argument() exactly as
    // before. Only an `endpoint` launch uses them as a whole.
    TransportOptions transport;
    // Which transport arguments were seen, so a target can refuse the ones
    // that mean nothing to it.
    bool endpoint_arguments = false;   // --host, --port, --url
    bool port_argument = false;        // --port specifically
    bool credential_arguments = false; // every other transport argument
    bool cookie_inline = false;        // --cookie (rather than --cookie-file)
    bool broker_arguments = false;     // --token*, --pin-sha256, --ca-file, ...
    std::string cookie_file;           // the path given to --cookie-file

    bool direct = false; // --direct

    std::string profile;       // --profile
    std::string profiles_file; // --profiles

    std::string ssh_host;                // --ssh-host
    std::optional<std::string> ssh_user; // --ssh-user
    std::optional<std::string> identity; // --identity
    std::optional<int> ssh_port;         // --ssh-port
    std::optional<int> remote_port;      // --remote-port
    std::optional<int> local_port;       // --local-port
};

// Consume one argument if it is a launch or transport argument. Returns true
// when consumed. `value` fetches the argument's value and may throw when it is
// missing; a malformed value throws std::runtime_error (a usage error).
bool parse_launch_argument(LaunchArguments& arguments, std::string_view argument,
                           const std::function<std::string()>& value);

// Decide the launch target and check the combination. Throws
// std::runtime_error, naming the conflicting arguments, for a mix that has no
// single meaning. For an `endpoint` launch it also folds --remote-port into
// arguments.transport.port (the two name the same port in direct mode).
LaunchTarget resolve_launch_target(LaunchArguments& arguments);

// The unsaved ssh-mode profile an `ssh_profile` launch connects with. Throws
// std::runtime_error with the offending field when validate() rejects it, so a
// bad --ssh-port is reported the way the editor reports it.
[[nodiscard]] ConnectionProfile ssh_launch_profile(const LaunchArguments& arguments,
                                                   int width, int height);

// The saved profile named `name_or_id` (an exact id match wins over a name
// match), or nullptr.
[[nodiscard]] const ConnectionProfile*
find_profile(const std::vector<ConnectionProfile>& profiles,
             std::string_view name_or_id);

// --help lines for the launch arguments (the transport ones are
// transport_usage()).
[[nodiscard]] std::string_view launch_usage();

} // namespace haiku_remote
