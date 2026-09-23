#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace haiku_remote {

// Why a connect() attempt failed, for callers that have to act differently on
// the two refusals instead of parsing the message. The distinction that matters
// is local-versus-remote: a credential this client was never given is a
// misinvocation here, fixed by passing --cookie-file or --token-file, and no
// socket was ever opened; everything else happened out on the wire and the
// remedy is somewhere else (a stale cookie, a broker verdict, a dead listener).
enum class ConnectFailure {
    none,
    // The transport requires a credential and was handed none. Refused before
    // any socket is opened, so nothing reached the server at all.
    missing_credential,
    // Anything else: name resolution, the socket, TLS, the upgrade, the
    // broker's answer, a cookie the protocol cannot carry.
    other,
};

// Process exit codes, shared by every front end so a harness can read one
// scheme. `failed` and `usage` keep the values they have always had -- callers
// that test for non-zero, or for 1 and 2 specifically, are unaffected -- and the
// new case gets a new number rather than displacing one of them.
namespace exit_status {
constexpr int ok = 0;
// The session failed, or the server refused it. A wrong or stale cookie lands
// here: the socket opens, the gate drops it on the wire, and nothing is drawn.
constexpr int failed = 1;
// Bad arguments, or anything else that threw before the session began.
constexpr int usage = 2;
// No credential was supplied for a connection that requires one, so no socket
// was opened. Distinct from `failed` because the fix is on this side.
constexpr int no_credential = 3;
} // namespace exit_status

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

    // True when the last receive() failure was the peer closing the stream in
    // an orderly way rather than a transport error. The two are not the same
    // outcome: app_server's RemoteHWInterface::_Disconnect() sends
    // RP_CLOSE_CONNECTION and closes the endpoint on shutdown, so a session
    // that ends this way delivered every pixel it was ever going to deliver.
    // Folding it into "error" is how a complete capture gets thrown away.
    [[nodiscard]] virtual bool peer_closed() const = 0;

    // A human-readable endpoint description for window titles and logs.
    [[nodiscard]] virtual std::string describe() const = 0;

    // Why the last connect() failed. Only meaningful after connect() returned
    // false; every implementation sets it on the way out.
    [[nodiscard]] ConnectFailure connect_failure() const { return failure_; }

protected:
    Transport() = default;

    ConnectFailure failure_ = ConnectFailure::none;
};

// The exit code a front end should return when connect() failed on `transport`.
[[nodiscard]] inline int connect_exit_status(const Transport& transport)
{
    return transport.connect_failure() == ConnectFailure::missing_credential
        ? exit_status::no_credential
        : exit_status::failed;
}

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

    // app_server's per-boot session cookie (the content of
    // <system settings>/remote_desktop/session_cookie.<listen port>, which is
    // mode 0600 and readable only by the user app_server runs as). It is the
    // mandatory first frame of a *direct* connection to the session port: the
    // candidate gate in NetReceiver reads exactly that frame and drops any
    // connection that opens with anything else, so a direct transport without
    // one cannot open a session at all. Required by the raw TCP transport;
    // refused for ws:// and wss://, where the broker reads the file itself and
    // presents its own cookie frame.
    std::string cookie;

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

// The exit-code table, for --help output. Shared for the same reason as
// transport_usage(): one documented scheme, not one per front end.
[[nodiscard]] std::string_view exit_status_usage();

// Creates the transport selected by `options` without connecting it. Returns
// nullptr and sets `error` when the URL is malformed or names an unsupported
// scheme (including wss:// in a build without TLS support).
std::unique_ptr<Transport> make_transport(const TransportOptions& options,
                                          std::string& error);

} // namespace haiku_remote
