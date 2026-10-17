#include "haiku_remote/quic.hpp"

#ifdef HAIKU_REMOTE_HAVE_QUIC

#include "haiku_remote/broker_handshake.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace haiku_remote {
namespace {

constexpr int default_handshake_timeout_ms = 10000;

// QUIC's CONNECTION_CLOSE carries a TLS alert as CRYPTO_ERROR 0x100 + alert
// (RFC 9001 4.8). The broker's ALPN callback refuses anything but
// debeos-urp/1 with SSL_TLSEXT_ERR_ALERT_FATAL (QuicStream.cpp select_alpn),
// which OpenSSL sends as no_application_protocol (120, RFC 7301 3.2).
constexpr std::uint64_t crypto_error_no_application_protocol = 0x100 + 120;

#ifdef _WIN32
// abort() cannot be relied on to wake a select() on a UDP socket on Windows, so
// no wait there lasts longer than this: it bounds how late a Cancel is seen,
// never how late data is (a datagram ends the wait at once).
constexpr int abort_poll_slice_ms = 100;
#endif
constexpr int write_timeout_ms = 10000;
constexpr std::size_t max_buffered_bytes = 256 * 1024 * 1024;

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
void close_native(NativeSocket handle) { closesocket(handle); }
#else
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;
void close_native(NativeSocket handle) { ::close(handle); }
#endif

std::string openssl_error(std::string_view context)
{
    std::ostringstream text;
    text << context;
    const unsigned long code = ERR_get_error();
    if (code != 0) {
        char buffer[256];
        ERR_error_string_n(code, buffer, sizeof(buffer));
        text << ": " << buffer;
    }
    ERR_clear_error();
    return text.str();
}

// On Haiku the client needs DeBeOS's openssl3 >= 3.5.9-2 (Haiku-Graviton
// #790): earlier builds strip the sign of Haiku's negative errno in
// ERR_GET_REASON(), so BIO_err_is_non_fatal() takes EAGAIN for a dead network
// and no QUIC handshake completes. 3.5.9-1 and 3.5.9-2 share a version
// number, so connect() checks the behaviour itself and says so, rather than
// timing out against a broker that is fine.
bool errno_sign_survives()
{
    ERR_set_mark();
    ERR_raise(ERR_LIB_SYS, EAGAIN);
    const unsigned long packed = ERR_peek_last_error();
    const bool intact = ERR_GET_REASON(packed) == EAGAIN
        && BIO_err_is_non_fatal(static_cast<unsigned int>(packed)) == 1;
    ERR_pop_to_mark();
    return intact;
}


int milliseconds_until(std::chrono::steady_clock::time_point deadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return remaining <= 0 ? 0 : static_cast<int>(std::min<long long>(remaining, INT_MAX));
}

} // namespace

struct QuicTransport::Quic {
    SSL_CTX* context = nullptr;
    SSL* connection = nullptr;
    SSL* stream = nullptr;
    NativeSocket socket = invalid_socket;
#ifdef _WIN32
    bool winsock = false;
#endif

    ~Quic()
    {
        if (stream != nullptr)
            SSL_free(stream);
        if (connection != nullptr)
            SSL_free(connection);
        if (context != nullptr)
            SSL_CTX_free(context);
        if (socket != invalid_socket)
            close_native(socket);
#ifdef _WIN32
        if (winsock)
            WSACleanup();
#endif
    }
};

QuicTransport::QuicTransport(std::string host, std::uint16_t port,
                             const TransportOptions& options)
    : host_(std::move(host))
    , port_(port)
    , token_(options.token)
    , pin_sha256_(options.pin_sha256)
    , ca_file_(options.ca_file)
    , insecure_(options.insecure)
    , known_brokers_file_(options.known_brokers_file)
    , known_broker_fingerprint_(options.known_broker_fingerprint)
    , handshake_timeout_ms_(options.quic_handshake_timeout_ms > 0
                                ? options.quic_handshake_timeout_ms
                                : default_handshake_timeout_ms)
{
}

