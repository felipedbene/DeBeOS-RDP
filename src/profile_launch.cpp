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
// a path that does not exist) yields no cookie -- the connect then fails with
// the gate's own message rather than a guess. Best-effort and never throws.
std::string read_cookie_source(const std::string& source)
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

} // namespace

LaunchPlan plan_launch(const ConnectionProfile& profile)
{
    LaunchPlan plan;
    const std::uint16_t remote = clamp_port(profile.remote_port);

    switch (profile.mode) {
    case ConnectionMode::direct:
        plan.transport.host = profile.host;
        plan.transport.port = remote;
        plan.transport.cookie = read_cookie_source(profile.cookie_source);
        plan.note = "direct connect to " + profile.host + ":"
                    + std::to_string(remote);
        return plan;

    case ConnectionMode::ssh:
        // Part 2 owns the tunnel: spawn ssh -L <local>:127.0.0.1:<remote>, wait
        // for the forward, then connect to 127.0.0.1:<local>. Until then, try a
        // direct connect to host:remotePort -- which succeeds only where that
        // port is already reachable (an existing forward), and otherwise fails
        // with a connect error rather than a wrong picture.
        plan.transport.host = profile.host;
        plan.transport.port = remote;
        plan.transport.cookie = read_cookie_source(profile.cookie_source);
        plan.is_stub = true;
        plan.note = "SSH tunnel mode is handled in Part 2; attempting a direct "
                    "connect to " + profile.host + ":" + std::to_string(remote)
                    + " for now";
        return plan;

    case ConnectionMode::wss:
        // Part 2 owns the broker handshake: acquire a session token, present it
        // as RP_AUTHENTICATE, and (optionally) pin the broker certificate. The
        // profile deliberately stores no token, so this fallback opens a plain
        // wss:// to host:remotePort with none; the broker refuses it, which is
        // the correct, visible failure until Part 2 supplies the token.
        plan.transport.url =
            "wss://" + profile.host + ":" + std::to_string(remote);
        plan.is_stub = true;
        plan.note = "wss/broker mode is handled in Part 2 (session token + "
                    "certificate pinning); opening wss://" + profile.host + ":"
                    + std::to_string(remote) + " without a managed token for now";
        return plan;
    }

    plan.transport.host = profile.host;
    plan.transport.port = remote;
    plan.note = "direct connect to " + profile.host + ":"
                + std::to_string(remote);
    return plan;
}

} // namespace haiku_remote
