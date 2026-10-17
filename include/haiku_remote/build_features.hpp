#pragma once

// What this build can speak, decided once, at compile time, for every header
// that needs to know: the routes a build cannot honour are never planned or
// offered (connection_profile.hpp, profile_launch.hpp).
//
// wss:// needs OpenSSL (the build defines HAIKU_REMOTE_HAVE_WSS when it finds
// it). quic:// needs OpenSSL 3.5 or newer, the release the broker's QUIC front
// door uses and the only one the QUIC transport has been tested with (3.2's
// client API has most of what it needs, but that is unverified);
// HAIKU_REMOTE_HAVE_QUIC says whether this build has it.
#ifdef HAIKU_REMOTE_HAVE_WSS
#include <openssl/opensslv.h>
#if OPENSSL_VERSION_NUMBER >= 0x30500000L
#define HAIKU_REMOTE_HAVE_QUIC 1
#endif
#endif

namespace haiku_remote {

#ifdef HAIKU_REMOTE_HAVE_WSS
inline constexpr bool build_supports_wss = true;
#else
inline constexpr bool build_supports_wss = false;
#endif

#ifdef HAIKU_REMOTE_HAVE_QUIC
inline constexpr bool build_supports_quic = true;
#else
inline constexpr bool build_supports_quic = false;
#endif

} // namespace haiku_remote
