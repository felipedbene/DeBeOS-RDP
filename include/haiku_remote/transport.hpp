#pragma once

#include "haiku_remote/known_brokers.hpp"

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
    // Trust on first use (known_brokers): the broker presented a certificate
    // with no stored entry for its host:port. The TLS handshake completed but
    // nothing was sent over it. broker_check() carries the fingerprint so the
    // caller can ask the user and reconnect.
    broker_unknown,
    // The broker presented a certificate that does not match the stored entry
    // for its host:port. broker_check() carries both fingerprints.
    broker_changed,
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
// The broker's certificate is not trusted: unknown (and nobody accepted it) or
// changed since it was recorded in known_brokers. Nothing was sent over the
// connection. Distinct because the remedy is a trust decision, not a retry.
constexpr int untrusted_broker = 4;
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

    // True when the last receive() failure was the peer *resetting* the
    // connection rather than closing it cleanly. app_server leaves a reset when
    // it tears a connection down with the client's pipelined bytes unread --
    // the signature of a refused or evicted candidate -- so a reset is the
    // server saying "go away", distinct from a clean FIN when a tunnel drops.
    // The reconnect policy must not retry a reset; it may retry a clean drop.
    // Defaults to false so a transport with no notion of a reset (the broker
    // path, where an eviction surfaces differently) is simply never treated as
    // one.
    [[nodiscard]] virtual bool connection_reset() const { return false; }

    // A human-readable endpoint description for window titles and logs.
    [[nodiscard]] virtual std::string describe() const = 0;

    // Why the last connect() failed. Only meaningful after connect() returned
    // false; every implementation sets it on the way out.
    [[nodiscard]] ConnectFailure connect_failure() const { return failure_; }

    // The trust-on-first-use verdict of the last connect(), meaningful when
    // connect_failure() is broker_unknown or broker_changed (and, with state
    // known, after a successful TOFU-verified connect).
    [[nodiscard]] const BrokerCheck& broker_check() const { return broker_check_; }

protected:
    Transport() = default;

    ConnectFailure failure_ = ConnectFailure::none;
    BrokerCheck broker_check_;
};

// The exit code a front end should return when connect() failed on `transport`.
[[nodiscard]] inline int connect_exit_status(const Transport& transport)
{
    switch (transport.connect_failure()) {
    case ConnectFailure::missing_credential:
        return exit_status::no_credential;
    case ConnectFailure::broker_unknown:
    case ConnectFailure::broker_changed:
        return exit_status::untrusted_broker;
    default:
        return exit_status::failed;
    }
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

    // How a wss:// broker is authenticated, in order of precedence:
    //
    //   1. --insecure: no chain, name or known_brokers check (testing only;
    //      the one blanket bypass). A pin given alongside is still enforced.
    //   2. --pin-sha256: the certificate's SHA-256 must equal the pin. The
    //      known_brokers store is neither read nor written.
    //   3. --ca-file: chain verification against that anchor plus the host
    //      name (SSL_set1_host).
    //   4. Otherwise, trust on first use against known_brokers: a matching
    //      entry proceeds; an unknown or changed certificate fails connect()
    //      with ConnectFailure::broker_unknown / broker_changed and the caller
    //      decides (resolve_broker_trust).

    // Certificate pinning: the broker certificate's SHA-256 fingerprint, as
    // hex (broker.fingerprint's exact content; an optional "sha256:" prefix
    // and colon separators are tolerated) or base64. When set, the pin alone
    // authenticates the server, so the broker's self-signed certificate needs
    // no CA. Ignored by raw TCP.
    std::string pin_sha256;

    // PEM trust anchor for chain + host-name verification. Ignored when a pin
    // is set.
    std::string ca_file;

    // Skip all server authentication (testing only).
    bool insecure = false;

    // The known_brokers store used by trust on first use. Empty means
    // KnownBrokers::default_file() (beside connections.json).
    std::string known_brokers_file;

    // Pre-seed: before the TOFU lookup, record this fingerprint for the
    // broker's host:port if the store has no entry for it yet. Never replaces
    // an existing entry, so it cannot hide a changed certificate.
    std::string known_broker_fingerprint;

    // --trust-new-broker: accept and record an UNKNOWN certificate without
    // asking (StrictHostKeyChecking=accept-new). Never accepts a changed one.
    // Consumed by connect_with_broker_trust(), not by the transport itself.
    bool trust_new_broker = false;
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

// The policy a command-line frontend uses: --trust-new-broker from `options`,
// and an ssh-style yes/no prompt on the terminal when stdin is a TTY (no
// prompt otherwise, so a non-interactive run refuses rather than hangs).
[[nodiscard]] TrustPolicy terminal_trust_policy(const TransportOptions& options);

// connect(), and if the broker's certificate is unknown or changed, resolve it
// with `policy`, record the decision in known_brokers, and connect once more --
// the reconnect re-verifies against the store, so what is accepted is exactly
// what was shown. Returns false with `error` (and the transport's
// connect_failure()) when refused. Other transports simply connect().
bool connect_with_broker_trust(Transport& transport, const TrustPolicy& policy,
                               std::string& error);

// Compute the SHA-256 fingerprint of a PEM certificate (what the broker writes
// to broker.fingerprint). False without TLS support or for an unparsable PEM.
bool fingerprint_pem_certificate(const std::string& pem, Fingerprint& out,
                                 std::string& error);

// Creates the transport selected by `options` without connecting it. Returns
// nullptr and sets `error` when the URL is malformed or names an unsupported
// scheme (including wss:// in a build without TLS support).
std::unique_ptr<Transport> make_transport(const TransportOptions& options,
                                          std::string& error);

} // namespace haiku_remote
