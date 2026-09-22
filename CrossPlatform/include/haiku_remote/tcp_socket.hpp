#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace haiku_remote {

class TcpSocket {
public:
    TcpSocket();
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool connect(std::string_view host, std::uint16_t port, std::string& error);
    bool send_all(std::span<const std::uint8_t> bytes, std::string& error);
    int receive(std::span<std::uint8_t> destination, int timeout_ms, std::string& error);
    void close();

    // True when receive() reported failure because the peer shut the stream
    // down cleanly (recv() returned 0) rather than because of a socket error.
    [[nodiscard]] bool peer_closed() const { return peer_closed_; }

    // The connected descriptor, for layering a TLS session on top of the
    // socket. Invalid (-1 / INVALID_SOCKET) before connect() succeeds.
#ifdef _WIN32
    [[nodiscard]] std::uintptr_t native_handle() const { return socket_; }
#else
    [[nodiscard]] int native_handle() const { return socket_; }
#endif

private:
#ifdef _WIN32
    std::uintptr_t socket_ = static_cast<std::uintptr_t>(~0ull);
#else
    int socket_ = -1;
#endif
    bool peer_closed_ = false;
};

} // namespace haiku_remote
