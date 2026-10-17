#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/launch_arguments.hpp"
#include "haiku_remote/png_writer.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/profile_store.hpp"
#include "haiku_remote/reconnect.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/transport.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace haiku_remote;

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    // What to connect to: the transport arguments (--host/--port/--url/...),
    // --direct, the --ssh-* tunnel arguments, or --profile NAME [--profiles
    // FILE], which launches a saved connection the way the GUI's library does
    // (issue #1: "continue supporting command-line launch"). Parsed by
    // launch_arguments.hpp, shared with haiku-remote-gui.
    LaunchArguments launch;
    LaunchTarget target = LaunchTarget::library;
    ReconnectConfig reconnect;
    int width = 1280;
    int height = 800;
    int seconds = 2;
    std::string output = "haiku-remote.png";
    bool draw_cursor = false;
    bool size_explicit = false;
};

int parse_integer(std::string_view value, std::string_view name,
                  int minimum, int maximum)
{
    std::size_t parsed = 0;
    const long long number = std::stoll(std::string(value), &parsed);
    if (parsed != value.size() || number < minimum || number > maximum)
        throw std::runtime_error("invalid " + std::string(name) + ": "
                                 + std::string(value));
    return static_cast<int>(number);
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc)
                throw std::runtime_error("missing value for " + argument);
            return argv[i];
        };
        if (parse_launch_argument(options.launch, argument, value)) {}
        else if (parse_reconnect_argument(options.reconnect, argument, value)) {}
        else if (argument == "--width") {
            options.width = parse_integer(
                value(), "width", 1, Surface::max_dimension);
            options.size_explicit = true;
        } else if (argument == "--height") {
            options.height = parse_integer(
                value(), "height", 1, Surface::max_dimension);
            options.size_explicit = true;
        }
        else if (argument == "--seconds")
            options.seconds = parse_integer(
                value(), "seconds", 0, std::numeric_limits<int>::max());
        else if (argument == "--output") options.output = value();
        else if (argument == "--draw-cursor") options.draw_cursor = true;
        else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: haiku-remote" << transport_usage()
                      << "\n  [--width PX] [--height PX] [--seconds N]"
                         " [--output FILE.png] [--draw-cursor]\n"
                      << launch_usage() << '\n'
                      << reconnect_usage()
                      << "\n      (a profile launch also reconnects when the"
                         " profile has Auto reconnect on)\n\n"
                      << exit_status_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    options.target = resolve_launch_target(options.launch);
    return options;
}

// One attempt's connected link: the transport plus whatever has to stay alive
// under it (ConnectResult owns a managed SSH tunnel). Dropping the link closes
// the transport and reaps the tunnel's ssh child, so an attempt's route is
// always torn down before the next attempt builds another (issue #1: "On
// reconnect, tear down the previous forward before starting another").
struct Link {
    ConnectResult managed; // managed.transport is connected when managed.ok.
    // Why there is no transport, when there is none.
    ConnectFailure failure = ConnectFailure::other;
    // A local misinvocation (a malformed URL): fail the run, never retry.
    bool fatal = false;
    // The cause will not change on a second try (ConnectError::can_retry
    // false): end the run with `status` instead of retrying.
    bool final = false;
    int status = exit_status::failed;
    // connect_with_progress already ran the RP handshake on the session (a
    // managed link); an endpoint link still needs Session::start().
    bool handshake_done = false;
};

// Connects one attempt. The session is the run's, kept across reconnects; a
// managed connect drives its handshake on it.
using Connector = std::function<Link(Session&)>;

// The raw endpoint path, unchanged: make_transport() + connect_with_broker_trust().
Link connect_endpoint(const TransportOptions& options)
{
    Link link;
    std::string error;
    link.managed.transport = make_transport(options, error);
    if (link.managed.transport == nullptr) {
        // A malformed URL or unsupported scheme is a misinvocation, not a
        // transport drop: fail the whole run rather than retry it.
        std::cerr << error << '\n';
        link.fatal = true;
        return link;
    }
    Transport& transport = *link.managed.transport;
    if (!connect_with_broker_trust(transport, terminal_trust_policy(options),
                                   error)) {
        std::cerr << "connect to " << transport.describe() << " failed: "
                  << error << '\n';
        link.failure = transport.connect_failure();
        link.managed.transport.reset();
        return link;
    }
    link.managed.ok = true;
    return link;
}