QuicTransport::~QuicTransport()
{
    close();
}

std::string QuicTransport::describe() const
{
    return "quic://" + host_ + ":" + std::to_string(port_);
}

bool QuicTransport::connect(std::string& error)
{
    close();
    failure_ = ConnectFailure::other;
    broker_check_ = BrokerCheck {};
    // Refused before any packet, for the reason WebSocketTransport::connect()
    // gives: the broker proxies nothing without RP_AUTHENTICATE, and saying
    // what is missing beats an opaque close from the far side.
    if (token_.empty()) {
        failure_ = ConnectFailure::missing_credential;
        error = "no authentication token: a broker connection requires"
                " --token or --token-file (the broker's own token file; the"
                " session cookie is the broker's to read, not this client's)";
        return false;
    }
    if (aborted_) {
        error = "connect cancelled";
        return false;
    }
    if (!errno_sign_survives()) {
        error = "this OpenSSL strips the sign of the platform's errno, so QUIC"
                " cannot work over it (on DeBeOS: openssl3 >= 3.5.9-2 is"
                " required, Haiku-Graviton #790)";
        return false;
    }
    if (!handshake(error) || !open_stream(error)) {
        close();
        return false;
    }
    open_ = true;
    if (!authenticate(error)) {
        close();
        return false;
    }
    failure_ = ConnectFailure::none;
    return true;
}

