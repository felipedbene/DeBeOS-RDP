#include "haiku_remote/launch_arguments.hpp"

#include <charconv>
#include <stdexcept>
#include <string>

namespace haiku_remote {

namespace {

int parse_port(const std::string& text, std::string_view option)
{
    unsigned int port = 0;
    const auto [pointer, status]
        = std::from_chars(text.data(), text.data() + text.size(), port);
    if (status != std::errc() || pointer != text.data() + text.size()
        || port < static_cast<unsigned int>(min_port)
        || port > static_cast<unsigned int>(max_port))
        throw std::runtime_error("invalid " + std::string(option) + ": " + text);
    return static_cast<int>(port);
}

std::string non_empty(std::string text, std::string_view option)
{
    if (text.empty())
        throw std::runtime_error(std::string(option) + " needs a value");
    return text;
}

// The ssh-only arguments that were given, for an error message.
std::string ssh_arguments_seen(const LaunchArguments& a)
{
    std::string seen;
    const auto add = [&](bool present, const char* name) {
        if (!present)
            return;
        if (!seen.empty())
            seen += ", ";
        seen += name;
    };
    add(!a.ssh_host.empty(), "--ssh-host");
    add(a.ssh_user.has_value(), "--ssh-user");
    add(a.identity.has_value(), "--identity");
    add(a.ssh_port.has_value(), "--ssh-port");
    add(a.local_port.has_value(), "--local-port");
    return seen;
}

} // namespace

bool parse_launch_argument(LaunchArguments& a, std::string_view argument,
                           const std::function<std::string()>& value)
{
    if (argument == "--direct") {
        a.direct = true;
    } else if (argument == "--profile") {
        a.profile = non_empty(value(), "--profile");
    } else if (argument == "--profiles") {
        a.profiles_file = non_empty(value(), "--profiles");
    } else if (argument == "--ssh-host") {
        a.ssh_host = non_empty(value(), "--ssh-host");
    } else if (argument == "--ssh-user") {
        a.ssh_user = non_empty(value(), "--ssh-user");
    } else if (argument == "--identity") {
        a.identity = non_empty(value(), "--identity");
    } else if (argument == "--ssh-port") {
        a.ssh_port = parse_port(value(), "--ssh-port");
    } else if (argument == "--remote-port") {
        a.remote_port = parse_port(value(), "--remote-port");
    } else if (argument == "--local-port") {
        a.local_port = parse_port(value(), "--local-port");
    } else if (argument == "--cookie-file") {
        // Read (and validated) by the shared transport parser exactly as
        // before; the path is also kept, because an --ssh-host launch hands it
        // to the profile as cookie_source rather than as bytes.
        std::string path;
        const auto capture = [&]() {
            path = value();
            return path;
        };
        parse_transport_argument(a.transport, argument, capture);
        a.cookie_file = path;
        a.credential_arguments = true;
    } else if (parse_transport_argument(a.transport, argument, value)) {
        if (argument == "--host" || argument == "--port" || argument == "--url") {
            a.endpoint_arguments = true;
            if (argument == "--port")
                a.port_argument = true;
        } else {
            a.credential_arguments = true;
            if (argument == "--cookie")
                a.cookie_inline = true;
            else
                a.broker_arguments = true;
        }
    } else {
        return false;
    }
    return true;
}

LaunchTarget resolve_launch_target(LaunchArguments& a)
{
    const std::string ssh_seen = ssh_arguments_seen(a);
    const bool any_ssh = !ssh_seen.empty();

    if (!a.profile.empty()) {
        if (a.direct || any_ssh || a.remote_port || a.endpoint_arguments
            || a.credential_arguments)
            throw std::runtime_error(
                "--profile launches a saved connection as it is saved; it cannot"
                " be combined with --direct, --ssh-*, --identity, --remote-port,"
                " --local-port or the --host/--port/--url/credential arguments"
                " (edit the profile, or launch without --profile)");
        return LaunchTarget::saved_profile;
    }
    if (!a.profiles_file.empty() && (a.endpoint_arguments || a.direct || any_ssh))
        throw std::runtime_error(
            "--profiles names the library file; it only means something with"
            " --profile, or alone (the GUI then opens that library)");

    if (!a.ssh_host.empty()) {
        if (a.direct)
            throw std::runtime_error(
                "--direct and --ssh-host are two different routes; pick one");
        if (a.endpoint_arguments)
            throw std::runtime_error(
                "--ssh-host already names the host; --host/--port/--url are for"
                " a direct connection (use --remote-port for app_server's port"
                " behind the tunnel)");
        if (a.cookie_inline)
            throw std::runtime_error(
                "an --ssh-host launch reads the cookie with --cookie-file (or"
                " fetches it over SSH); --cookie on the command line is not"
                " supported there");
        // Only --cookie-file among the credential arguments means anything on
        // the tunnel route; the rest are broker (ws/wss) arguments.
        if (a.broker_arguments)
            throw std::runtime_error(
                "the --token/--token-file/--pin-sha256/--ca-file/--insecure/"
                "--known-broker*/--trust-new-broker arguments are for a broker"
                " (--url wss://...) connection, not an --ssh-host tunnel");
        return LaunchTarget::ssh_profile;
    }
    if (any_ssh)
        throw std::runtime_error(ssh_seen
                                 + " only apply to a tunnel launch; add"
                                   " --ssh-host HOST");

    if (a.direct || a.endpoint_arguments || a.credential_arguments
        || a.remote_port) {
        if (a.remote_port) {
            // In direct mode --remote-port and --port name the same thing:
            // app_server's port on --host. Accept either, refuse a conflict.
            if (a.port_argument && a.transport.port != *a.remote_port)
                throw std::runtime_error(
                    "--port and --remote-port disagree; in a direct launch they"
                    " name the same port, give one");
            if (!a.transport.url.empty())
                throw std::runtime_error(
                    "--url carries its own port; --remote-port does not apply");
            a.transport.port = static_cast<std::uint16_t>(*a.remote_port);
        }
        return LaunchTarget::endpoint;
    }
    return LaunchTarget::library;
}

ConnectionProfile ssh_launch_profile(const LaunchArguments& a, int width,
                                     int height)
{
    ConnectionProfile p;
    p.name = a.ssh_host;
    p.host = a.ssh_host;
    p.mode = ConnectionMode::ssh;
    if (a.ssh_user)
        p.ssh_user = *a.ssh_user;
    if (a.ssh_port)
        p.ssh_port = *a.ssh_port;
    if (a.identity)
        p.identity_file = *a.identity;
    if (a.remote_port)
        p.remote_port = *a.remote_port;
    p.local_port = a.local_port;
    p.cookie_source = a.cookie_file;
    p.width = width;
    p.height = height;
    const ValidationResult result = validate(p);
    if (!result.ok()) {
        const FieldError& first = result.errors.front();
        throw std::runtime_error("invalid " + first.field + ": " + first.message);
    }
    return p;
}

const ConnectionProfile* find_profile(const std::vector<ConnectionProfile>& profiles,
                                      std::string_view name_or_id)
{
    for (const auto& candidate : profiles)
        if (candidate.id == name_or_id)
            return &candidate;
    for (const auto& candidate : profiles)
        if (candidate.name == name_or_id)
            return &candidate;
    return nullptr;
}

std::string_view launch_usage()
{
    return "  [--direct]   a direct connection to --host/--port (the default"
           " when only\n"
           "      --host/--port are given; a saved profile's Direct TCP mode is"
           " the same\n"
           "      route). app_server binds 127.0.0.1, so this is for a local"
           " server or a\n"
           "      forward you already run, e.g. an SSM port forward\n"
           "  [--ssh-host HOST [--ssh-user USER] [--ssh-port N] [--identity"
           " KEY]\n"
           "      [--remote-port N] [--local-port N] [--cookie-file FILE]]\n"
           "      connect through an ssh -L tunnel this client starts and owns"
           " (the\n"
           "      cookie is fetched over SSH unless --cookie-file names it)\n"
           "  [--profile NAME|ID [--profiles connections.json]]   launch a"
           " saved\n"
           "      connection directly (bypasses the library)";
}

} // namespace haiku_remote
