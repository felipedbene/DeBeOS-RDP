#include "haiku_remote/launch_options.hpp"

#include "haiku_remote/surface.hpp"

#include <charconv>
#include <stdexcept>

namespace haiku_remote {

namespace {

int parse_integer(std::string_view value, std::string_view name, int minimum,
                  int maximum)
{
    int number = 0;
    const auto [pointer, status]
        = std::from_chars(value.data(), value.data() + value.size(), number);
    if (status != std::errc() || pointer != value.data() + value.size()
        || number < minimum || number > maximum) {
        throw std::runtime_error("invalid " + std::string(name) + ": "
                                 + std::string(value));
    }
    return number;
}

} // namespace

std::string_view launch_usage()
{
    return " [connection flags]\n"
           "  With no connection flag the connection library opens; with one it"
           " connects\n"
           "  immediately (automation/debug), using a never-saved profile.\n"
           "\n"
           "  Direct TCP (an already-forwarded endpoint):\n"
           "    [--host HOST] [--port PORT] [--remote-port PORT]\n"
           "  Managed SSH tunnel:\n"
           "    --ssh-host HOST [--ssh-user USER] [--ssh-port PORT]\n"
           "    [--identity FILE] [--remote-port PORT] [--local-port PORT]\n"
           "  Broker (WebSocket): [--url ws://|wss://HOST[:PORT][/PATH]]\n"
           "  Saved profile:      [--profile ID-OR-NAME]\n"
           "  Force direct mode:  [--direct]\n"
           "  Credentials/TLS:    [--cookie COOKIE | --cookie-file FILE]\n"
           "                      [--token TOKEN | --token-file FILE]\n"
           "                      [--pin-sha256 DIGEST] [--ca-file FILE.pem]"
           " [--insecure]\n"
           "  Window:             [--width PX] [--height PX]";
}

LaunchResult parse_launch_options(int argc, char** argv)
{
    LaunchResult result;
    LaunchOptions& options = result.options;

    // Fields the endpoint flags fill, resolved into the profile after the loop.
    bool endpoint_given = false;
    bool force_direct = false;
    bool have_ssh = false;
    std::string ssh_host;
    std::string ssh_user = "baron";
    int ssh_port = 22;
    std::string identity;
    bool remote_port_given = false;
    int remote_port = 10900;
    bool local_port_given = false;
    int local_port = 0;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            auto value = [&]() -> std::string {
                if (++i >= argc)
                    throw std::runtime_error("missing value for " + argument);
                return argv[i];
            };

            // The "where to connect" flags also decide library-vs-connect.
            if (argument == "--host" || argument == "--port"
                || argument == "--url")
                endpoint_given = true;

            if (parse_transport_argument(options.transport, argument, value))
                continue;
            if (parse_reconnect_argument(options.reconnect, argument, value))
                continue;

            if (argument == "--profile") {
                options.profile_ref = value();
                endpoint_given = true;
            } else if (argument == "--direct") {
                force_direct = true;
                endpoint_given = true;
            } else if (argument == "--ssh-host") {
                ssh_host = value();
                have_ssh = true;
                endpoint_given = true;
            } else if (argument == "--ssh-user") {
                ssh_user = value();
            } else if (argument == "--ssh-port") {
                ssh_port = parse_integer(value(), "--ssh-port", min_port, max_port);
            } else if (argument == "--identity") {
                identity = value();
            } else if (argument == "--remote-port") {
                remote_port = parse_integer(value(), "--remote-port", min_port,
                                            max_port);
                remote_port_given = true;
                endpoint_given = true;
            } else if (argument == "--local-port") {
                local_port = parse_integer(value(), "--local-port", min_port,
                                           max_port);
                local_port_given = true;
            } else if (argument == "--width") {
                options.width = parse_integer(value(), "--width", 1,
                                              Surface::max_dimension);
                options.size_explicit = true;
            } else if (argument == "--height") {
                options.height = parse_integer(value(), "--height", 1,
                                               Surface::max_dimension);
                options.size_explicit = true;
            } else if (argument == "--help" || argument == "-h") {
                result.show_help = true;
                return result;
            } else {
                throw std::runtime_error("unknown argument: " + argument);
            }
        }

        options.uses_url = !options.transport.url.empty();
        options.open_library = !endpoint_given;

        if (have_ssh && force_direct)
            throw std::runtime_error("--direct cannot be combined with --ssh-host");
        if (have_ssh && options.uses_url)
            throw std::runtime_error("--ssh-host cannot be combined with --url");
        if (options.open_library)
            return result; // nothing more to validate; the library will open

        // A saved profile is resolved by the caller; the ephemeral profile below
        // is unused in that case.
        if (!options.profile_ref.empty())
            return result;

        // Build the ephemeral profile.
        ConnectionProfile& profile = options.profile;
        profile.width = options.width;
        profile.height = options.height;
        profile.auto_reconnect = options.reconnect.enabled;

        if (have_ssh) {
            profile.mode = ConnectionMode::ssh;
            profile.host = ssh_host;
            profile.ssh_user = ssh_user;
            profile.ssh_port = ssh_port;
            profile.identity_file = identity;
            profile.remote_port = remote_port;
            if (local_port_given)
                profile.local_port = local_port;
            profile.name = ssh_host;
        } else {
            // Direct TCP (also the shape the broker URL path connects through,
            // where host/port are ignored in favour of the URL).
            profile.mode = ConnectionMode::direct;
            profile.host = options.transport.host;
            profile.remote_port = remote_port_given
                ? remote_port
                : static_cast<int>(options.transport.port);
            profile.name = options.transport.host;
        }

        // A broker URL carries its own endpoint; the profile's host is only a
        // label then, so do not reject it for validation. Otherwise validate the
        // ephemeral profile before anyone tries to connect with it.
        if (!options.uses_url) {
            const ValidationResult validation = validate(profile);
            if (!validation.ok()) {
                const FieldError& first = validation.errors.front();
                throw std::runtime_error(first.field + ": " + first.message);
            }
        }
    } catch (const std::exception& error) {
        result.ok = false;
        result.exit_code = exit_status::usage;
        result.error = error.what();
    }
    return result;
}

CoordinatorConfig coordinator_config_from(const LaunchOptions& options)
{
    CoordinatorConfig config;
    config.profile = options.profile;
    config.cookie = options.transport.cookie;
    config.reconnect = options.reconnect;
    if (options.uses_url)
        config.transport_override = options.transport;
    return config;
}

} // namespace haiku_remote