// The managed path a saved or --ssh-host profile takes, the one the GUI's
// library uses: plan_launch, then connect_with_progress (broker first, SSH
// tunnel fallback, credentials fetched over SSH, the tunnel owned and reaped).
// Stage changes go to stderr so a log shows where an attempt is. Run afresh
// for every attempt, so a reconnect rebuilds the tunnel.
Link connect_profile(const ConnectionProfile& profile, Session& session,
                     int width, int height)
{
    Link link;
    ConnectFlow flow(plan_launch(profile));
    std::optional<std::size_t> shown;
    const auto on_progress = [&] {
        const auto active = flow.active_stage();
        if (active.has_value() && active != shown) {
            shown = active;
            std::cerr << "connect: " << flow.stages()[*active].label << '\n';
        }
    };
    // connect_with_progress runs the RP handshake too, on the run's session:
    // "connected" means app_server acknowledged RP_INIT_CONNECTION.
    ConnectOptions connect_options;
    connect_options.width = width;
    connect_options.height = height;
    connect_options.session = &session;
    link.managed = connect_with_progress(profile, flow, on_progress, connect_options);
    link.handshake_done = true;
    if (link.managed.ok && link.managed.transport != nullptr) {
        if (!link.managed.route_note.empty())
            std::cerr << "route: " << link.managed.route_note << '\n';
        return link;
    }
    const ConnectError& e = link.managed.error;
    std::cerr << "connect failed: " << e.summary() << '\n';
    link.managed.ok = false;
    link.managed.transport.reset();
    // The same exit-code table as an endpoint launch (exit_status_usage()).
    switch (e.cause) {
    case ConnectErrorCause::missing_credential:
        link.failure = ConnectFailure::missing_credential;
        break;
    case ConnectErrorCause::broker_cert_unknown:
        link.failure = ConnectFailure::broker_unknown;
        break;
    case ConnectErrorCause::broker_cert_changed:
        link.failure = ConnectFailure::broker_changed;
        break;
    default:
        link.failure = ConnectFailure::other;
        link.final = !e.can_retry;
        break;
    }
    return link;
}

// The outcome of one connect-and-receive attempt, gathered for the reconnect
// policy. `fatal_error` carries a genuine local failure (a malformed URL) that
// ends the run regardless of the policy; `final_failure` a connect whose cause
// a retry cannot change.
struct AttemptResult {
    ConnectionResult connection;
    bool fatal_error = false;
    bool final_failure = false;
    int final_status = exit_status::failed;
};

// Drive one connection through `session`: connect a fresh link, run the
// receive loop until the deadline or until the stream ends, and report how it
// ended. `session` persists across attempts so the surface and the session
// identity survive a reconnect; the caller calls session.reset() between
// attempts. An unset `deadline` starts at this attempt's successful connect --
// a profile launch, whose SSH set-up would otherwise eat the capture window.
AttemptResult run_one_connection(const Options& options, const Connector& connect,
                                 Session& session,
                                 std::optional<Clock::time_point>& deadline)
{
    AttemptResult attempt;
    std::string error;

    Link link = connect(session);
    if (link.fatal) {
        attempt.fatal_error = true;
        return attempt;
    }
    if (!link.managed.ok) {
        attempt.connection.connected = false;
        attempt.connection.connect_failure = link.failure;
        attempt.final_failure = link.final;
        attempt.final_status = link.status;
        return attempt;
    }
    attempt.connection.connected = true;
    if (!deadline)
        deadline = Clock::now() + std::chrono::seconds(options.seconds);
    const auto& transport = link.managed.transport;

    // Point the session's sender at this attempt's transport. On a reconnect
    // start() re-presents RP_INIT_CONNECTION + RP_HELLO (with RP_CAP_RESYNC), so
    // the server re-negotiates and replays state onto the reset session.
    session.set_sender([&](std::span<const std::uint8_t> bytes) {
        return transport->send_all(bytes, error);
    });

    error.clear();
    if (!link.handshake_done)
        session.start();
    if (!error.empty()) {
        std::cerr << "handshake send to " << transport->describe()
                  << " failed: " << error << '\n';
        // A handshake that could not be written is a drop during the handshake,
        // not a local misuse: let the policy decide whether to try again.
        attempt.connection.connection_reset = transport->connection_reset();
        attempt.connection.peer_closed = transport->peer_closed();
        return attempt;
    }

    std::array<std::uint8_t, 256 * 1024> buffer {};
    while (Clock::now() < *deadline) {
        const int count = transport->receive(buffer, 100, error);
        if (count < 0) {
            attempt.connection.peer_closed = transport->peer_closed();
            attempt.connection.connection_reset = transport->connection_reset();
            if (!transport->peer_closed() && !transport->connection_reset())
                std::cerr << "receive failed: " << error << '\n';
            return attempt;
        }
        if (count > 0)
            session.ingest(
                std::span(buffer.data(), static_cast<std::size_t>(count)));
        if (session.server_closed()) {
            attempt.connection.server_closed = true;
            return attempt;
        }
    }
    attempt.connection.reached_deadline = true;
    return attempt;
}

