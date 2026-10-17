#pragma once

// A saved remote-desktop connection and its validation, with no UI and no
// filesystem. This lives in haiku_remote_core (which has no SDL on its link
// line) so every front end -- SDL, X11, headless -- shares one definition of a
// profile and one validator; disagreeing on what a valid port or dimension is
// across front ends is exactly the kind of drift this prevents.

#include "haiku_remote/build_features.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace haiku_remote {

// How the client reaches app_server. Issue #1 designed two -- direct TCP and
// the managed SSH tunnel -- and the broker added a third (wss) and the
// broker-first `auto` route on top of those. The stored names are the
// on-disk schema; see profile_store.hpp for the v1 -> v2 migration that gave
// "direct" back its designed meaning.
//
//   ssh       -- the "tunnel" mode: SSH forwards a loopback-bound remote port
//                to a local one. app_server binds 127.0.0.1 only (see
//                PROTOCOL.md), so the tunnel is how an ordinary network client
//                reaches it. The default for a new profile (issue #1's field
//                table: "Connection mode | SSH tunnel").
//   direct    -- a plain TCP connection to host:remotePort, no SSH at all, with
//                the session cookie read from cookieSource. Reachable only when
//                that port is already exposed to the client: an SSM
//                AWS-StartPortForwardingSession, a forward the user runs, or a
//                client on the guest itself.
//   wss       -- WebSocket-over-TLS to the DeBeOS remote-desktop broker, which
//                holds the cookie and presents it on the client's behalf.
//   automatic -- stored as "auto": the broker first, then the SSH tunnel. What
//                schema v1 called "direct" (profile_launch.cpp planned it that
//                way), so every v1 "direct" profile migrates to it.
//   quic      -- QUIC to the same broker (DeBeOS #766): the same token,
//                certificate and session as wss, over UDP; the broker must run
//                with -q. Auto already prefers it where the broker advertises
//                it; this mode forces it, with no fallback.
//
// Adding a mode (feat/quic-transport adds one) means: an enumerator here, its
// stored name in mode_name()/mode_from_name(), its place in connection_modes
// (which is the editor's chip cycle), a label in mode_label(), and a plan in
// plan_launch(). Nothing in the library screen enumerates modes itself.
enum class ConnectionMode {
    ssh,
    direct,
    wss,
    automatic,
    quic,
};

[[nodiscard]] std::string_view mode_name(ConnectionMode mode);
[[nodiscard]] std::optional<ConnectionMode> mode_from_name(std::string_view name);

// Whether this build can make a wss transport at all (OpenSSL found at build
// time), and a quic one (OpenSSL >= 3.5): build_features.hpp. A route the build
// cannot honour is never planned or offered: without wss `auto` is the tunnel
// alone, without QUIC it skips its QUIC step, the editor's chip skips what the
// build lacks, and validate() refuses a profile that asks for it. Every
// function below that depends on them takes them as defaulted parameters, so
// every build's behaviour is testable from any. QUIC needs TLS, so
// quic_available is taken as false wherever wss_available is.

// Every mode, in the order the editor's mode chip cycles through them. The one
// list of modes; next_mode() and the tests walk it.
inline constexpr ConnectionMode connection_modes[] = {
    ConnectionMode::ssh,
    ConnectionMode::automatic,
    ConnectionMode::direct,
    ConnectionMode::wss,
    ConnectionMode::quic,
};

// Whether `mode` can be used at all in a build with (or without) wss.
[[nodiscard]] bool mode_available(ConnectionMode mode,
                                  bool wss_available = build_supports_wss,
                                  bool quic_available = build_supports_quic);

// The next available mode after `mode` in connection_modes, wrapping around.
[[nodiscard]] ConnectionMode next_mode(ConnectionMode mode,
                                       bool wss_available = build_supports_wss,
                                       bool quic_available = build_supports_quic);

