#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace haiku_remote {

// A bidirectional byte stream carrying the RP_ protocol. Implementations:
//
// - TcpTransport: the classic raw TCP connection to app_server's remote
//   interface (loopback or an SSH tunnel).
// - WebSocketTransport: WebSocket over TLS (wss://) or plain (ws://) to the
//   DeBeOS remote-desktop broker, presenting a session token and optionally
//   pinning the broker certificate.
class Transport {
public:
    virtual ~Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    virtual bool connect(std::string& error) = 0;
    virtual bool send_all(std::span<const std::uint8_t> bytes, std::string& error) = 0;
    // Returns the number of bytes read, 0 on timeout, -1 on error/close.
    virtual int receive(std::span<std::uint8_t> destination, int timeout_ms,
                        std::string& error) = 0;
    virtual void close() = 0;

    // A human-readable endpoint description for window titles and logs.
    [[nodiscard]] virtual std::string describe() const = 0;

protected:
    Transport() = default;
};

struct TransportOptions {
    // When set, selects the transport by scheme: tcp://host[:port],
    // ws://host[:port][/path], wss://host[:port][/path]. When empty, host and
    // port select the classic raw TCP transport.
    std::string url;
    std::string host = "127.0.0.1";
    std::uint16_t port = 10900;

    // Broker authentication token (the content of the broker's `token`
    // settings file). Sent as RP_AUTHENTICATE, the mandatory first message on
    // the WebSocket; the broker proxies nothing until it answers
    // RP_AUTH_RESULT with success. Ignored by the raw TCP transport.
    std::string token;

    // Certificate pinning: the broker certificate's SHA-256 fingerprint, as
    // hex (broker.fingerprint's exact content; an optional "sha256:" prefix
    // and colon separators are tolerated) or base64. When set, the pin alone
    // authenticates the server, so the broker's self-signed certificate needs
    // no CA. Ignored by raw TCP.
    std::string pin_sha256;

    // Extra PEM trust anchor for chain verification (instead of, not in
    // addition to, the system store). Ignored when a pin is set.
    std::string ca_file;

    // Skip all server authentication (testing only).
    bool insecure = false;
};

// Shared command-line handling so every frontend accepts the same transport
// arguments. Returns true when the argument was consumed. `value` fetches the
// argument's value and may throw when it is missing.
bool parse_transport_argument(TransportOptions& options, std::string_view argument,
                              const std::function<std::string()>& value);

// One usage line per accepted argument, for --help output.
[[nodiscard]] std::string_view transport_usage();

// Creates the transport selected by `options` without connecting it. Returns
// nullptr and sets `error` when the URL is malformed or names an unsupported
// scheme (including wss:// in a build without TLS support).
std::unique_ptr<Transport> make_transport(const TransportOptions& options,
                                          std::string& error);

} // namespace haiku_remote
