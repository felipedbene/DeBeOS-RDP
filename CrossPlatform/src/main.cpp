#include "haiku_remote/png_writer.hpp"
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

using namespace haiku_remote;

namespace {

struct Options {
    TransportOptions transport;
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
                         " [--output FILE.png] [--draw-cursor]\n\n"
                      << exit_status_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parse_options(argc, argv);
        std::string error;
        const auto transport = make_transport(options.transport, error);
        if (transport == nullptr) {
            std::cerr << error << '\n';
            return exit_status::failed;
        }
        if (!transport->connect(error)) {
            std::cerr << "connect to " << transport->describe() << " failed: "
                      << error << '\n';
            // A credential this client was never given exits differently from a
            // refusal out on the wire: they are different problems with
            // different remedies, and a harness should not have to read prose
            // to tell them apart.
            return connect_exit_status(*transport);
        }
        Session session(
            options.width, options.height,
            [&](std::span<const std::uint8_t> bytes) {
                return transport->send_all(bytes, error);
            },
            [](std::string_view line) { std::cerr << line << '\n'; });
        // Session::start() returns void and ignores whether the handshake went
        // out, so a connection that dies between connect() and the first write
        // otherwise looks like a session that simply received nothing. The send
        // callback only assigns `error` on failure, so an empty string after
        // start() means both handshake messages were written.
        error.clear();
        session.start();
        if (!error.empty()) {
            std::cerr << "handshake send to " << transport->describe()
                      << " failed: " << error << '\n';
            return exit_status::failed;
        }

        std::array<std::uint8_t, 256 * 1024> buffer {};
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(options.seconds);
        // Why the capture survives the end of the session: app_server hangs up
        // on shutdown -- RemoteHWInterface::_Disconnect() sends
        // RP_CLOSE_CONNECTION and closes the endpoint -- so the stream ending
        // before --seconds elapses is the NORMAL case, not a failure. Bailing
        // out of the loop with `return 1` threw away every pixel that had
        // already arrived and decoded.
        bool orderly_end = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const int count = transport->receive(buffer, 100, error);
            if (count < 0) {
                if (transport->peer_closed()) {
                    orderly_end = true;
                    break;
                }
                std::cerr << "receive failed: " << error << '\n';
                // Still write what was decoded: the pixels are real even when
                // the stream died badly. The exit code stays non-zero.
                std::string ignored;
                if (write_png(session.surface(), options.output, ignored)) {
                    std::cerr << "wrote " << options.output << " anyway after "
                              << session.message_count() << " messages\n";
                }
                return exit_status::failed;
            }
            if (count > 0)
                session.ingest(std::span(buffer.data(), static_cast<std::size_t>(count)));
            // RP_CLOSE_CONNECTION arrives before the EOF does. Stop on it
            // rather than spinning out the rest of --seconds against a socket
            // that will never say anything again.
            if (session.server_closed()) {
                orderly_end = true;
                break;
            }
        }
        // Compositing happens on a *copy*, never on session.surface(): the
        // framebuffer is what the next frame's drawing and any RP_READ_BITMAP
        // readback are computed against, so burning a cursor into it would make
        // the cursor's own pixels part of the server's picture of the screen.
        // The flag is opt-in because the default capture is a framebuffer
        // capture -- directly comparable with earlier PNGs and with what a
        // readback returns, neither of which contains a cursor -- while
        // --draw-cursor gives the screen as a user would see it.
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
        // A hang-up before the first message is not a short session, it is a
        // session that never started: something upstream -- authentication, an
        // access check, the wrong port -- refused it, and the only evidence is
        // that nothing was ever decoded. Reporting that as success is how a
        // blank capture passes for a working desktop.
        if (orderly_end && session.message_count() == 0) {
            std::cerr << "wrote " << options.output
                      << " but the session produced nothing: the server closed"
                         " the connection before sending any drawing data, so"
                         " the session was refused or never started\n";
            return exit_status::failed;
        }
        std::cout << "wrote " << options.output << " after "
                  << session.message_count() << " messages";
        if (!session.unhandled().empty())
            std::cout << " (" << session.unhandled().size() << " unhandled opcodes)";
        if (orderly_end) {
            std::cout << (session.server_closed()
                              ? " (server closed the connection)"
                              : " (server hung up)");
        }
        std::cout << '\n';
        return exit_status::ok;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return exit_status::usage;
    }
}
