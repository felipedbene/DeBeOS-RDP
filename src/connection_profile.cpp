#include "haiku_remote/connection_profile.hpp"

#include "haiku_remote/surface.hpp"

#include <cstdlib>
#include <sstream>

namespace haiku_remote {

std::string_view mode_name(ConnectionMode mode)
{
    switch (mode) {
        case ConnectionMode::ssh: return "ssh";
        case ConnectionMode::direct: return "direct";
        case ConnectionMode::wss: return "wss";
    }
    return "ssh";
}

std::optional<ConnectionMode> mode_from_name(std::string_view name)
{
    if (name == "ssh")
        return ConnectionMode::ssh;
    if (name == "direct")
        return ConnectionMode::direct;
    if (name == "wss")
        return ConnectionMode::wss;
    return std::nullopt;
}

std::string ConnectionProfile::route_summary() const
{
    const std::string shown_host = host.empty() ? "<host>" : host;
    std::ostringstream out;
    if (mode == ConnectionMode::direct) {
        out << "client -> " << shown_host << ':' << remote_port;
        return out.str();
    }
    if (mode == ConnectionMode::wss) {
        out << "client -> wss -> broker " << shown_host << ':' << remote_port;
        return out.str();
    }
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

ValidationResult validate(const ConnectionProfile& profile)
{
    ValidationResult result;
    auto& errors = result.errors;

    if (profile.name.empty())
        errors.push_back({"name", "name must not be empty"});

    if (profile.host.empty()) {
        errors.push_back({"host", profile.mode == ConnectionMode::ssh
                                      ? "host is required in SSH tunnel mode"
                                      : "host is required"});
    }

    check_port(errors, "remotePort", profile.remote_port);
    if (profile.mode == ConnectionMode::ssh)
        check_port(errors, "sshPort", profile.ssh_port);
    if (profile.local_port)
        check_port(errors, "localPort", *profile.local_port);

    check_dimension(errors, "width", profile.width);
    check_dimension(errors, "height", profile.height);

    result.route = profile.route_summary();
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
