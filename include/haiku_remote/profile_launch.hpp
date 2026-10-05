#pragma once

// The bridge from a saved profile to the TransportOptions a session connects
// with -- and the single, explicit seam where Part 2's managed tunnel will
// slot in.
//
// Part 1 contract: given a profile, produce the TransportOptions a frontend can
// hand straight to make_transport(). Only `direct` is fully wired, because it
// needs nothing this client does not already have. `ssh` (the tunnel) and `wss`
// (the broker) each require a lifecycle Part 1 does not build -- spawning and
// supervising an ssh process, or acquiring and presenting a broker token -- so
// for now they are reported as stubs and fall back to a best-effort direct
// attempt to host:remotePort. The fallback is honest, not silent: `is_stub` is
// set and `note` says what Part 2 will do instead, so a frontend can surface it
// and a reader can see exactly where the deferred work attaches.
//
// Part 2 replaces plan_launch() for ssh/wss with code that stands up the tunnel
// or broker session and returns TransportOptions pointing at the local end of
// it, plus a handle whose destructor tears it back down. Nothing else in the
// client needs to change: frontends already drive a session from
// TransportOptions.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/transport.hpp"

#include <string>

namespace haiku_remote {

struct LaunchPlan {
    // Ready to pass to make_transport().
    TransportOptions transport;
    // Human-readable description of what will actually happen, including the
    // Part 2 caveat for a stubbed mode. Always set.
    std::string note;
    // True when the profile's mode asks for behaviour deferred to Part 2 and
    // this plan is a fallback rather than the real thing.
    bool is_stub = false;
};

// Build the launch plan for a profile. Pure except for a best-effort read of
// cookie_source when it names an existing file (direct mode needs app_server's
// session cookie as its first frame); the cookie is read at connect time only
// and never stored. Never throws.
[[nodiscard]] LaunchPlan plan_launch(const ConnectionProfile& profile);

} // namespace haiku_remote
