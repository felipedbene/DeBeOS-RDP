#pragma once

// quic:// is built against OpenSSL 3.5 or newer; build_features.hpp decides
// (HAIKU_REMOTE_HAVE_QUIC).
#include "haiku_remote/build_features.hpp"

#include "haiku_remote/transport.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace haiku_remote {

// The ALPN of the broker's QUIC front door (DeBeOS #766). The "/1" names the
// stream layout -- one client-initiated bidirectional stream carrying exactly
// what a wss:// connection carries, minus the WebSocket framing -- not the
// remote protocol, whose features RP_HELLO negotiates.
inline constexpr std::string_view quic_alpn = "debeos-urp/1";

// The broker listens for QUIC on its own TCP port number, over UDP.
inline constexpr std::uint16_t quic_default_port = 10902;

// QUIC transport to the DeBeOS remote-desktop broker (quic://host[:port]).
// Same token, same certificate trust (pin, --ca-file, or known_brokers on first
// use), same session behind it as wss://; what changes is TLS 1.3 over UDP
// instead of TLS over TCP plus WebSocket framing.
//
// Built with OpenSSL 3.5 or newer (see above). The connection is driven from
// this object's own calls (receive() waits on the socket and services QUIC's
// timers), so the session loop that already calls receive() with a timeout
// keeps it alive.
class QuicTransport final : public Transport {
public:
    QuicTransport(std::string host, std::uint16_t port,
                  const TransportOptions& options);
    ~QuicTransport() override;

    bool connect(std::string& error) override;
    // Thread-safe: ends a connect() blocked in the handshake or waiting for
    // the broker's RP_AUTH_RESULT, from another thread (a user's Cancel).
    void abort() override;
    bool send_all(std::span<const std::uint8_t> bytes, std::string& error) override;
    int receive(std::span<std::uint8_t> destination, int timeout_ms,
                std::string& error) override;
    void close() override;
    // The broker's FIN on the stream, or a close carrying application error 0,
    // is the orderly end of the session.
    [[nodiscard]] bool peer_closed() const override { return peer_closed_; }
    [[nodiscard]] std::string describe() const override;

private:
    struct Quic;

    bool handshake(std::string& error);
    bool open_stream(std::string& error);
    bool authenticate(std::string& error);
    // Reads whatever the stream has within timeout_ms into incoming_.
    // Returns bytes read, 0 on timeout, -1 on end of stream or error.
    int read_some(int timeout_ms, std::string& error);
    // Waits up to timeout_ms for the socket (or QUIC's next timer), then lets
    // the stack process what arrived.
    void service(int timeout_ms);
    // Notes how the connection ended, for peer_closed() and the error text.
    void note_close(std::string& error);

    std::string host_;
    std::uint16_t port_;
    std::string token_;
    std::string pin_sha256_;
    std::string ca_file_;
    bool insecure_;
    std::string known_brokers_file_;
    std::string known_broker_fingerprint_;
    int handshake_timeout_ms_;

    // Guards quic_ (creation, deletion and its socket) against abort().
    std::mutex abort_mutex_;
    std::atomic<bool> aborted_ {false};
    // A datagram arrived during the handshake: tells a silent path
    // (quic_no_answer) from a handshake that started and stalled.
    bool datagram_seen_ = false;
    Quic* quic_ = nullptr;
    bool open_ = false;
    bool peer_closed_ = false;
    std::vector<std::uint8_t> incoming_;
};

} // namespace haiku_remote
