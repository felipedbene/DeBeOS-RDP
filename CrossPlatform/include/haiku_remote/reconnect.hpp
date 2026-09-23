#pragma once

#include "haiku_remote/transport.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace haiku_remote {

// How a connection ended, reduced to the single question the reconnect policy
// has to answer: may the client come back?
//
// The distinction that matters is NOT "did the socket close" -- a refused
// candidate, an evicted client, an orderly server shutdown and a torn-down
// tunnel all end with a closed socket. It is *why* it closed, and that is read
// from more than the socket: from whether RP_CLOSE_CONNECTION arrived in band,
// from whether the peer reset the connection (RST) or closed it cleanly (FIN),
// and from whether a session ever got going at all.
enum class ConnectionOutcome {
    // The server sent RP_CLOSE_CONNECTION. An orderly, server-initiated
    // teardown -- "go away" -- and never a transport blip. Do not retry.
    server_closed,

    // The peer reset the connection (RST) with our pipelined bytes unread.
    // That is the signature app_server leaves for a refused or evicted
    // candidate: the candidate gate is newest-wins, so an auto-reconnecting
    // evicted client would evict whoever displaced it and be evicted back --
    // the exact loop the policy exists to prevent. Do not retry.
    evicted,

    // The connection was accepted and closed cleanly (FIN) but the session
    // produced nothing: the candidate gate dropped us, or the session never
    // started. Retrying repeats the same rejection. Do not retry.
    refused,

    // connect() failed for lack of a credential (no cookie / no token). A
    // misinvocation fixed on this side, not out on the wire, and deterministic:
    // retrying with the same missing credential fails identically. Do not retry.
    missing_credential,

    // The connection dropped mid-session in a way that is not the server
    // rejecting us: a clean FIN with no RP_CLOSE_CONNECTION (a tunnel torn
    // down), or a non-reset socket/network error. This is the retriable case.
    transport_dropped,

    // connect() failed for a reason other than a missing credential (the
    // listener or the tunnel is not up yet). Retriable, so a reconnect can wait
    // for the far end to come back.
    connect_failed,

    // The session ran and ended as expected (the capture deadline elapsed, or
    // any other clean, intended stop). Not a drop; nothing to retry.
    completed,
};

[[nodiscard]] std::string_view outcome_name(ConnectionOutcome outcome);

// The observations a finished (or failed) connection leaves behind, gathered
// from the transport and the session. Kept as a plain struct so the
// classification is a pure function of them and can be unit-tested with no
// socket in the loop.
struct ConnectionResult {
    // Did connect() succeed at all?
    bool connected = false;
    // Only meaningful when connected == false.
    ConnectFailure connect_failure = ConnectFailure::none;
    // How many protocol messages the session decoded. Zero means no session
    // ever got going (a gate refusal, or a hang-up during the handshake).
    std::size_t message_count = 0;
    // The server sent RP_CLOSE_CONNECTION in band.
    bool server_closed = false;
    // The receive that ended the connection: a clean FIN (peer_closed) versus a
    // reset (connection_reset). Both may be false when the connection ended for
    // another reason (the capture deadline, a local error that is neither).
    bool peer_closed = false;
    bool connection_reset = false;
    // The connection ended because a normal, intended stopping point was
    // reached (the deadline) rather than because the stream dropped.
    bool reached_deadline = false;
};

// Reduce a ConnectionResult to the one question that decides retry. The order of
// the checks is the policy: an in-band RP_CLOSE_CONNECTION and a reset both win
// over "we had a session", because either is the server refusing us and neither
// is a transport blip.
[[nodiscard]] ConnectionOutcome classify_connection(const ConnectionResult& result);

// Reconnect configuration. Off by default: reconnect is opt-in, so a build or a
// front end that does not set enabled == true behaves exactly as before -- one
// connection, and the process ends when it does.
struct ReconnectConfig {
    bool enabled = false;
    // Bounded: at most this many reconnect attempts after the first connection.
    // Zero disables retry even when enabled is true.
    int max_attempts = 5;
    // Exponential backoff, doubled each attempt and capped. The first retry
    // waits base_backoff, the next 2x, then 4x, ... never past max_backoff.
    std::chrono::milliseconds base_backoff {200};
    std::chrono::milliseconds max_backoff {5000};
};

// The retry decision, separated from any I/O so the whole policy -- bounded
// count, backoff schedule, and which outcomes are retriable -- is exercised by
// unit tests directly.
class ReconnectPolicy {
public:
    explicit ReconnectPolicy(ReconnectConfig config) : config_(config) {}

    [[nodiscard]] const ReconnectConfig& config() const { return config_; }

    // May the client reconnect after `outcome`, given that `attempts_made`
    // reconnects have already happened? False when reconnect is disabled, when
    // the bound is reached, or when the outcome is not a retriable transport
    // failure.
    [[nodiscard]] bool should_retry(ConnectionOutcome outcome,
                                    int attempts_made) const;

    // The delay before the reconnect numbered `attempts_made` (0 = the first
    // reconnect). Exponential from base_backoff, capped at max_backoff.
    [[nodiscard]] std::chrono::milliseconds backoff_for(int attempts_made) const;

private:
    ReconnectConfig config_;
};

// Whether an outcome is a retriable transport failure at all, independent of
// the count/enabled gates. Exposed so a caller can log "gave up after N" versus
// "did not retry: <reason>" precisely.
[[nodiscard]] bool outcome_is_retriable(ConnectionOutcome outcome);

// Shared command-line handling for the reconnect options, so every front end
// accepts the same flags. Returns true when the argument was consumed. `value`
// fetches the argument's value and may throw when it is missing or invalid.
//   --reconnect                    turn reconnect on (opt-in; off otherwise)
//   --reconnect-max-attempts N     bound the retries (default 5)
//   --reconnect-backoff-ms MS      base backoff, doubled each attempt (default 200)
bool parse_reconnect_argument(ReconnectConfig& config, std::string_view argument,
                              const std::function<std::string()>& value);

// One usage block for the reconnect options, for --help output.
[[nodiscard]] std::string_view reconnect_usage();

} // namespace haiku_remote
