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

private:
#ifdef _WIN32
    std::uintptr_t socket_ = static_cast<std::uintptr_t>(~0ull);
#else
    int socket_ = -1;
#endif
};

} // namespace haiku_remote
