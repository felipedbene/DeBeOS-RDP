#include "haiku_remote/tcp_socket.hpp"

#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace haiku_remote {

TcpSocket::TcpSocket()
{
#ifdef _WIN32
    WSADATA data {};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

TcpSocket::~TcpSocket()
{
    close();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool TcpSocket::connect(std::string_view host, std::uint16_t port, std::string& error)
{
    close();
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto port_text = std::to_string(port);
    const std::string host_text(host);
    const int status = getaddrinfo(host_text.c_str(), port_text.c_str(), &hints, &addresses);
    if (status != 0) {
#ifdef _WIN32
        error = "getaddrinfo failed: " + std::to_string(status);
#else
        error = gai_strerror(status);
#endif
        return false;
    }
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        const auto candidate = ::socket(address->ai_family, address->ai_socktype,
                                        address->ai_protocol);
#ifdef _WIN32
        if (candidate == INVALID_SOCKET)
            continue;
#else
        if (candidate < 0)
            continue;
#endif
#ifdef _WIN32
        const int connected = ::connect(
            candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
#else
        const int connected = ::connect(
            candidate, address->ai_addr,
            static_cast<socklen_t>(address->ai_addrlen));
#endif
        if (connected == 0) {
            int no_delay = 1;
#ifdef _WIN32
            setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&no_delay),
                       static_cast<int>(sizeof(no_delay)));
#else
            setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY,
                       &no_delay, sizeof(no_delay));
#ifdef SO_NOSIGPIPE
            setsockopt(candidate, SOL_SOCKET, SO_NOSIGPIPE,
                       &no_delay, sizeof(no_delay));
#endif
#endif
            socket_ = candidate;
            freeaddrinfo(addresses);
            return true;
        }
#ifdef _WIN32
        closesocket(candidate);
#else
        ::close(candidate);
#endif
    }
    freeaddrinfo(addresses);
    error = "could not connect to any resolved address";
    return false;
}

bool TcpSocket::send_all(std::span<const std::uint8_t> bytes, std::string& error)
{
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const int count = ::send(socket_,
            reinterpret_cast<const char*>(bytes.data() + sent),
            static_cast<int>(bytes.size() - sent), send_flags);
#ifndef _WIN32
        if (count < 0 && errno == EINTR)
            continue;
#endif
        if (count <= 0) {
            error = "socket send failed";
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

int TcpSocket::receive(std::span<std::uint8_t> destination, int timeout_ms,
                       std::string& error)
{
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(static_cast<SOCKET>(socket_), &set);
    timeval timeout {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int ready = select(0, &set, nullptr, nullptr, &timeout);
#else
    pollfd descriptor {socket_, POLLIN, 0};
    int ready = 0;
    do {
        ready = poll(&descriptor, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
#endif
    if (ready == 0)
        return 0;
    if (ready < 0) {
        error = "socket wait failed";
        return -1;
    }
    int count = 0;
    do {
        count = ::recv(socket_, reinterpret_cast<char*>(destination.data()),
                       static_cast<int>(destination.size()), 0);
#ifdef _WIN32
        break;
#endif
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
        error = "socket receive failed";
        return -1;
    }
    if (count == 0) {
        // Orderly shutdown, not a failure. The caller still gets -1 so every
        // existing read loop stops, but peer_closed() now lets it tell the two
        // apart -- a capture that ends this way is complete, not lost.
        error = "connection closed by peer";
        peer_closed_ = true;
        return -1;
    }
    return count;
}

void TcpSocket::close()
{
#ifdef _WIN32
    if (socket_ != static_cast<std::uintptr_t>(INVALID_SOCKET)) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = static_cast<std::uintptr_t>(INVALID_SOCKET);
    }
#else
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
#endif
}

} // namespace haiku_remote
