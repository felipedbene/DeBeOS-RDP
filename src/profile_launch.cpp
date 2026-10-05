#include "haiku_remote/profile_launch.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace haiku_remote {

namespace {

std::uint16_t clamp_port(int port)
{
    if (port < 1)
        return 1;
    if (port > 65535)
        return 65535;
    return static_cast<std::uint16_t>(port);
}

// Read cookie_source when it is a path to an existing regular file, trimming a
// single trailing newline. Anything else (empty, a source tag such as "broker",
// a path that does not exist) yields no cookie -- open_connection() then fetches
// it live over SSH. Best-effort and never throws.
std::string read_cookie_file(const std::string& source)
{
    if (source.empty())
        return {};
    std::error_code ec;
    const std::filesystem::path path = expand_user_path(source);
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return {};
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string cookie = buffer.str();
    while (!cookie.empty() && (cookie.back() == '\n' || cookie.back() == '\r'))
        cookie.pop_back();
    return cookie;
}

// The broker route template: wss://host:<broker port>. The token and ca_file
// are deliberately left empty -- the profile stores no secret, and both are
// acquired by open_connection() at connect time (over SSH). A session cookie
// never belongs on this path (the broker reads app_server's cookie itself), so
// it is left empty too; make_transport() refuses a cookie over wss anyway.
RouteStep broker_step(const ConnectionProfile& profile)
{
    RouteStep step;
    step.kind = RouteKind::broker;
    step.transport.url = "wss://" + profile.host + ":"
                         + std::to_string(default_broker_port);
    step.note = "broker (wss) at " + step.transport.url;
    return step;
}

// The tunnel route template: TcpTransport to the local end of an SSH -L
// forward. host/port point at 127.0.0.1 and a placeholder 0 -- open_connection()
// chooses the real local port once the forward is up (or honours
// profile.local_port when the profile pins one). The cookie is read here only
// when cookie_source is a plain file; otherwise open_connection() fetches it.
RouteStep tunnel_step(const ConnectionProfile& profile)
{
    RouteStep step;
    step.kind = RouteKind::tunnel;
    step.transport.host = "127.0.0.1";
    step.transport.port = profile.local_port && *profile.local_port >= 1
                                  && *profile.local_port <= 65535
                              ? static_cast<std::uint16_t>(*profile.local_port)
                              : 0;
    step.transport.cookie = read_cookie_file(profile.cookie_source);
    const std::uint16_t remote = clamp_port(profile.remote_port);
    step.note = "ssh tunnel to " + profile.ssh_user + "@" + profile.host + ":"
                + std::to_string(profile.ssh_port) + " -> 127.0.0.1:"
                + std::to_string(remote);
    return step;
}

} // namespace

std::string_view route_kind_name(RouteKind kind)
{
    switch (kind) {
    case RouteKind::broker:
        return "broker";
    case RouteKind::tunnel:
        return "tunnel";
    }
    return "broker";
}

LaunchPlan plan_launch(const ConnectionProfile& profile)
{
    LaunchPlan plan;

    switch (profile.mode) {
    case ConnectionMode::wss:
        // Forced broker: a single step, no fallback.
        plan.steps.push_back(broker_step(profile));
        plan.note = "wss mode: " + plan.steps.front().note + " (broker only)";
        break;

    case ConnectionMode::ssh:
        // Forced tunnel: a single step, no fallback.
        plan.steps.push_back(tunnel_step(profile));
        plan.note = "ssh mode: " + plan.steps.front().note + " (tunnel only)";
        break;

    case ConnectionMode::direct:
        // Auto: try the broker first, fall back to the SSH tunnel.
        plan.steps.push_back(broker_step(profile));
        plan.steps.push_back(tunnel_step(profile));
        plan.note = "direct mode (auto): " + plan.steps[0].note
                    + ", falling back to " + plan.steps[1].note;
        break;
    }

    // Non-exhaustive guard: an unknown mode degrades to a tunnel rather than an
    // empty plan, so plan.steps is never empty.
    if (plan.steps.empty()) {
        plan.steps.push_back(tunnel_step(profile));
        plan.note = plan.steps.front().note;
    }

    plan.transport = plan.steps.front().transport;
    return plan;
}

} // namespace haiku_remote