// The user-facing name of a mode: the editor chip and the library card.
[[nodiscard]] std::string mode_label(ConnectionMode mode);

// True for the modes that run ssh at all (a tunnel, or a credential fetch), and
// so for which the SSH fields of a profile mean something.
[[nodiscard]] bool mode_uses_ssh(ConnectionMode mode);

inline constexpr int min_port = 1;
inline constexpr int max_port = 65535;

// The broker's well-known port. app_server listens on remote_port (10900 by
// default); the broker proxies wss (TCP) and QUIC (UDP) on this port in front
// of it. A profile's broker_port defaults to it.
inline constexpr std::uint16_t default_broker_port = 10902;

// JSON members a newer client wrote that this one does not understand, kept so
// a load/save round trip does not destroy them (issue #1, Persistence: "preserve
// unknown fields to allow forward-compatible migrations"). Each entry is the
// member name and its value as compact JSON text, in file order. Stored as text
// rather than a json::Value so this header stays free of the JSON seam.
using ExtraFields = std::vector<std::pair<std::string, std::string>>;

// A saved connection. Schema version 2 (profile_store.hpp).
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
    // The broker's port on `host` (TCP for wss, UDP for QUIC), for a broker
    // that is not on the well-known one. Stored as "brokerPort" only when it
    // differs from default_broker_port, so every file written before it
    // existed reads, and writes back, unchanged. Not in the editor: it is a
    // deployment detail, set in the file (and how the e2e harness points the
    // client at a bridge on an ephemeral port).
    int broker_port = default_broker_port;
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
    // Unknown members of this profile's JSON object, preserved verbatim across
    // load/save. Never exported (export cannot vouch they hold no secret).
    ExtraFields extra_fields;

    friend bool operator==(const ConnectionProfile&, const ConnectionProfile&)
        = default;

    // The route the editor summarizes, e.g.
    //   ssh:    client -> 127.0.0.1:<local> -> ssh -> <host>:22 -> 127.0.0.1:10900
    //   direct: client -> <host>:10900
    //   wss:    client -> wss -> broker <host>:10902
    //   auto:   the wss route, then the ssh one, joined by " | else " -- what
    //           plan_launch() does for auto (broker first, tunnel fallback);
    //           just the ssh one in a build without wss
    // An auto local port renders as "auto"; an empty host as "<host>".
    [[nodiscard]] std::string
    route_summary(bool wss_available = build_supports_wss,
                  bool quic_available = build_supports_quic) const;
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

// Validate a profile's schema: ports in [1,65535] (sshPort only for a mode that
// runs ssh), width/height in [1, Surface::max_dimension], and a host present
// (required outright in ssh mode, and in every other mode because there is
// nothing to connect to without one). Pure: it touches neither the filesystem nor the network, and
// it never expands a tilde -- a path is checked for existence only at use.
//
// A `wss` profile in a build without wss is an error on the "mode" field.
[[nodiscard]] ValidationResult validate(const ConnectionProfile& profile,
                                        bool wss_available = build_supports_wss,
                                        bool quic_available = build_supports_quic);

// Parse a number exactly as a user typed it into a numeric field: one or more
// ASCII digits and nothing else -- no sign, no spaces, no "10900abc". Anything
// else yields nullopt and a message saying what was wrong ("is required",
// "must be a whole number", "is too large"), so a UI can show a field error
// instead of silently coercing the text to 0 or dropping characters. Range
// checks (ports, dimensions) stay in validate(); this only answers "is this a
// number at all".
[[nodiscard]] std::optional<int> parse_whole_number(std::string_view text,
                                                    std::string& error);

// Expand a leading "~" or "~/" to the user's home directory. Applied only at
// use (launching ssh, opening a cookie or known_hosts file), never when storing
// or validating, so the stored path stays literal and survives a home-dir move.
// Returns the input unchanged when there is no home directory to expand to.
[[nodiscard]] std::string expand_user_path(const std::string& path);

} // namespace haiku_remote
