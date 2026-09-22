#include "haiku_remote/png_writer.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/transport.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
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
        else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: haiku-remote" << transport_usage()
                      << "\n  [--width PX] [--height PX] [--seconds N]"
                         " [--output FILE.png]\n";
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
            return 1;
        }
        if (!transport->connect(error)) {
            std::cerr << "connect to " << transport->describe() << " failed: "
                      << error << '\n';
            return 1;
        }
        Session session(
            options.width, options.height,
            [&](std::span<const std::uint8_t> bytes) {
                return transport->send_all(bytes, error);
            },
            [](std::string_view line) { std::cerr << line << '\n'; });
        session.start();

        std::array<std::uint8_t, 256 * 1024> buffer {};
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(options.seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            const int count = transport->receive(buffer, 100, error);
            if (count < 0) {
                std::cerr << "receive failed: " << error << '\n';
                return 1;
            }
            if (count > 0)
                session.ingest(std::span(buffer.data(), static_cast<std::size_t>(count)));
        }
        if (!write_png(session.surface(), options.output, error)) {
            std::cerr << "capture failed: " << error << '\n';
            return 1;
        }
        std::cout << "wrote " << options.output << " after "
                  << session.message_count() << " messages";
        if (!session.unhandled().empty())
            std::cout << " (" << session.unhandled().size() << " unhandled opcodes)";
        std::cout << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
