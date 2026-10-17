#pragma once

// What every transport to the DeBeOS remote-desktop broker shares, whatever
// carries it: the RP_AUTHENTICATE preamble, the meaning of RP_AUTH_RESULT, and
// how the broker's certificate is trusted (pin, or known_brokers on first use).
// The WebSocket transport (wss://) and the QUIC transport (quic://) differ only
// in the bytes underneath; keeping these here is what stops the two from
// drifting apart on the parts that are security decisions.
//
// Built only with OpenSSL (HAIKU_REMOTE_HAVE_WSS).

#include "haiku_remote/known_brokers.hpp"
#include "haiku_remote/transport.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

// Broker transport-security preamble opcodes (RemoteMessage.h).
inline constexpr std::uint16_t rp_authenticate = 10;
inline constexpr std::uint16_t rp_auth_result = 11;
inline constexpr std::size_t rp_auth_result_size = 10;

// The broker answers a denied token only after an anti-brute-force delay.
inline constexpr int broker_auth_timeout_ms = 15000;

// The RP_AUTHENTICATE message: u16 code, u32 total length including the 6-byte
// header, then u32 method (1 = shared token), u32 token length, token bytes --
// all little-endian.
[[nodiscard]] std::vector<std::uint8_t> broker_authenticate_message(
    std::string_view token);

// Interprets a complete 10-byte RP_AUTH_RESULT. True on success; otherwise
// false with `error` saying what the status means and where the remedy is.
bool broker_auth_result(std::span<const std::uint8_t> reply, std::string& error);

// Decodes a pin given as hex or base64 into a SHA-256 digest. Accepts the
// broker.fingerprint format (lower/upper hex, optionally colon-separated), an
// optional "sha256:" prefix, and curl's "sha256//".
bool decode_certificate_pin(std::string_view pin,
                            std::array<std::uint8_t, 32>& digest,
                            std::string& error);

// What identifies the broker a transport is talking to and how it is to be
// trusted; filled from TransportOptions.
struct BrokerIdentity {
    std::string host;
    std::uint16_t port = 0;
    std::string pin_sha256;
    bool insecure = false;
    // When a CA file is configured the TLS layer has already verified the
    // chain and the name, so nothing is left to check here.
    bool chain_verified = false;
    std::string known_brokers_file;
    std::string known_broker_fingerprint;
};

// The trust decision for the certificate the broker presented, after the
// handshake: --insecure accepts; a pin must match; a verified chain accepts;
// otherwise known_brokers decides (trust on first use). On an unknown or
// changed certificate it returns false with `failure` set to broker_unknown /
// broker_changed and `check` filled for the caller's prompt.
bool verify_broker_certificate(const BrokerIdentity& identity,
                               const Fingerprint& presented, BrokerCheck& check,
                               ConnectFailure& failure, std::string& error);

} // namespace haiku_remote
