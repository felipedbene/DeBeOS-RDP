#include "haiku_remote/png_writer.hpp"
#include "haiku_remote/reconnect.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/transport.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace haiku_remote;

namespace {

struct Options {
    TransportOptions transport;
    ReconnectConfig reconnect;
    int width = 1280;
    int height = 800;
    int seconds = 2;
    std::string output = "haiku-remote.png";
    bool draw_cursor = false;
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
        if (parse_transport_argument(options.transport, argument, value)) {}
        else if (parse_reconnect_argument(options.reconnect, argument, value)) {}
        else if (argument == "--width")
            options.width = parse_integer(
                value(), "width", 1, Surface::max_dimension);
        else if (argument == "--height")
            options.height = parse_integer(
                value(), "height", 1, Surface::max_dimension);
        else if (argument == "--seconds")
            options.seconds = parse_integer(
                value(), "seconds", 0, std::numeric_limits<int>::max());
        else if (argument == "--output") options.output = value();
        else if (argument == "--draw-cursor") options.draw_cursor = true;
        else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: haiku-remote" << transport_usage()
                      << "\n  [--width PX] [--height PX] [--seconds N]"
                         " [--output FILE.png] [--draw-cursor]\n"
                      << reconnect_usage() << "\n\n"
                      << exit_status_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    return options;
}

// The outcome of one connect-and-receive attempt, gathered for the reconnect
// policy. `fatal_error` carries a genuine local failure (a PNG that could not be
// written, a handshake that could not be sent) that ends the run regardless of
// the policy.
struct AttemptResult {
    ConnectionResult connection;
    bool fatal_error = false;
};

// Drive one connection through `session`: connect a fresh transport, run the
// receive loop until the deadline or until the stream ends, and report how it
// ended. `session` persists across attempts so the surface and the session
// identity survive a reconnect; the caller calls session.reset() between
// attempts.
AttemptResult run_one_connection(const Options& options, Session& session,
                                 std::chrono::steady_clock::time_point deadline)
{
    AttemptResult attempt;
    std::string error;

    const auto transport = make_transport(options.transport, error);
    if (transport == nullptr) {
        // A malformed URL or unsupported scheme is a misinvocation, not a
        // transport drop: fail the whole run rather than retry it.
        std::cerr << error << '\n';
        attempt.fatal_error = true;
        return attempt;
    }
    if (!transport->connect(error)) {
        std::cerr << "connect to " << transport->describe() << " failed: "
                  << error << '\n';
        attempt.connection.connected = false;
        attempt.connection.connect_failure = transport->connect_failure();
        return attempt;
    }
    attempt.connection.connected = true;

    // Point the session's sender at this attempt's transport. On a reconnect
    // start() re-presents RP_INIT_CONNECTION + RP_HELLO (with RP_CAP_RESYNC), so
    // the server re-negotiates and replays state onto the reset session.
    session.set_sender([&](std::span<const std::uint8_t> bytes) {
        return transport->send_all(bytes, error);
    });

    error.clear();
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
    while (std::chrono::steady_clock::now() < deadline) {
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

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parse_options(argc, argv);
        const ReconnectPolicy policy(options.reconnect);

        // The session, and the surface it paints into, live across reconnects.
        // The send callback is rebound to each attempt's transport inside
        // run_one_connection().
        Session session(
            options.width, options.height,
            [](std::span<const std::uint8_t>) { return true; },
            [](std::string_view line) { std::cerr << line << '\n'; });

        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(options.seconds);

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
            auto attempt = run_one_connection(options, session, deadline);
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

            if (!policy.should_retry(outcome, attempts_made))
                break;

            auto delay = policy.backoff_for(attempts_made);
            const auto now = std::chrono::steady_clock::now();
            if (now + delay >= deadline)
                break; // No time left in the capture window to try again.
            std::cerr << "connection ended (" << outcome_name(outcome)
                      << "); reconnecting in " << delay.count() << " ms"
                      << " (attempt " << (attempts_made + 1) << " of "
                      << options.reconnect.max_attempts << ")\n";
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
        std::cout << '\n';
        return exit_status::ok;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return exit_status::usage;
    }
}
