#include "haiku_remote/reconnect.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string>

namespace haiku_remote {

std::string_view outcome_name(ConnectionOutcome outcome)
{
    switch (outcome) {
    case ConnectionOutcome::server_closed:      return "server closed the connection";
    case ConnectionOutcome::evicted:            return "refused or evicted (connection reset)";
    case ConnectionOutcome::refused:            return "session refused or never started";
    case ConnectionOutcome::missing_credential: return "no credential supplied";
    case ConnectionOutcome::transport_dropped:  return "transport dropped";
    case ConnectionOutcome::connect_failed:     return "could not connect";
    case ConnectionOutcome::completed:          return "session completed";
    }
    return "unknown";
}

ConnectionOutcome classify_connection(const ConnectionResult& result)
{
    if (!result.connected) {
        return result.connect_failure == ConnectFailure::missing_credential
            ? ConnectionOutcome::missing_credential
            : ConnectionOutcome::connect_failed;
    }

    // Server saying "go away", in band, wins over everything else: it is an
    // orderly, deliberate teardown, so even if a reset chased the FIN it is not
    // a transport blip.
    if (result.server_closed)
        return ConnectionOutcome::server_closed;

    // A peer reset with our bytes unread is app_server's refused/evicted
    // signature. Checked before "did we have a session", on purpose: an evicted
    // client HAD a full session and still must not come back, because the gate
    // is newest-wins and it would just evict its successor in a loop.
    if (result.connection_reset)
        return ConnectionOutcome::evicted;

    // A clean, intended stop (the capture deadline). Not a drop.
    if (result.reached_deadline)
        return ConnectionOutcome::completed;

    // Connected and closed cleanly but nothing was ever decoded: the candidate
    // gate dropped us, or the session never started. Retrying repeats it.
    if (result.message_count == 0)
        return ConnectionOutcome::refused;

    // Anything left is a mid-session drop that is not the server rejecting us:
    // a clean FIN with no RP_CLOSE_CONNECTION (a tunnel torn down), or a
    // non-reset socket error. This is the one retriable case.
    return ConnectionOutcome::transport_dropped;
}

bool outcome_is_retriable(ConnectionOutcome outcome)
{
    return outcome == ConnectionOutcome::transport_dropped
        || outcome == ConnectionOutcome::connect_failed;
}

bool ReconnectPolicy::should_retry(ConnectionOutcome outcome,
                                   int attempts_made) const
{
    if (!config_.enabled)
        return false;
    if (config_.max_attempts <= 0)
        return false;
    if (attempts_made >= config_.max_attempts)
        return false;
    return outcome_is_retriable(outcome);
}

std::chrono::milliseconds ReconnectPolicy::backoff_for(int attempts_made) const
{
    if (attempts_made < 0)
        attempts_made = 0;
    // Double base_backoff `attempts_made` times, saturating at max_backoff.
    // Done in the loop rather than with a shift so a large attempt count cannot
    // overflow the shift, and so the cap is applied at every step.
    auto delay = config_.base_backoff;
    for (int i = 0; i < attempts_made; ++i) {
        if (delay >= config_.max_backoff)
            return config_.max_backoff;
        delay *= 2;
    }
    return std::min(delay, config_.max_backoff);
}

namespace {

int parse_positive(std::string_view text, std::string_view name, int minimum)
{
    int value = 0;
    const auto [pointer, status]
        = std::from_chars(text.data(), text.data() + text.size(), value);
    if (status != std::errc() || pointer != text.data() + text.size()
        || value < minimum) {
        throw std::runtime_error("invalid " + std::string(name) + ": "
                                 + std::string(text));
    }
    return value;
}

} // namespace

bool parse_reconnect_argument(ReconnectConfig& config, std::string_view argument,
                              const std::function<std::string()>& value)
{
    if (argument == "--reconnect") {
        config.enabled = true;
    } else if (argument == "--reconnect-max-attempts") {
        config.max_attempts = parse_positive(value(), "--reconnect-max-attempts", 0);
    } else if (argument == "--reconnect-backoff-ms") {
        config.base_backoff = std::chrono::milliseconds(
            parse_positive(value(), "--reconnect-backoff-ms", 1));
    } else {
        return false;
    }
    return true;
}

std::string_view reconnect_usage()
{
    return "  [--reconnect]                    reconnect on a transport drop"
           " (opt-in; off by default)\n"
           "  [--reconnect-max-attempts N]     bound the retries (default 5)\n"
           "  [--reconnect-backoff-ms MS]      base backoff, doubled each"
           " attempt (default 200)";
}

} // namespace haiku_remote
