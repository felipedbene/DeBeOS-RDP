#pragma once

// A saved remote-desktop connection and its validation, with no UI and no
// filesystem. This lives in haiku_remote_core (which has no SDL on its link
// line) so every front end -- SDL, X11, headless -- shares one definition of a
// profile and one validator; disagreeing on what a valid port or dimension is
// across front ends is exactly the kind of drift this prevents.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

// How the client reaches app_server. These are the three modes DeBeOS-RDP
// issue #1 names (direct | tunnel | wss); `ssh` is the stored name of the
// tunnel mode, kept from the schema's first version so an on-disk library
// written before `wss` existed still loads unchanged.
//
//   ssh    -- the "tunnel" mode: SSH forwards a loopback-bound remote port to a
//             local one. app_server binds 127.0.0.1 only (see PROTOCOL.md), so
//             the tunnel is how an ordinary network client reaches it.
//   direct -- connects straight to host:remotePort, reachable only when that
//             port is already exposed to the client (e.g. an SSM port-forward).
//   wss    -- WebSocket-over-TLS to the DeBeOS remote-desktop broker, which
//             holds the cookie and presents it on the client's behalf.
//
// Part 1 only *stores* the mode. The behaviour behind `ssh` (spawning and
// supervising the tunnel) and `wss` (the broker token/handshake lifecycle) is
// deferred to Part 2; see profile_launch.hpp for the seam.
enum class ConnectionMode {
    ssh,
    direct,
    wss,
};

[[nodiscard]] std::string_view mode_name(ConnectionMode mode);
[[nodiscard]] std::optional<ConnectionMode> mode_from_name(std::string_view name);

inline constexpr int min_port = 1;
inline constexpr int max_port = 65535;

// A saved connection. Schema version 1.
//
// Secrets are deliberately not representable here. The only key reference is
// `identity_file`, which is a PATH, and `cookie_source` names where a session
// cookie is read from (a file path or a source tag such as "broker") -- never
// the cookie's bytes. There is no field for a passphrase, a private key, or a
// cookie value, so a profile can never carry one to disk. ProfileStore relies
// on this: "never persist a secret" is enforced by the type, not by remembering
// to strip fields before each save.
struct ConnectionProfile {
    std::string id;
    std::string name;
    bool favorite = false;
    ConnectionMode mode = ConnectionMode::ssh;
    std::string host;
    int remote_port = 10900;
    std::string ssh_user = "baron";
    int ssh_port = 22;
    // Optional SSH identity file. Stored verbatim, tilde included;
    // expand_user_path() is applied only when ssh is actually launched.
    std::string identity_file;
    // Local forwarded port; null means "pick a free port at connect time".
    std::optional<int> local_port;
    int width = 1280;
    int height = 800;
    bool auto_reconnect = true;
    // Optional per-profile known_hosts file for TOFU host-key pinning. Path
    // only, tilde kept literal.
    std::string known_hosts_file;
    // Where the session cookie is read from (a path or a source tag). Never the
    // cookie value itself.
    std::string cookie_source;
    // Unix time (seconds) of the last successful connection; 0 means never.
    std::int64_t last_connected_at = 0;
    // Human-readable last failure, shown on the library card. Not a secret.
    std::string last_error;

    friend bool operator==(const ConnectionProfile&, const ConnectionProfile&)
        = default;

    // The route the editor summarizes, e.g.
    //   ssh:    client -> 127.0.0.1:<local> -> ssh -> <host>:22 -> 127.0.0.1:10900
    //   direct: client -> <host>:10900
    // An auto local port renders as "auto"; an empty host as "<host>".
    [[nodiscard]] std::string route_summary() const;
};

// One validation failure, naming the offending field so a UI can mark it and a
// log can say exactly what was wrong -- never a silent coercion.
struct FieldError {
    std::string field;
    std::string message;
};

struct ValidationResult {
    std::vector<FieldError> errors;
    std::string route; // == ConnectionProfile::route_summary()

    [[nodiscard]] bool ok() const { return errors.empty(); }
    [[nodiscard]] const FieldError* find(std::string_view field) const;
};

// Validate a profile's schema: ports in [1,65535], width/height in
// [1, Surface::max_dimension], and a host present (required outright in ssh
// mode, and still required in direct mode because there is nothing to connect
// to without one). Pure: it touches neither the filesystem nor the network, and
// it never expands a tilde -- a path is checked for existence only at use.
[[nodiscard]] ValidationResult validate(const ConnectionProfile& profile);

// Expand a leading "~" or "~/" to the user's home directory. Applied only at
// use (launching ssh, opening a cookie or known_hosts file), never when storing
// or validating, so the stored path stays literal and survives a home-dir move.
// Returns the input unchanged when there is no home directory to expand to.
[[nodiscard]] std::string expand_user_path(const std::string& path);

} // namespace haiku_remote