// Connect, capture until the deadline, reconnect per `config`, write the PNG:
// the one capture loop every launch target runs. `via_profile` is set for a
// profile launch and only adds to the final report line.
int run_capture(const Options& options, const Connector& connect,
                const ReconnectConfig& config, int width, int height,
                std::optional<Clock::time_point> deadline,
                const std::string& via_profile)
{
    const ReconnectPolicy policy(config);

    // The session, and the surface it paints into, live across reconnects.
    // The send callback is rebound to each attempt's transport inside
    // run_one_connection().
    Session session(
        width, height,
        [](std::span<const std::uint8_t>) { return true; },
        [](std::string_view line) { std::cerr << line << '\n'; });

    std::size_t total_messages = 0;
    int attempts_made = 0;
    ConnectionOutcome outcome = ConnectionOutcome::completed;
    bool connected_at_all = false;
    bool orderly_end = false;

    for (;;) {
        if (attempts_made > 0) {
            // Between attempts, throw away every scrap of the previous
            // session's client state so the reconnect's replay is not
            // composited on top of it -- the client half of the reconnect
            // black screen.
            session.reset();
        }
        auto attempt = run_one_connection(options, connect, session, deadline);
        // The message count is per-connection (reset() zeroes it between
        // attempts); fold it into the running total and into the result the
        // classifier reads, so a drop after a real session is a transport
        // drop, not mistaken for a gate refusal.
        attempt.connection.message_count = session.message_count();
        total_messages += session.message_count();
        connected_at_all = connected_at_all || attempt.connection.connected;

        if (attempt.fatal_error)
            return exit_status::usage;

        outcome = classify_connection(attempt.connection);
        orderly_end = outcome == ConnectionOutcome::server_closed
            || outcome == ConnectionOutcome::completed
            || attempt.connection.peer_closed;

        if (attempt.connection.connect_failure
            == ConnectFailure::missing_credential
            && attempts_made == 0) {
            // No credential was ever supplied: this side's fix, its own code.
            return exit_status::no_credential;
        }
        // An untrusted broker certificate is a decision, not a blip: never
        // retried, so a refusal cannot turn into a prompt loop or be
        // waited out.
        if (attempt.connection.connect_failure == ConnectFailure::broker_unknown
            || attempt.connection.connect_failure
                == ConnectFailure::broker_changed)
            return exit_status::untrusted_broker;
        // A managed connect whose cause a retry cannot change (a pinned
        // certificate mismatch, say): retrying would only repeat it.
        if (attempt.final_failure)
            return attempt.final_status;

        if (!policy.should_retry(outcome, attempts_made))
            break;

        auto delay = policy.backoff_for(attempts_made);
        const auto now = Clock::now();
        if (deadline && now + delay >= *deadline)
            break; // No time left in the capture window to try again.
        std::cerr << "connection ended (" << outcome_name(outcome)
                  << "); reconnecting in " << delay.count() << " ms"
                  << " (attempt " << (attempts_made + 1) << " of "
                  << config.max_attempts << ")\n";
        std::this_thread::sleep_for(delay);
        ++attempts_made;
    }

    // connect() never succeeded, and it was not a missing credential:
    // report the transport failure.
    if (!connected_at_all)
        return exit_status::failed;

    // Compositing happens on a *copy*, never on session.surface(): the
    // framebuffer is what the next frame's drawing and any RP_READ_BITMAP
    // readback are computed against, so burning a cursor into it would make
    // the cursor's own pixels part of the server's picture of the screen.
    std::string error;
    std::optional<Surface> composited;
    if (options.draw_cursor) {
        composited = session.surface();
        composite_cursor(session.cursor(), *composited);
    }
    if (!write_png(composited.has_value() ? *composited : session.surface(),
                   options.output, error)) {
        std::cerr << "capture failed: " << error << '\n';
        return exit_status::failed;
    }

    // A hang-up before any message ever arrived is not a short session, it
    // is a session that never started: something upstream refused it, and
    // the only evidence is that nothing was ever decoded across every
    // attempt.
    if (orderly_end && total_messages == 0) {
        std::cerr << "wrote " << options.output
                  << " but the session produced nothing: the server closed"
                     " the connection before sending any drawing data, so"
                     " the session was refused or never started\n";
        return exit_status::failed;
    }

    // A connection that ended by a means other than an orderly close or the
    // capture deadline -- a reset (an eviction/refusal), or a transport
    // error we could not or would not recover from -- is a failure even
    // though the pixels we did decode are real and were written. Same exit
    // code as the single-shot client always returned for a receive error;
    // reconnect only adds the note about giving up.
    if (!orderly_end) {
        std::cerr << "wrote " << options.output << " after " << total_messages
                  << " messages, but the connection ended abnormally ("
                  << outcome_name(outcome) << ")";
        if (attempts_made > 0)
            std::cerr << " -- reconnect gave up after " << attempts_made
                      << (attempts_made == 1 ? " attempt" : " attempts");
        std::cerr << '\n';
        return exit_status::failed;
    }

    std::cout << "wrote " << options.output << " after "
              << total_messages << " messages";
    if (attempts_made > 0)
        std::cout << " over " << (attempts_made + 1) << " connections";
    if (!session.unhandled().empty())
        std::cout << " (" << session.unhandled().size() << " unhandled opcodes)";
    std::cout << (session.server_closed()
                      ? " (server closed the connection)"
                      : " (server hung up)");
    if (!via_profile.empty())
        std::cout << " via profile \"" << via_profile << '"';
    std::cout << '\n';
    return exit_status::ok;
}

