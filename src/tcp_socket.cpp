#include "haiku_remote/tcp_socket.hpp"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace haiku_remote {

namespace {

// How long one address may take to accept the connection. Without a bound, a
// path that drops SYNs (a firewall, an unroutable private address) holds
// connect() for the kernel's whole retry schedule -- about two minutes on
// Linux -- and an auto-mode fallback behind it waits all that time.
constexpr int connect_timeout_ms = 10000;

#ifdef _WIN32
using Native = SOCKET;
#else
using Native = int;
#endif

bool connect_bounded(Native candidate, const sockaddr* address, std::size_t length)
{
#ifdef _WIN32
    u_long non_blocking = 1;
    ioctlsocket(candidate, FIONBIO, &non_blocking);
    int connected = ::connect(candidate, address, static_cast<int>(length));
    if (connected != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(candidate, &writable);
        FD_SET(candidate, &failed);
        timeval timeout {connect_timeout_ms / 1000, (connect_timeout_ms % 1000) * 1000};
        int socket_error = 0;
        int error_length = sizeof(socket_error);
        if (select(0, nullptr, &writable, &failed, &timeout) == 1
            && FD_ISSET(candidate, &writable)
            && getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                          reinterpret_cast<char*>(&socket_error), &error_length) == 0
            && socket_error == 0)
            connected = 0;
    }
    non_blocking = 0;
    ioctlsocket(candidate, FIONBIO, &non_blocking);
    return connected == 0;
#else
    const int flags = fcntl(candidate, F_GETFL, 0);
    fcntl(candidate, F_SETFL, flags | O_NONBLOCK);
    int connected = ::connect(candidate, address, static_cast<socklen_t>(length));
    if (connected != 0 && errno == EINPROGRESS) {
        pollfd descriptor {candidate, POLLOUT, 0};
        int ready = 0;
        do {
            ready = poll(&descriptor, 1, connect_timeout_ms);
        } while (ready < 0 && errno == EINTR);
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (ready == 1
            && getsockopt(candidate, SOL_SOCKET, SO_ERROR, &socket_error,
                          &error_length) == 0
            && socket_error == 0)
            connected = 0;
    }
    fcntl(candidate, F_SETFL, flags);
    return connected == 0;
#endif
}

} // namespace

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
    // A reused socket must not carry the previous connection's disposition into
    // the new one: the reconnect policy reads these after the next receive().
    peer_closed_ = false;
    connection_reset_ = false;
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
        {
            // Published before the blocking connect, so abort() can reach it.
            std::lock_guard<std::mutex> lock(mutex_);
            if (aborted_) {
#ifdef _WIN32
                closesocket(candidate);
#else
                ::close(candidate);
#endif
                freeaddrinfo(addresses);
                error = "connect cancelled";
                return false;
            }
            socket_ = candidate;
        }
        // Bounded per address (connect_bounded); abort()'s shutdown() wakes
        // the poll inside it, as it woke the blocking connect.
        const bool connected
            = connect_bounded(candidate, address->ai_addr, address->ai_addrlen);
        if (connected) {
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
            freeaddrinfo(addresses);
            std::lock_guard<std::mutex> lock(mutex_);
            if (aborted_) {
                // Connected just as it was cancelled: the descriptor is
                // already shut down (or, on Windows, closed); close() or the
                // destructor releases what is left.
                error = "connect cancelled";
                return false;
            }
            return true;
        }
        std::lock_guard<std::mutex> lock(mutex_);
#ifdef _WIN32
        // abort() closes the socket itself on Windows and clears socket_.
        if (socket_ == static_cast<std::uintptr_t>(candidate))
            closesocket(candidate);
        socket_ = static_cast<std::uintptr_t>(INVALID_SOCKET);
#else
        ::close(candidate);
        socket_ = -1;
#endif
        if (aborted_) {
            freeaddrinfo(addresses);
            error = "connect cancelled";
            return false;
        }
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
#ifdef _WIN32
        error = "socket wait failed: " + std::to_string(WSAGetLastError());
#else
        error = std::string("socket wait failed: ") + std::strerror(errno);
#endif
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
        // Name the reason. "socket receive failed" was the whole diagnostic for
        // every failure mode, and the two that matter read very differently to
        // an operator: a reset means the peer tore the connection down with our
        // bytes still unread -- which is what a server that refuses a session
        // without draining it produces -- while anything else is a genuine
        // local or network fault. peer_closed_ deliberately stays false here:
        // a reset is not an orderly shutdown, and callers rely on that
        // distinction.
#ifdef _WIN32
        const int code = WSAGetLastError();
        connection_reset_ = code == WSAECONNRESET;
        error = connection_reset_
            ? "connection reset by peer"
            : "socket receive failed: " + std::to_string(code);
#else
        connection_reset_ = errno == ECONNRESET;
        error = connection_reset_
            ? std::string("connection reset by peer")
            : std::string("socket receive failed: ") + std::strerror(errno);
#endif
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

void TcpSocket::abort()
{
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
#ifdef _WIN32
    if (socket_ != static_cast<std::uintptr_t>(INVALID_SOCKET)) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = static_cast<std::uintptr_t>(INVALID_SOCKET);
    }
#else
    // Not close(): the owner's thread may be inside connect() or poll() on
    // this descriptor, and a closed number can be reused under it. shutdown()
    // wakes both (a SYN_SENT socket is disconnected) and the owner closes.
    if (socket_ >= 0)
        ::shutdown(socket_, SHUT_RDWR);
#endif
}

void TcpSocket::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
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
