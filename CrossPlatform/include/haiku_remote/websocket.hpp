#pragma once

#include "haiku_remote/tcp_socket.hpp"
#include "haiku_remote/transport.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace haiku_remote {

// RFC 6455 WebSocket client transport, optionally over TLS (wss://), carrying
// the RP_ byte stream in binary frames. Each send_all() call — one RP_ message
// from the session — becomes one masked binary frame; received binary frame
// payloads are concatenated back into the byte stream the Framer consumes, so
// the server may split or batch RP_ messages across frames freely.
class WebSocketTransport final : public Transport {
public:
    WebSocketTransport(std::string host, std::uint16_t port, std::string target,
                       bool secure, const TransportOptions& options);
    ~WebSocketTransport() override;

    bool connect(std::string& error) override;
    bool send_all(std::span<const std::uint8_t> bytes, std::string& error) override;
    int receive(std::span<std::uint8_t> destination, int timeout_ms,
                std::string& error) override;
    void close() override;
    [[nodiscard]] std::string describe() const override;

private:
    struct Tls;

    bool tls_connect(std::string& error);
    bool verify_pin(std::string& error);
    bool upgrade(std::string& error);
    bool authenticate(std::string& error);
    bool raw_send(std::span<const std::uint8_t> bytes, std::string& error);
    // Reads whatever is available within timeout_ms into frame_buffer_.
    // Returns bytes read, 0 on timeout, -1 on error/close.
    int raw_receive(int timeout_ms, std::string& error);
    bool send_frame(std::uint8_t opcode, std::span<const std::uint8_t> payload,
                    std::string& error);
    // Consumes complete frames from frame_buffer_ into incoming_, answering
    // control frames. Returns false (with error) on protocol error or close.
    bool drain_frames(std::string& error);

    std::string host_;
    std::uint16_t port_;
    std::string target_; // request path including any query
    bool secure_;
    std::string token_;
    std::string pin_sha256_;
    std::string ca_file_;
    bool insecure_;

    TcpSocket socket_;
    Tls* tls_ = nullptr;
    bool open_ = false;
    bool peer_closed_ = false;
    std::vector<std::uint8_t> frame_buffer_; // raw bytes, not yet deframed
    std::vector<std::uint8_t> incoming_;     // decoded application bytes
};

} // namespace haiku_remote
