#include "haiku_remote/connection_profile.hpp"

#include "haiku_remote/profile_launch.hpp" // default_broker_port
#include "haiku_remote/surface.hpp"

#include <cctype>
#include <cstdlib>
#include <iterator>
#include <sstream>

namespace haiku_remote {

std::string_view mode_name(ConnectionMode mode)
{
    switch (mode) {
        case ConnectionMode::ssh: return "ssh";
        case ConnectionMode::direct: return "direct";
        case ConnectionMode::wss: return "wss";
        case ConnectionMode::automatic: return "auto";
    }
    return "ssh";
}

std::optional<ConnectionMode> mode_from_name(std::string_view name)
{
    for (const ConnectionMode mode : connection_modes) {
        if (mode_name(mode) == name)
            return mode;
    }
    return std::nullopt;
}

bool mode_available(ConnectionMode mode, bool wss_available)
{
    return mode != ConnectionMode::wss || wss_available;
}

ConnectionMode next_mode(ConnectionMode mode, bool wss_available)
{
    constexpr std::size_t count = std::size(connection_modes);
    std::size_t at = 0;
    while (at < count && connection_modes[at] != mode)
        ++at;
    // From the mode's place (or the top, for a mode not in the list), the next
    // one this build can use. ssh always can, so this always finds one.
    for (std::size_t step = 1; step <= count; ++step) {
        const ConnectionMode candidate = connection_modes[(at + step) % count];
        if (mode_available(candidate, wss_available))
            return candidate;
    }
    return ConnectionMode::ssh;
}

std::string mode_label(ConnectionMode mode)
{
    switch (mode) {
        case ConnectionMode::ssh: return "SSH tunnel";
        case ConnectionMode::direct: return "Direct TCP";
        case ConnectionMode::wss: return "Broker (WSS)";
        case ConnectionMode::automatic: return "Auto";
    }
    // A mode this table does not know yet still shows, as its stored name in
    // capitals ("QUIC").
    std::string name(mode_name(mode));
    for (char& c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

bool mode_uses_ssh(ConnectionMode mode)
{
    // wss fetches the broker's token and certificate over SSH, and auto runs
    // both that fetch and the tunnel; only direct never starts ssh.
    return mode != ConnectionMode::direct;
}

std::string ConnectionProfile::route_summary(bool wss_available) const
{
    const std::string shown_host = host.empty() ? "<host>" : host;
    std::ostringstream out;
    // The summary describes the route plan_launch() actually takes. Direct is
    // one TCP connection to app_server's port, no SSH. The broker listens on
    // default_broker_port, not on remote_port (app_server's port, which the
    // broker proxies to), and "auto" is planned as broker first, then the SSH
    // tunnel (profile_launch.cpp).
    if (mode == ConnectionMode::direct) {
        out << "client -> " << shown_host << ':' << remote_port;
        return out.str();
    }
    // Without wss, auto is planned as the tunnel alone (and a wss profile is
    // refused by validate(), but still summarised as what it asks for).
    const bool broker_first = mode == ConnectionMode::automatic && wss_available;
    if (broker_first || mode == ConnectionMode::wss)
        out << "client -> wss -> broker " << shown_host << ':'
            << default_broker_port;
    if (mode == ConnectionMode::wss)
        return out.str();
    if (broker_first)
        out << " | else ";
    out << "client -> 127.0.0.1:";
    if (local_port)
        out << *local_port;
    else
        out << "auto";
    out << " -> ssh -> " << shown_host << ':' << ssh_port
        << " -> 127.0.0.1:" << remote_port;
    return out.str();
}

const FieldError* ValidationResult::find(std::string_view field) const
{
    for (const auto& error : errors) {
        if (error.field == field)
            return &error;
    }
    return nullptr;
}

namespace {

void check_port(std::vector<FieldError>& errors, const char* field, int port)
{
    if (port < min_port || port > max_port) {
        errors.push_back(
            {field, "port must be between " + std::to_string(min_port) + " and "
                        + std::to_string(max_port)});
    }
}

void check_dimension(std::vector<FieldError>& errors, const char* field,
                     int value)
{
    if (value < 1 || value > Surface::max_dimension) {
        errors.push_back(
            {field, std::string(field) + " must be between 1 and "
                        + std::to_string(Surface::max_dimension)});
    }
}

} // namespace

ValidationResult validate(const ConnectionProfile& profile, bool wss_available)
{
    ValidationResult result;
    auto& errors = result.errors;

    if (!mode_available(profile.mode, wss_available))
        errors.push_back({"mode", "this build has no TLS support, so it cannot"
                                  " connect to the broker (wss); choose another"
                                  " mode"});

    if (profile.name.empty())
        errors.push_back({"name", "name must not be empty"});

    if (profile.host.empty()) {
        errors.push_back({"host", profile.mode == ConnectionMode::ssh
                                      ? "host is required in SSH tunnel mode"
                                      : "host is required"});
    }

    check_port(errors, "remotePort", profile.remote_port);
    if (mode_uses_ssh(profile.mode))
        check_port(errors, "sshPort", profile.ssh_port);
    if (profile.local_port)
        check_port(errors, "localPort", *profile.local_port);

    check_dimension(errors, "width", profile.width);
    check_dimension(errors, "height", profile.height);

    result.route = profile.route_summary(wss_available);
    return result;
}

std::string expand_user_path(const std::string& path)
{
    if (path.empty() || path.front() != '~')
        return path;
    // Only a bare "~" or a "~/..." prefix expands; "~user" is left alone.
    if (path.size() != 1 && path[1] != '/')
        return path;

    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (home == nullptr || *home == '\0')
        home = std::getenv("USERPROFILE");
#endif
    if (home == nullptr || *home == '\0')
        return path;
    return std::string(home) + path.substr(1);
}

} // namespace haiku_remote
