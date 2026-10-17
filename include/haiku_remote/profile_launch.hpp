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
//   direct  -- TcpTransport straight to host:remotePort with the cookie from
//              cookie_source; no SSH. For a port that is already exposed to
//              the client (an SSM port-forward, a forward the user runs, a
//              client on the guest).
//
// The broker is reachable two ways, both RouteKind::broker with the same token
// and certificate: quic://host:10902 (UDP, DeBeOS #766, when the broker runs
// with -q and says so in broker.quic) and wss://host:10902 (TCP).
//
// How a profile's mode maps onto those routes:
//
//   wss    -> [broker wss]          forces wss; no fallback.
//   quic   -> [broker quic]         forces QUIC; no fallback.
//   ssh    -> [tunnel]              forces the SSH tunnel; no fallback.
//   direct -> [direct]              one plain TCP connection; no fallback.
//   auto   -> [broker quic, broker wss, tunnel]
//                                   QUIC when the broker offers it (broker.quic,
//                                   read with the credentials), wss when it does
//                                   not or UDP is blocked (after
//                                   quic_give_up_ms), the tunnel when the broker
//                                   is down. (Schema v1 stored this as
//                                   "direct"; see profile_store.hpp.)
//             [broker wss, tunnel]  ...in a build without QUIC (OpenSSL < 3.5).
//             [tunnel]              ...in a build without wss.
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

// How long auto mode's QUIC handshake may take before it falls back to wss.
inline constexpr int quic_give_up_ms = 3000;

// Which concrete route a step stands up.
enum class RouteKind {
    broker, // QuicTransport (quic) or WebSocketTransport (wss) to the broker.
    tunnel, // TcpTransport to the local end of an SSH -L forward.
    direct, // TcpTransport to host:remotePort, no SSH.
};

[[nodiscard]] std::string_view route_kind_name(RouteKind kind);

// One candidate route. `transport` is a *template*: everything the pure planner
// can know is filled in, and open_connection() completes the parts that need a
// side effect -- the broker token and certificate seed after fetching them, or the
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
//
// `wss_available` is whether a broker step could be honoured (the build has a
// wss transport). Without it, auto plans the tunnel alone: a broker step that
// can only fail with "unsupported scheme" is not a route. An explicit `wss`
// profile still plans its single broker step -- it asked for nothing else, and
// the transport then says plainly that this build cannot do it (validate()
// refuses such a profile before it gets that far).
//
// `quic_available` likewise for a QUIC broker step (OpenSSL >= 3.5): without
// it auto plans no QUIC step, and an explicit `quic` profile still plans its
// one step for the same reason as `wss`.
[[nodiscard]] LaunchPlan plan_launch(const ConnectionProfile& profile,
                                     bool wss_available = build_supports_wss,
                                     bool quic_available = build_supports_quic);

} // namespace haiku_remote