// --profile / --ssh-host: resolve the profile, then the shared capture loop over
// the managed connect. A saved profile's own "Auto reconnect" setting is
// honoured (issue #1's profile editor: "Auto reconnect -- Rebuild tunnel before
// reconnecting"); --reconnect turns it on for a profile that has it off, and
// the --reconnect-* bounds apply either way. Each retry runs the whole managed
// connect again, so the tunnel is rebuilt rather than reused.
int run_profile(const Options& options)
{
    ConnectionProfile profile;
    if (options.target == LaunchTarget::ssh_profile) {
        profile = ssh_launch_profile(options.launch, options.width, options.height);
    } else {
        const ProfileStore store = options.launch.profiles_file.empty()
            ? ProfileStore()
            : ProfileStore(options.launch.profiles_file);
        const LoadResult loaded = store.load();
        if (!loaded.ok()) {
            std::cerr << "cannot read " << store.file().string() << ": "
                      << loaded.message << '\n';
            return exit_status::usage;
        }
        const ConnectionProfile* found
            = find_profile(loaded.profiles, options.launch.profile);
        if (found == nullptr) {
            std::cerr << "no profile named \"" << options.launch.profile
                      << "\" in " << store.file().string() << '\n';
            return exit_status::usage;
        }
        profile = *found;
    }

    ReconnectConfig config = options.reconnect;
    config.enabled = config.enabled || profile.auto_reconnect;
    const int width = options.size_explicit ? options.width : profile.width;
    const int height = options.size_explicit ? options.height : profile.height;
    return run_capture(
        options,
        [&](Session& session) {
            return connect_profile(profile, session, width, height);
        },
        config, width, height,
        std::nullopt, profile.name.empty() ? profile.host : profile.name);
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parse_options(argc, argv);
        if (options.target == LaunchTarget::saved_profile
            || options.target == LaunchTarget::ssh_profile)
            return run_profile(options);

        // An endpoint launch -- or no target at all, which for this headless
        // client (it has no library to open) is the default endpoint, as it
        // always was. The capture window starts before the first connect.
        return run_capture(
            options,
            [&](Session&) { return connect_endpoint(options.launch.transport); },
            options.reconnect, options.width, options.height,
            Clock::now() + std::chrono::seconds(options.seconds), {});
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return exit_status::usage;
    }
}
