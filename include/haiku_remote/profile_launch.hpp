#pragma once

// The bridge from a saved profile to the TransportOptions a session connects
// with. This header is the *pure* half of that bridge: given a profile it
// produces an ordered list of candidate routes (RouteStep) and the
// TransportOptions each would use, touching neither the network nor any child
// process. The side-effecting half -- spawning and supervising an SSH tunnel,
// or fetching the broker's token and certificate -- lives in
// managed_transport.hpp (open_connection()), which consumes this plan.
//
// Route model (congruent with tools/haiku-remote-connect.sh and PR #67):
//
//   broker  -- WebSocket-over-TLS to the DeBeOS remote-desktop broker, which
//              binds all interfaces (reachable straight over a VPN) and holds
//              app_server's session cookie, presenting it on the client's
//              behalf. url = wss://host:<broker port>, authenticated with a
//              session token and the broker's self-signed certificate.
//   tunnel  -- app_server's own session port is loopback-only by design (see
//              PROTOCOL.md), so an ordinary network client reaches it only
//              through an SSH `-L` forward: TcpTransport to 127.0.0.1:<local>
//              with the session cookie as the mandatory first frame.
//
// How a profile's mode maps onto those routes:
//
//   wss    -> [broker]            forces the broker; no fallback.
//   ssh    -> [tunnel]            forces the SSH tunnel; no fallback.
//   direct -> [broker, tunnel]    the "auto" default: try the broker first and
//                                 fall back to the SSH tunnel. This is the
//                                 recommended everyday mode -- the broker when
//                                 it is up, the tunnel when it is not.
//
// Representing a mode as a *list* is what lets open_connection() implement the
// broker-first/tunnel-fallback behaviour without the renderer or the transport
// core knowing anything about routing: it walks the steps in order and uses the
// first whose substrate it can stand up.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/transport.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace haiku_remote {

// The broker's well-known WebSocket port. app_server listens on remote_port
// (10900 by default); the broker proxies wss on this port in front of it. The
// profile does not (yet) carry a per-connection broker port, so this constant
// is the one place to change if that is ever added.
inline constexpr std::uint16_t default_broker_port = 10902;

// Which of the two concrete routes a step stands up.
enum class RouteKind {
    broker, // WebSocketTransport (wss) to the broker.
    tunnel, // TcpTransport to the local end of an SSH -L forward.
};

[[nodiscard]] std::string_view route_kind_name(RouteKind kind);

// One candidate route. `transport` is a *template*: everything the pure planner
// can know is filled in, and open_connection() completes the parts that need a
// side effect -- the broker token and ca_file after fetching them, or the
// tunnel's local port after the forward is up.
struct RouteStep {
    RouteKind kind = RouteKind::broker;
    TransportOptions transport;
    std::string note;
};

struct LaunchPlan {
    // The routes to try, in order. Never empty.
    std::vector<RouteStep> steps;
    // Convenience alias for steps.front().transport, kept so existing callers
    // and tests that read a single plan.transport still compile. The managed
    // path uses `steps`.
    TransportOptions transport;
    // Human-readable description of the plan.
    std::string note;
    // Retained from Part 1 for source compatibility; always false now that the
    // ssh and wss modes are fully wired. A reader can treat it as "this plan is
    // a best-effort fallback", which no longer happens.
    bool is_stub = false;
};

// Build the ordered launch plan for a profile. Pure except for a best-effort
// read of cookie_source when it names an existing regular file (a tunnel needs
// app_server's session cookie as its first frame, and reading a local file is
// the one case the planner can satisfy without SSH); the cookie is read at plan
// time only when it is a plain file and is never stored. Never throws, never
// spawns a process, never opens a socket.
[[nodiscard]] LaunchPlan plan_launch(const ConnectionProfile& profile);

} // namespace haiku_remote
