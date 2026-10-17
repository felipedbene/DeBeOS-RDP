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
    // The socket is the local end of an SSH port forward (`ssh -L`), not
    // app_server's own. That changes what a reset means: ssh accepts on the
    // local port before the remote channel exists, and when the channel then
    // fails to open ("open failed: connect failed: Connection refused" -- the
    // listener is not up) it closes a socket whose pipelined bytes it never
    // read, so the kernel sends RST. app_server's own refusal, by contrast,
    // reaches us after ssh has already read and forwarded those bytes, so it
    // arrives as a FIN. Observed live against the mock through a real forward.
    bool through_forward = false;
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

// The reconnect policy driven over time, for a long-lived interactive session
// (the GUI's connected desktop) rather than a one-shot capture. It owns the
// bookkeeping a front end would otherwise re-derive: how many attempts this
// outage has used, when the next one is due, and whether the policy has given
// up. It does no I/O and never sleeps -- the caller asks due() from its own
// event loop, so the window stays responsive while a backoff runs out and the
// user can cancel it.
//
// Lifecycle: connection_ended() after every ending (a drop, or a failed
// attempt); when due() says so, attempt_started() and then build a fresh
// connection; established() once that connection carried a session. That
// refills the budget, so a session that ran for an hour and then dropped gets
// the full set of retries again rather than whatever the last outage left.
class ReconnectSchedule {
public:
    using Clock = std::chrono::steady_clock;

    explicit ReconnectSchedule(ReconnectConfig config) : policy_(config) {}

    [[nodiscard]] const ReconnectPolicy& policy() const { return policy_; }

    // The connection (or an attempt) ended with `outcome` at `now`. Schedules
    // the next attempt and returns true when the policy allows one; otherwise
    // nothing is pending and gave_up() is true.
    bool connection_ended(ConnectionOutcome outcome, Clock::time_point now);

    // The user asked for a reconnect: due immediately, with a fresh budget.
    // Allowed whether or not automatic reconnect is enabled -- an explicit
    // request is not the policy's decision to make.
    void request_now(Clock::time_point now);

    // Drop a pending attempt (the user cancelled, or is disconnecting).
    void cancel() { pending_ = false; }

    [[nodiscard]] bool pending() const { return pending_; }
    [[nodiscard]] bool due(Clock::time_point now) const
    {
        return pending_ && now >= due_at_;
    }
    // When the pending attempt is due. Meaningless unless pending().
    [[nodiscard]] Clock::time_point due_at() const { return due_at_; }

    // The pending attempt is being made now: it counts against the budget.
    void attempt_started();

    // The connection the last attempt built carried a session.
    void established();

    // Attempts used in the current outage (0 while connected).
    [[nodiscard]] int attempts_made() const { return attempts_made_; }
    [[nodiscard]] ConnectionOutcome last_outcome() const { return last_outcome_; }
    // The policy declined to retry the last ending.
    [[nodiscard]] bool gave_up() const { return gave_up_; }
    // It declined because the budget ran out, rather than because the ending
    // was not retriable or reconnect is off.
    [[nodiscard]] bool exhausted() const { return exhausted_; }

private:
    ReconnectPolicy policy_;
    int attempts_made_ = 0;
    bool pending_ = false;
    bool gave_up_ = false;
    bool exhausted_ = false;
    ConnectionOutcome last_outcome_ = ConnectionOutcome::completed;
    Clock::time_point due_at_ {};
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