bool QuicTransport::handshake(std::string& error)
{
    {
        std::lock_guard<std::mutex> lock(abort_mutex_);
        quic_ = new Quic;
    }
    datagram_seen_ = false;
#ifdef _WIN32
    WSADATA data {};
    quic_->winsock = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif

    // IPv4 only, like the broker's listener.
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* addresses = nullptr;
    const std::string port_text = std::to_string(port_);
    const int status = getaddrinfo(host_.c_str(), port_text.c_str(), &hints,
                                   &addresses);
    if (status != 0 || addresses == nullptr) {
        error = "could not resolve " + host_ + " (the QUIC front door is IPv4"
                " only)";
        return false;
    }
    sockaddr_in server {};
    std::memcpy(&server, addresses->ai_addr, sizeof(server));
    freeaddrinfo(addresses);

    {
        // Published under the lock, so abort() can shut it down from the
        // cancelling thread; a cancel that came first wins here.
        std::lock_guard<std::mutex> lock(abort_mutex_);
        quic_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (aborted_) {
            error = "connect cancelled";
            return false;
        }
    }
    if (quic_->socket == invalid_socket) {
        error = "could not create a UDP socket";
        return false;
    }
    sockaddr_in local {};
    local.sin_family = AF_INET;
    if (::bind(quic_->socket, reinterpret_cast<const sockaddr*>(&local),
               sizeof(local)) != 0) {
        error = "could not bind a UDP socket";
        return false;
    }

    // The plain client method, driven by this object's own calls. Not the
    // thread-assisted one: with it, OpenSSL 3.5's client stops taking
    // server-to-client data after ~5.8 kB under load (DeBeOS #766, reproduced
    // on 3.5.7 and 3.5.8; openssl s_client, which uses this method, does not).
    quic_->context = SSL_CTX_new(OSSL_QUIC_client_method());
    if (quic_->context == nullptr) {
        error = openssl_error("SSL_CTX_new(QUIC) failed");
        return false;
    }

    const bool verify_chain = pin_sha256_.empty() && !insecure_ && !ca_file_.empty();
    if (verify_chain) {
        if (SSL_CTX_load_verify_locations(quic_->context, ca_file_.c_str(),
                                          nullptr) != 1) {
            error = openssl_error("could not load --ca-file " + ca_file_);
            return false;
        }
        SSL_CTX_set_verify(quic_->context, SSL_VERIFY_PEER, nullptr);
    } else {
        // Not "no verification": the decision is made after the handshake,
        // by pin or by known_brokers (verify_broker_certificate()).
        SSL_CTX_set_verify(quic_->context, SSL_VERIFY_NONE, nullptr);
    }

    quic_->connection = SSL_new(quic_->context);
    if (quic_->connection == nullptr) {
        error = openssl_error("SSL_new failed");
        return false;
    }
    SSL_set_tlsext_host_name(quic_->connection, host_.c_str());
    if (verify_chain && SSL_set1_host(quic_->connection, host_.c_str()) != 1) {
        error = "could not configure host name verification";
        return false;
    }

    BIO* network = BIO_new_dgram(static_cast<int>(quic_->socket), BIO_NOCLOSE);
    if (network == nullptr) {
        error = openssl_error("could not create the datagram BIO");
        return false;
    }
    SSL_set_bio(quic_->connection, network, network);

    std::vector<unsigned char> alpn;
    alpn.push_back(static_cast<unsigned char>(quic_alpn.size()));
    alpn.insert(alpn.end(), quic_alpn.begin(), quic_alpn.end());
    if (SSL_set_alpn_protos(quic_->connection, alpn.data(),
                            static_cast<unsigned int>(alpn.size())) != 0) {
        error = "could not configure ALPN";
        return false;
    }

    BIO_ADDR* peer = BIO_ADDR_new();
    if (peer == nullptr
        || !BIO_ADDR_rawmake(peer, AF_INET, &server.sin_addr,
                             sizeof(server.sin_addr), server.sin_port)
        || !SSL_set1_initial_peer_addr(quic_->connection, peer)) {
        BIO_ADDR_free(peer);
        error = openssl_error("could not set the broker address");
        return false;
    }
    BIO_ADDR_free(peer);

    if (!SSL_set_default_stream_mode(quic_->connection,
                                     SSL_DEFAULT_STREAM_MODE_NONE)
        || !SSL_set_blocking_mode(quic_->connection, 0)) {
        error = openssl_error("could not configure the QUIC connection");
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(handshake_timeout_ms_);
    while (true) {
        const int result = SSL_connect(quic_->connection);
        if (result == 1)
            break;
        if (aborted_) {
            ERR_clear_error();
            error = "connect cancelled";
            return false;
        }
        const int code = SSL_get_error(quic_->connection, result);
        if (code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE) {
            SSL_CONN_CLOSE_INFO info {};
            if (SSL_get_conn_close_info(quic_->connection, &info, sizeof(info)) == 1
                && (info.flags & SSL_CONN_CLOSE_FLAG_TRANSPORT) != 0
                && (info.flags & SSL_CONN_CLOSE_FLAG_LOCAL) == 0
                && info.error_code == crypto_error_no_application_protocol) {
                ERR_clear_error();
                failure_ = ConnectFailure::quic_alpn_refused;
                error = "the broker at " + host_ + ":" + port_text + " refused ALPN "
                    + std::string(quic_alpn)
                    + " (TLS alert no_application_protocol): it speaks a"
                      " different QUIC stream protocol than this client";
                return false;
            }
            error = openssl_error("QUIC handshake with " + host_ + " failed"
                                  " (is the broker running with -q?)");
            return false;
        }
        const int remaining = milliseconds_until(deadline);
        if (remaining == 0) {
            if (datagram_seen_) {
                // Also what a refused ALPN looks like from an OpenSSL 3.5
                // server (the broker's stack): its ALPN callback fails the
                // handshake, but it sends only an ACK, no CONNECTION_CLOSE
                // (seen against 3.5.8 with SSL_set_msg_callback), so the
                // no_application_protocol branch above never sees an alert.
                failure_ = ConnectFailure::quic_handshake_timeout;
                error = "the QUIC handshake with " + host_ + ":" + port_text
                    + " started but did not complete within "
                    + std::to_string(handshake_timeout_ms_)
                    + " ms (a lossy path, a busy broker, or one that does not"
                      " speak " + std::string(quic_alpn) + ")";
            } else {
                failure_ = ConnectFailure::quic_no_answer;
                error = "no answer over UDP from " + host_ + ":" + port_text
                    + " within " + std::to_string(handshake_timeout_ms_)
                    + " ms (UDP may be blocked, or the broker is not listening"
                      " for QUIC)";
            }
            return false;
        }
        service(remaining);
    }

    X509* certificate = SSL_get1_peer_certificate(quic_->connection);
    if (certificate == nullptr) {
        error = "server presented no certificate to pin against";
        return false;
    }
    Fingerprint presented {};
    unsigned int digest_length = 0;
    const int digested
        = X509_digest(certificate, EVP_sha256(), presented.data(), &digest_length);
    X509_free(certificate);
    if (digested != 1 || digest_length != presented.size()) {
        error = openssl_error("could not fingerprint server certificate");
        return false;
    }

    BrokerIdentity identity;
    identity.host = host_;
    identity.port = port_;
    identity.pin_sha256 = pin_sha256_;
    identity.insecure = insecure_;
    identity.chain_verified = verify_chain;
    identity.known_brokers_file = known_brokers_file_;
    identity.known_broker_fingerprint = known_broker_fingerprint_;
    BrokerCheck check;
    const bool trusted
        = verify_broker_certificate(identity, presented, check, failure_, error);
    if (!check.host.empty())
        broker_check_ = check;
    return trusted;
}

bool QuicTransport::open_stream(std::string& error)
{
    // Client-initiated bidirectional stream 0: the only stream the broker
    // accepts. Opening anything else, or anything more, closes the connection.
    quic_->stream = SSL_new_stream(quic_->connection, 0);
    if (quic_->stream == nullptr) {
        error = openssl_error("could not open the QUIC stream");
        return false;
    }
    if (!SSL_set_blocking_mode(quic_->stream, 0)) {
        error = openssl_error("could not configure the QUIC stream");
        return false;
    }
    return true;
}

bool QuicTransport::authenticate(std::string& error)
{
    const std::vector<std::uint8_t> message = broker_authenticate_message(token_);
    if (!send_all(message, error))
        return false;

    // A denied attempt is answered only after the broker's anti-brute-force
    // delay. Session bytes pipelined behind the result stay in incoming_.
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(broker_auth_timeout_ms);
    while (incoming_.size() < rp_auth_result_size) {
        if (aborted_) {
            error = "connect cancelled";
            return false;
        }
        const int remaining = milliseconds_until(deadline);
        if (remaining == 0) {
            error = "timed out waiting for the broker's authentication result";
            return false;
        }
        std::string receive_error;
        if (read_some(remaining, receive_error) < 0
            && incoming_.size() < rp_auth_result_size) {
            error = "connection lost before the broker's authentication result"
                    " (" + receive_error + ")";
            return false;
        }
    }

    const bool accepted = broker_auth_result(
        std::span(incoming_.data(), rp_auth_result_size), error);
    incoming_.erase(incoming_.begin(),
                    incoming_.begin() + static_cast<std::ptrdiff_t>(rp_auth_result_size));
    return accepted;
}

void QuicTransport::service(int timeout_ms)
{
    // Never sleep past QUIC's own next deadline (loss recovery, ACK delay,
    // keep-alive): with the plain client method, nothing else handles it.
    timeval quic_timeout {};
    int infinite = 0;
    if (SSL_get_event_timeout(quic_->connection, &quic_timeout, &infinite) == 1
        && !infinite) {
        const long long quic_ms = static_cast<long long>(quic_timeout.tv_sec) * 1000
            + (quic_timeout.tv_usec + 999) / 1000;
        if (quic_ms < timeout_ms)
            timeout_ms = static_cast<int>(quic_ms);
    }

    if (timeout_ms > 0 && !aborted_) {
#ifdef _WIN32
        timeout_ms = std::min(timeout_ms, abort_poll_slice_ms);
        fd_set set;
        FD_ZERO(&set);
        FD_SET(quic_->socket, &set);
        timeval timeout {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(0, &set, nullptr, nullptr, &timeout) == 1
            && FD_ISSET(quic_->socket, &set))
            datagram_seen_ = true;
#else
        pollfd descriptor {quic_->socket, POLLIN, 0};
        if (SSL_net_write_desired(quic_->connection))
            descriptor.events |= POLLOUT;
        int ready = 0;
        do {
            ready = poll(&descriptor, 1, timeout_ms);
        } while (ready < 0 && errno == EINTR);
        // POLLIN on a socket abort() shut down is the shutdown, not a reply.
        if (ready > 0 && (descriptor.revents & POLLIN) != 0 && !aborted_)
            datagram_seen_ = true;
#endif
    }
    SSL_handle_events(quic_->connection);
    ERR_clear_error();
}

void QuicTransport::note_close(std::string& error)
{
    SSL_CONN_CLOSE_INFO info {};
    if (SSL_get_conn_close_info(quic_->connection, &info, sizeof(info)) == 1) {
        const bool transport = (info.flags & SSL_CONN_CLOSE_FLAG_TRANSPORT) != 0;
        if (!transport && info.error_code == 0) {
            peer_closed_ = true;
            error = "connection closed by peer";
            return;
        }
        // Application codes are the broker's (DeBeOS QuicStream.h): 1 the
        // relay ended (session port gone, a stalled write, or a newer client
        // took the session over), 2 a stream-rule violation.
        std::ostringstream text;
        text << "QUIC connection closed by the "
             << ((info.flags & SSL_CONN_CLOSE_FLAG_LOCAL) != 0 ? "client" : "broker")
             << " with " << (transport ? "transport" : "application")
             << " error " << info.error_code;
        if (info.reason != nullptr && info.reason_len > 0)
            text << " (" << std::string(info.reason, info.reason_len) << ")";
        error = text.str();
        return;
    }
    error = "QUIC stream ended";
}

int QuicTransport::read_some(int timeout_ms, std::string& error)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    std::uint8_t buffer[64 * 1024];
    while (true) {
        std::size_t read = 0;
        // A success that read nothing happens on DeBeOS's OpenSSL 3.5.7, in
        // bursts; it is neither data nor an end of stream, so the connection
        // is serviced and the read tried again.
        if (SSL_read_ex(quic_->stream, buffer, sizeof(buffer), &read) == 1
            && read > 0) {
            incoming_.insert(incoming_.end(), buffer, buffer + read);
            if (incoming_.size() > max_buffered_bytes) {
                error = "receive buffer overflow";
                return -1;
            }
            return static_cast<int>(read);
        }
        ERR_clear_error();
        switch (SSL_get_stream_read_state(quic_->stream)) {
        case SSL_STREAM_STATE_OK:
            break;
        case SSL_STREAM_STATE_FINISHED:
            // The broker's FIN: the orderly end of the session.
            peer_closed_ = true;
            error = "connection closed by peer";
            return -1;
        default:
            note_close(error);
            return -1;
        }
        const int remaining = milliseconds_until(deadline);
        if (remaining == 0)
            return 0;
        service(remaining);
    }
}

bool QuicTransport::send_all(std::span<const std::uint8_t> bytes,
                             std::string& error)
{
    if (quic_ == nullptr || quic_->stream == nullptr) {
        error = "QUIC stream is not open";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(write_timeout_ms);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        std::size_t written = 0;
        if (SSL_write_ex(quic_->stream, bytes.data() + sent, bytes.size() - sent,
                         &written) == 1) {
            sent += written;
            continue;
        }
        ERR_clear_error();
        if (SSL_get_stream_write_state(quic_->stream) != SSL_STREAM_STATE_OK) {
            note_close(error);
            return false;
        }
        const int remaining = milliseconds_until(deadline);
        if (remaining == 0) {
            error = "timed out sending to the broker";
            return false;
        }
        service(std::min(remaining, 50));
    }
    // Push it out now rather than at the next receive().
    SSL_handle_events(quic_->connection);
    ERR_clear_error();
    return true;
}

int QuicTransport::receive(std::span<std::uint8_t> destination, int timeout_ms,
                           std::string& error)
{
    if (!open_ || quic_ == nullptr) {
        error = "QUIC stream is not open";
        return -1;
    }
    if (incoming_.empty()) {
        const int count = read_some(timeout_ms, error);
        if (count < 0)
            return -1;
        if (incoming_.empty())
            return 0;
    }
    const std::size_t count = std::min(destination.size(), incoming_.size());
    std::copy_n(incoming_.begin(), count, destination.begin());
    incoming_.erase(incoming_.begin(),
                    incoming_.begin() + static_cast<std::ptrdiff_t>(count));
    return static_cast<int>(count);
}

void QuicTransport::abort()
{
    std::lock_guard<std::mutex> lock(abort_mutex_);
    aborted_ = true;
    // Not close: the connecting thread may be inside poll() or OpenSSL on this
    // descriptor, and a closed number can be reused under it. shutdown()
    // wakes poll() even on an unconnected UDP socket, and the connecting
    // thread sees aborted_ and gives up; it closes the socket itself.
    if (quic_ != nullptr && quic_->socket != invalid_socket) {
#ifdef _WIN32
        ::shutdown(quic_->socket, SD_BOTH);
#else
        ::shutdown(quic_->socket, SHUT_RDWR);
#endif
    }
}

void QuicTransport::close()
{
    if (quic_ != nullptr) {
        if (open_ && !peer_closed_ && quic_->connection != nullptr) {
            // The orderly end, as the broker expects it: a FIN on the stream,
            // then a close carrying application error 0. Bounded, since a
            // broker that has gone away will never acknowledge either.
            if (quic_->stream != nullptr)
                SSL_stream_conclude(quic_->stream, 0);
            SSL_SHUTDOWN_EX_ARGS args {};
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(500);
            while (SSL_shutdown_ex(quic_->connection, SSL_SHUTDOWN_FLAG_NO_BLOCK,
                                   &args, sizeof(args)) != 1) {
                const int remaining = milliseconds_until(deadline);
                if (remaining == 0)
                    break;
                service(std::min(remaining, 20));
            }
            ERR_clear_error();
        } else if (!open_ && quic_->connection != nullptr
                   && SSL_is_init_finished(quic_->connection)) {
            // connect() gave up after the handshake -- most often a
            // certificate this client does not trust. Say so at once rather
            // than leaving the broker holding a handshake slot until its 15 s
            // budget runs out.
            SSL_SHUTDOWN_EX_ARGS args {};
            args.quic_reason = "client refused the session";
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(100);
            while (SSL_shutdown_ex(quic_->connection,
                                   SSL_SHUTDOWN_FLAG_RAPID | SSL_SHUTDOWN_FLAG_NO_BLOCK,
                                   &args, sizeof(args)) != 1) {
                const int remaining = milliseconds_until(deadline);
                if (remaining == 0)
                    break;
                service(std::min(remaining, 10));
            }
            // A rapid shutdown can report done with the close still queued;
            // one more pass puts it on the wire before the socket goes.
            SSL_handle_events(quic_->connection);
            ERR_clear_error();
        }
        std::lock_guard<std::mutex> lock(abort_mutex_);
        delete quic_;
        quic_ = nullptr;
    }
    open_ = false;
    peer_closed_ = false;
    incoming_.clear();
}

} // namespace haiku_remote

#endif // HAIKU_REMOTE_HAVE_QUIC
