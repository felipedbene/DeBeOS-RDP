// quic_bridge_mock -- a stand-in for the DeBeOS remote_broker's QUIC front door.
//
// The QUIC counterpart of tools/wss_bridge_mock.py, for the end-to-end harness
// (tests/e2e/profile_e2e.py). Python here has no QUIC (no aioquic; the ssl
// module has none), so this is C++ against OpenSSL >= 3.5's QUIC server API.
//
// It follows the broker's own QUIC server, DeBeOS
// src/servers/remote_broker/QuicStream.cpp and RemoteBroker.cpp
// (handle_quic_connection(), serve_session(), QuicStream::Relay(), Close()):
//
//   - ALPN: a select callback that accepts exactly "debeos-urp/1" and fails
//     the handshake with a fatal alert otherwise (QuicStream.cpp select_alpn);
//     the selected ALPN is re-checked after the handshake (AcceptStream).
//   - No 0-RTT: SSL_CTX_set_max_early_data / set_recv_max_early_data 0.
//   - A thread-assisted, blocking domain; a listener with address validation
//     on; one thread per connection.
//   - Exactly one stream: client-initiated bidirectional stream 0. Anything
//     else, or a second stream at any time, closes the connection with
//     application error 2 (kQuicErrorStreamRules).
//   - RP_AUTHENTICATE(10) must be the first message, carrying the shared token
//     (method 1); a denial is answered RP_AUTH_RESULT(11) status 1 after the
//     broker's 2 s anti-brute-force delay, then error 1. On success the
//     session cookie is read from --cookie-file (trimmed of \n \r and space,
//     16..256 bytes, as read_session_cookie() does), the backend is dialled,
//     RP_SESSION_COOKIE(12, method 1) goes to it FIRST, then RP_AUTH_RESULT 0
//     to the client, then any bytes the client pipelined behind its
//     RP_AUTHENTICATE, then the relay both ways.
//   - The relay: two threads (SSL_poll waits on QUIC objects only). The
//     client's FIN, or the backend ending cleanly, is the orderly end:
//     CONNECTION_CLOSE with application error 0 after a stream FIN. Anything
//     else closes with error 1, rapidly.
//
// Deliberately not modelled: the Haiku-only workarounds (#790 errno sign,
// #792 SSL_poll race), the peer-address accounting and rate limiting, viewer
// tokens and session takeover.
//
// Everything it sees goes to stdout, one line each, so a harness can assert
// from the server side:
//   listening on quic://127.0.0.1:PORT -> tcp://HOST:PORT
//   conn N: alpn refused (client offered: a,b)
//   conn N: alpn debeos-urp/1
//   conn N: stream 0 accepted
//   conn N: auth ok | auth denied | auth malformed
//   conn N: cookie presented to the backend
//   conn N: relay ended: client FIN | backend closed | ...
//   conn N: closing with application error E
//   conn N: peer closed with application error E      (when the peer did)
//
// A client mode, --probe-alpn NAME, connects offering only NAME and reports
// whether the handshake completed, so the harness can show the server refuses
// any other ALPN (a client offering the wrong one sees the same refusal).
//
// --listen 0 (the default) binds an ephemeral UDP port; the "listening on"
// line is how the harness learns which, so parallel runs on one host never
// contend for a port.
//
// Usage:
//   quic_bridge_mock --listen 0 --backend 127.0.0.1:10900 --token T
//       --cert cert.pem --key key.pem --cookie-file session_cookie  (one line)
//   quic_bridge_mock --make-cert cert.pem key.pem
//   quic_bridge_mock --probe-alpn debeos-urp/2 --port PORT   (the port a bridge reported)
//
// Portable to Windows (Winsock) for the harness there; the few socket calls
// that differ go through the shims below.

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if OPENSSL_VERSION_NUMBER < 0x30500000L
#error "quic_bridge_mock needs OpenSSL 3.5 or newer (its QUIC server API)"
#endif

namespace {

// Sockets are ints here, as OpenSSL's BIO_new_dgram() takes them; on Windows
// a SOCKET fits (OpenSSL itself makes the same cast) and INVALID_SOCKET
// becomes -1, so `fd < 0` means the same on both.
#if defined(_WIN32)
void close_socket(int fd) { ::closesocket(static_cast<SOCKET>(fd)); }
int poll_in(int fd, int timeout_ms)
{
    WSAPOLLFD descriptor {static_cast<SOCKET>(fd), POLLIN, 0};
    return ::WSAPoll(&descriptor, 1, timeout_ms);
}
int socket_error() { return ::WSAGetLastError(); }
std::string socket_error_text(int code) { return "Winsock error " + std::to_string(code); }
const int kShutBoth = SD_BOTH;
#else
void close_socket(int fd) { ::close(fd); }
int poll_in(int fd, int timeout_ms)
{
    pollfd descriptor {fd, POLLIN, 0};
    return ::poll(&descriptor, 1, timeout_ms);
}
int socket_error() { return errno; }
std::string socket_error_text(int code) { return std::strerror(code); }
const int kShutBoth = SHUT_RDWR;
#endif

// QuicStream.h: QUIC_ALPN_URP1 is the ALPN in wire form.
const unsigned char kAlpnWire[] = "\x0c" "debeos-urp/1";
const unsigned int kAlpnWireLength = 13;

// QuicStream.h application error codes.
const uint64_t kQuicErrorNone = 0;
const uint64_t kQuicErrorRelay = 1;
const uint64_t kQuicErrorStreamRules = 2;

// RemoteBroker.cpp wire constants.
const uint16_t kRPAuthenticate = 10;
const uint16_t kRPAuthResult = 11;
const uint16_t kRPSessionCookie = 12;
const uint32_t kAuthMethodSharedToken = 1;
const uint32_t kCookieMethodPerBoot = 1;
const uint32_t kAuthResultSuccess = 0;
const uint32_t kAuthResultDenied = 1;
const uint32_t kAuthResultNoSession = 2;
const uint32_t kAuthResultNoCookie = 3;
const size_t kMaxAuthMessageSize = 4096;
const size_t kMaxTokenLength = 1024;
const size_t kMaxCookieLength = 256;
const size_t kMinCookieLength = 16;

using Clock = std::chrono::steady_clock;
const auto kHandshakeTimeout = std::chrono::seconds(15);
const auto kAuthFailureDelay = std::chrono::seconds(2);
const auto kRelayWriteTimeout = std::chrono::seconds(10);
const auto kWaitSlice = std::chrono::milliseconds(100);

std::mutex gLogLock;

void say(const char* format, ...)
{
    std::lock_guard<std::mutex> lock(gLogLock);
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

std::string openssl_error()
{
    unsigned long code = ERR_get_error();
    char buffer[256] = "unknown error";
    if (code != 0)
        ERR_error_string_n(code, buffer, sizeof(buffer));
    ERR_clear_error();
    return buffer;
}

struct Config {
    uint16_t port = 0; // 0: an ephemeral port, reported on the listening line
    std::string backendHost = "127.0.0.1";
    uint16_t backendPort = 10900;
    std::string token;
    std::string cert;
    std::string key;
    std::string cookieFile;
};

Config gConfig;
uint8_t gTokenDigest[SHA256_DIGEST_LENGTH];
std::atomic<int> gConnections {0};

int select_alpn(SSL* ssl, const unsigned char** out, unsigned char* outLength,
                const unsigned char* in, unsigned int inLength, void*)
{
    // QuicStream.cpp select_alpn(): exactly one protocol, a fatal alert for
    // anything else -- or nothing.
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), outLength,
                              kAlpnWire, kAlpnWireLength, in, inLength)
        == OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_OK;

    std::string offered;
    for (unsigned int i = 0; i < inLength;) {
        unsigned int length = in[i++];
        if (i + length > inLength)
            break;
        if (!offered.empty())
            offered += ",";
        offered.append(reinterpret_cast<const char*>(in + i), length);
        i += length;
    }
    (void)ssl;
    say("alpn refused (client offered: %s)", offered.c_str());
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// --- a QuicStream, minus the Haiku workarounds and the peer address ---------

class Stream {
public:
    Stream(SSL* connection, int id) : fConnection(connection), fId(id) {}
    ~Stream()
    {
        SSL_free(fStream);
        SSL_free(fConnection);
    }

    int Id() const { return fId; }

    enum Status { kOk, kTimedOut, kInterrupted, kIoError, kNotAllowed };

    Status AcceptStream(Clock::time_point deadline)
    {
        if (!SSL_set_blocking_mode(fConnection, 0)
            || !SSL_set_default_stream_mode(fConnection,
                                            SSL_DEFAULT_STREAM_MODE_NONE)
            || !SSL_set_incoming_stream_policy(
                fConnection, SSL_INCOMING_STREAM_POLICY_ACCEPT, 0)) {
            say("conn %d: configuring the connection: %s", fId,
                openssl_error().c_str());
            return kIoError;
        }
        SSL_set_mode(fConnection, SSL_MODE_ENABLE_PARTIAL_WRITE);

        while (true) {
            fStream = SSL_accept_stream(fConnection, SSL_ACCEPT_STREAM_NO_BLOCK);
            if (fStream != nullptr)
                break;
            uint64_t revents = 0;
            Status result = Wait(fConnection,
                                 SSL_POLL_EVENT_IS | SSL_POLL_EVENT_EC, deadline,
                                 &revents);
            if (result != kOk)
                return result;
            if ((revents & SSL_POLL_EVENT_EC) != 0)
                return kIoError;
        }

        const unsigned char* alpn = nullptr;
        unsigned int alpnLength = 0;
        SSL_get0_alpn_selected(fConnection, &alpn, &alpnLength);
        if (alpnLength != kAlpnWireLength - 1
            || std::memcmp(alpn, kAlpnWire + 1, alpnLength) != 0) {
            say("conn %d: alpn after the handshake is not debeos-urp/1", fId);
            return kNotAllowed;
        }
        say("conn %d: alpn %.*s", fId, static_cast<int>(alpnLength),
            reinterpret_cast<const char*>(alpn));

        const uint64_t streamId = SSL_get_stream_id(fStream);
        if (streamId != 0 || HasForbiddenStream()) {
            say("conn %d: stream rules broken (first stream id %llu%s)", fId,
                static_cast<unsigned long long>(streamId),
                HasForbiddenStream() ? ", a second stream queued" : "");
            return kNotAllowed;
        }
        say("conn %d: stream %llu accepted", fId,
            static_cast<unsigned long long>(streamId));
        return kOk;
    }

    // > 0 bytes read, 0 the peer's FIN (or the connection closed), < 0 a
    // Status negated.
    long ReadSome(void* buffer, size_t size, Clock::time_point deadline)
    {
        while (true) {
            size_t read = 0;
            if (SSL_read_ex(fStream, buffer, size, &read) == 1 && read > 0)
                return static_cast<long>(read);
            switch (SSL_get_stream_read_state(fStream)) {
            case SSL_STREAM_STATE_OK:
                break;
            case SSL_STREAM_STATE_FINISHED:
                fSawFin = true;
                ERR_clear_error();
                return 0;
            case SSL_STREAM_STATE_CONN_CLOSED:
                ERR_clear_error();
                return 0;
            default:
                ERR_clear_error();
                return -kIoError;
            }
            ERR_clear_error();
            Status result = Wait(fStream,
                                 SSL_POLL_EVENT_R | SSL_POLL_EVENT_ER
                                     | SSL_POLL_EVENT_EC,
                                 deadline, nullptr);
            if (result != kOk)
                return -result;
        }
    }

    Status WriteAll(const void* buffer, size_t size, Clock::time_point deadline)
    {
        const uint8_t* position = static_cast<const uint8_t*>(buffer);
        while (size > 0) {
            size_t written = 0;
            if (SSL_write_ex(fStream, position, size, &written) == 1) {
                position += written;
                size -= written;
                continue;
            }
            if (SSL_get_stream_write_state(fStream) != SSL_STREAM_STATE_OK) {
                ERR_clear_error();
                return kIoError;
            }
            ERR_clear_error();
            Status result = Wait(fStream,
                                 SSL_POLL_EVENT_W | SSL_POLL_EVENT_EW
                                     | SSL_POLL_EVENT_EC,
                                 deadline, nullptr);
            if (result != kOk)
                return result;
        }
        return kOk;
    }

    bool HasForbiddenStream()
    {
        return SSL_get_accept_stream_queue_len(fConnection) > 0;
    }

    bool SawFin() const { return fSawFin; }

    void Stop() { fStopped.store(true); }

    void Close(uint64_t errorCode)
    {
        say("conn %d: closing with application error %llu", fId,
            static_cast<unsigned long long>(errorCode));
        uint64_t flags = SSL_SHUTDOWN_FLAG_NO_BLOCK;
        if (errorCode == kQuicErrorNone) {
            if (fStream != nullptr)
                SSL_stream_conclude(fStream, 0);
        } else
            flags |= SSL_SHUTDOWN_FLAG_RAPID;

        SSL_SHUTDOWN_EX_ARGS args;
        std::memset(&args, 0, sizeof(args));
        args.quic_error_code = errorCode;
        args.quic_reason = errorCode == kQuicErrorNone ? "" : "remote_broker";

        const auto deadline = Clock::now() + std::chrono::seconds(1);
        while (SSL_shutdown_ex(fConnection, flags, &args, sizeof(args)) != 1) {
            if (Clock::now() >= deadline)
                break;
            SSL_handle_events(fConnection);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ERR_clear_error();
    }

    // Who closed the connection, and with what.
    void ReportClose()
    {
        SSL_CONN_CLOSE_INFO info;
        std::memset(&info, 0, sizeof(info));
        if (SSL_get_conn_close_info(fConnection, &info, sizeof(info)) != 1)
            return;
        say("conn %d: %s closed with %s error %llu%s%.*s", fId,
            (info.flags & SSL_CONN_CLOSE_FLAG_LOCAL) != 0 ? "bridge" : "peer",
            (info.flags & SSL_CONN_CLOSE_FLAG_TRANSPORT) != 0 ? "transport"
                                                               : "application",
            static_cast<unsigned long long>(info.error_code),
            info.reason_len > 0 ? ": " : "",
            static_cast<int>(info.reason_len),
            info.reason != nullptr ? info.reason : "");
    }

private:
    Status Wait(SSL* object, uint64_t events, Clock::time_point deadline,
                uint64_t* revents)
    {
        while (true) {
            if (fStopped.load())
                return kInterrupted;
            const auto now = Clock::now();
            if (now >= deadline)
                return kTimedOut;
            auto slice = std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - now);
            if (slice > kWaitSlice)
                slice = kWaitSlice;
            timeval timeout;
            timeout.tv_sec = static_cast<long>(slice.count() / 1000000);
            timeout.tv_usec = static_cast<long>(slice.count() % 1000000);

            SSL_POLL_ITEM item;
            std::memset(&item, 0, sizeof(item));
            item.desc = SSL_as_poll_descriptor(object);
            item.events = events;
            size_t count = 0;
            if (SSL_poll(&item, 1, sizeof(item), &timeout, 0, &count) != 1) {
                say("conn %d: SSL_poll failed: %s", fId, openssl_error().c_str());
                return kIoError;
            }
            if (count > 0 && (item.revents & events) != 0) {
                if (revents != nullptr)
                    *revents = item.revents;
                return kOk;
            }
        }
    }

    SSL* fConnection;
    SSL* fStream = nullptr;
    int fId;
    std::atomic<bool> fStopped {false};
    bool fSawFin = false;
};

bool write_fully(int fd, const uint8_t* buffer, size_t size)
{
    while (size > 0) {
#if defined(_WIN32)
        const int written = ::send(static_cast<SOCKET>(fd),
                                   reinterpret_cast<const char*>(buffer),
                                   static_cast<int>(size), 0);
        if (written < 0)
            return false;
#else
        ssize_t written = ::write(fd, buffer, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
#endif
        buffer += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

void put32(uint8_t* at, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        at[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint32_t get32(const uint8_t* at)
{
    return uint32_t(at[0]) | uint32_t(at[1]) << 8 | uint32_t(at[2]) << 16
        | uint32_t(at[3]) << 24;
}

Stream::Status send_auth_result(Stream& stream, uint32_t status)
{
    uint8_t reply[10];
    reply[0] = kRPAuthResult & 0xff;
    reply[1] = kRPAuthResult >> 8;
    put32(reply + 2, sizeof(reply));
    put32(reply + 6, status);
    return stream.WriteAll(reply, sizeof(reply),
                           Clock::now() + std::chrono::seconds(1));
}

enum class AuthOutcome { ok, denied, malformed, lost };

// RemoteBroker.cpp authenticate_client(): the complete RP_AUTHENTICATE, the
// token compared hash-then-compare; pipelined bytes are left in the buffer.
AuthOutcome authenticate(Stream& stream, Clock::time_point deadline,
                         uint8_t* buffer, size_t bufferSize, size_t& extraOffset,
                         size_t& extraLength)
{
    size_t used = 0;
    uint32_t messageLength = 0;
    while (true) {
        if (used >= 6) {
            uint16_t code = uint16_t(buffer[0] | buffer[1] << 8);
            messageLength = get32(buffer + 2);
            if (code != kRPAuthenticate || messageLength < 6 + 8
                || messageLength > kMaxAuthMessageSize)
                return AuthOutcome::malformed;
            if (used >= messageLength)
                break;
        }
        if (used == bufferSize)
            return AuthOutcome::malformed;
        long read = stream.ReadSome(buffer + used, bufferSize - used, deadline);
        if (read <= 0)
            return AuthOutcome::lost;
        used += static_cast<size_t>(read);
    }

    uint32_t method = get32(buffer + 6);
    uint32_t tokenLength = get32(buffer + 10);
    if (method != kAuthMethodSharedToken || tokenLength > kMaxTokenLength
        || 6 + 8 + tokenLength != messageLength)
        return AuthOutcome::malformed;

    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(buffer + 14, tokenLength, digest);
    if (CRYPTO_memcmp(digest, gTokenDigest, SHA256_DIGEST_LENGTH) != 0)
        return AuthOutcome::denied;

    extraOffset = messageLength;
    extraLength = used - messageLength;
    return AuthOutcome::ok;
}

bool read_session_cookie(std::string& cookie)
{
    std::ifstream in(gConfig.cookieFile, std::ios::binary);
    if (!in)
        return false;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (text.size() > kMaxCookieLength)
        return false;
    while (!text.empty()
           && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    if (text.size() < kMinCookieLength)
        return false;
    cookie = text;
    return true;
}

int connect_backend()
{
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0)
        return -1;
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(gConfig.backendPort);
    if (inet_pton(AF_INET, gConfig.backendHost.c_str(), &address.sin_addr) != 1
        || ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))
            != 0) {
        close_socket(fd);
        return -1;
    }
    int noDelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
               sizeof(noDelay));
    return fd;
}

bool send_session_cookie(int fd, const std::string& cookie)
{
    std::vector<uint8_t> frame(6 + 8 + cookie.size());
    frame[0] = kRPSessionCookie & 0xff;
    frame[1] = kRPSessionCookie >> 8;
    put32(frame.data() + 2, static_cast<uint32_t>(frame.size()));
    put32(frame.data() + 6, kCookieMethodPerBoot);
    put32(frame.data() + 10, static_cast<uint32_t>(cookie.size()));
    std::memcpy(frame.data() + 14, cookie.data(), cookie.size());
    return write_fully(fd, frame.data(), frame.size());
}

struct Outbound {
    Stream* stream;
    int socket;
    std::atomic<bool> ended {false};
    bool orderly = false;
};

void relay_outbound(Outbound* relay)
{
    uint8_t buffer[16 * 1024];
    relay->orderly = false;
    while (true) {
        const auto read = ::recv(relay->socket, reinterpret_cast<char*>(buffer),
                                 static_cast<int>(sizeof(buffer)), 0);
        if (read < 0 && socket_error() == EINTR)
            continue;
        if (read <= 0) {
            relay->orderly = read == 0;
            break;
        }
        if (relay->stream->WriteAll(buffer, static_cast<size_t>(read),
                                    Clock::now() + kRelayWriteTimeout)
            != Stream::kOk)
            break;
    }
    relay->ended.store(true);
    relay->stream->Stop();
}

// QuicStream::Relay().
uint64_t relay(Stream& stream, int backend)
{
    Outbound outbound;
    outbound.stream = &stream;
    outbound.socket = backend;
    std::thread thread(relay_outbound, &outbound);

    uint64_t errorCode = kQuicErrorRelay;
    const char* why = "relay failed";
    uint8_t buffer[16 * 1024];
    while (true) {
        if (stream.HasForbiddenStream()) {
            why = "client opened a second stream";
            errorCode = kQuicErrorStreamRules;
            break;
        }
        long read = stream.ReadSome(buffer, sizeof(buffer),
                                    Clock::now() + 4 * kWaitSlice);
        if (read == -Stream::kTimedOut)
            continue;
        if (read == -Stream::kInterrupted) {
            if (outbound.ended.load() && outbound.orderly) {
                why = "backend closed";
                errorCode = kQuicErrorNone;
            } else
                why = "backend failed";
            break;
        }
        if (read <= 0) {
            if (read == 0) {
                why = stream.SawFin() ? "client FIN" : "client closed";
                errorCode = kQuicErrorNone;
            } else
                why = "client stream failed";
            break;
        }
        if (!write_fully(backend, buffer, static_cast<size_t>(read))) {
            why = "backend write failed";
            break;
        }
    }
    stream.Stop();
    ::shutdown(backend, kShutBoth);
    thread.join();
    say("conn %d: relay ended: %s", stream.Id(), why);
    return errorCode;
}

// RemoteBroker.cpp handle_quic_connection() + serve_session().
void serve(SSL* connection, int id)
{
    Stream stream(connection, id);
    const auto deadline = Clock::now() + kHandshakeTimeout;

    Stream::Status accepted = stream.AcceptStream(deadline);
    if (accepted != Stream::kOk) {
        say("conn %d: QUIC handshake or stream setup failed", id);
        stream.Close(accepted == Stream::kNotAllowed ? kQuicErrorStreamRules
                                                     : kQuicErrorRelay);
        stream.ReportClose();
        return;
    }

    uint8_t authBuffer[kMaxAuthMessageSize];
    size_t extraOffset = 0, extraLength = 0;
    AuthOutcome outcome = authenticate(stream, deadline, authBuffer,
                                       sizeof(authBuffer), extraOffset,
                                       extraLength);
    if (outcome != AuthOutcome::ok) {
        OPENSSL_cleanse(authBuffer, sizeof(authBuffer));
        say("conn %d: auth %s", id,
            outcome == AuthOutcome::denied   ? "denied"
            : outcome == AuthOutcome::malformed ? "malformed"
                                                : "lost before the message");
        std::this_thread::sleep_for(kAuthFailureDelay);
        send_auth_result(stream, kAuthResultDenied);
        stream.Close(kQuicErrorRelay);
        stream.ReportClose();
        return;
    }
    say("conn %d: auth ok", id);

    std::string cookie;
    if (!read_session_cookie(cookie)) {
        say("conn %d: cannot read the session cookie from %s", id,
            gConfig.cookieFile.c_str());
        send_auth_result(stream, kAuthResultNoCookie);
        stream.Close(kQuicErrorRelay);
        return;
    }
    int backend = connect_backend();
    if (backend < 0) {
        say("conn %d: session port unreachable", id);
        send_auth_result(stream, kAuthResultNoSession);
        stream.Close(kQuicErrorRelay);
        return;
    }
    bool ok = send_session_cookie(backend, cookie);
    OPENSSL_cleanse(&cookie[0], cookie.size());
    if (ok) {
        say("conn %d: cookie presented to the backend", id);
        ok = send_auth_result(stream, kAuthResultSuccess) == Stream::kOk;
    }
    if (ok && extraLength > 0)
        ok = write_fully(backend, authBuffer + extraOffset, extraLength);
    OPENSSL_cleanse(authBuffer, sizeof(authBuffer));
    if (!ok) {
        close_socket(backend);
        say("conn %d: could not start the session", id);
        return;
    }

    uint64_t errorCode = relay(stream, backend);
    close_socket(backend);
    stream.Close(errorCode);
    stream.ReportClose();
    say("conn %d: session ended", id);
}

// --- server ---------------------------------------------------------------

int run_server()
{
    SSL_CTX* context = SSL_CTX_new(OSSL_QUIC_server_method());
    if (context == nullptr) {
        std::fprintf(stderr, "SSL_CTX_new(QUIC): %s\n", openssl_error().c_str());
        return 1;
    }
    if (SSL_CTX_use_certificate_chain_file(context, gConfig.cert.c_str()) != 1
        || SSL_CTX_use_PrivateKey_file(context, gConfig.key.c_str(),
                                       SSL_FILETYPE_PEM) != 1
        || SSL_CTX_check_private_key(context) != 1) {
        std::fprintf(stderr, "loading the certificate/key: %s\n",
                     openssl_error().c_str());
        return 1;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
    if (SSL_CTX_set_max_early_data(context, 0) != 1
        || SSL_CTX_set_recv_max_early_data(context, 0) != 1) {
        std::fprintf(stderr, "disabling early data: %s\n", openssl_error().c_str());
        return 1;
    }
    SSL_CTX_set_alpn_select_cb(context, select_alpn, nullptr);

    SSL* domain = SSL_new_domain(context, SSL_DOMAIN_FLAG_THREAD_ASSISTED
                                              | SSL_DOMAIN_FLAG_BLOCKING);
    if (domain == nullptr) {
        std::fprintf(stderr, "SSL_new_domain: %s\n", openssl_error().c_str());
        return 1;
    }
    SSL* listener = SSL_new_listener_from(domain, 0);
    if (listener == nullptr) {
        std::fprintf(stderr, "SSL_new_listener_from: %s\n", openssl_error().c_str());
        return 1;
    }

    int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(gConfig.port);
    if (fd < 0
        || ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))
            != 0) {
        // Exit 3: a port given explicitly with --listen was taken.
        std::fprintf(stderr, "cannot bind UDP 127.0.0.1:%u: %s\n", gConfig.port,
                     socket_error_text(socket_error()).c_str());
        return 3;
    }
    // The port the kernel chose for --listen 0 (and the one asked for
    // otherwise): the listening line below reports what was really bound.
    {
        sockaddr_in bound;
        std::memset(&bound, 0, sizeof(bound));
#if defined(_WIN32)
        int length = sizeof(bound);
#else
        socklen_t length = sizeof(bound);
#endif
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
            std::fprintf(stderr, "getsockname: %s\n",
                         socket_error_text(socket_error()).c_str());
            return 1;
        }
        gConfig.port = ntohs(bound.sin_port);
    }
    BIO* datagram = BIO_new_dgram(fd, BIO_NOCLOSE);
    if (datagram == nullptr) {
        std::fprintf(stderr, "BIO_new_dgram: %s\n", openssl_error().c_str());
        return 1;
    }
    SSL_set_bio(listener, datagram, datagram);
    if (SSL_listen(listener) != 1) {
        std::fprintf(stderr, "SSL_listen: %s\n", openssl_error().c_str());
        return 1;
    }
    say("listening on quic://127.0.0.1:%u -> tcp://%s:%u", gConfig.port,
        gConfig.backendHost.c_str(), gConfig.backendPort);

    while (true) {
        SSL* connection = SSL_accept_connection(listener, 0);
        if (connection == nullptr) {
            say("SSL_accept_connection: %s", openssl_error().c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        int id = ++gConnections;
        say("conn %d: accepted", id);
        std::thread(serve, connection, id).detach();
    }
}

// --- certificate -------------------------------------------------------------

int make_cert(const char* certPath, const char* keyPath)
{
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    X509* cert = X509_new();
    if (key == nullptr || cert == nullptr)
        return 1;
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert),
                     static_cast<long>(std::time(nullptr)) & 0x7fffffff);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 7 * 24 * 3600);
    X509_set_pubkey(cert, key);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"),
                               -1, -1, 0);
    X509_set_issuer_name(cert, name);
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* san = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name,
                                              "DNS:localhost,IP:127.0.0.1");
    if (san == nullptr || X509_add_ext(cert, san, -1) != 1)
        return 1;
    X509_EXTENSION_free(san);
    if (X509_sign(cert, key, EVP_sha256()) == 0)
        return 1;
    FILE* c = std::fopen(certPath, "wb");
    FILE* k = std::fopen(keyPath, "wb");
    if (c == nullptr || k == nullptr)
        return 1;
    bool ok = PEM_write_X509(c, cert) == 1
        && PEM_write_PrivateKey(k, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
    std::fclose(c);
    std::fclose(k);
    X509_free(cert);
    EVP_PKEY_free(key);
    return ok ? 0 : 1;
}

// --- probe -------------------------------------------------------------------

// Connects offering only `alpn`; exit 0 and "probe: handshake completed, alpn
// X" if the server took it, exit 1 and "probe: handshake failed: ..." if not.
int probe(const std::string& alpn, uint16_t port)
{
    SSL_CTX* context = SSL_CTX_new(OSSL_QUIC_client_method());
    SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
    SSL* ssl = SSL_new(context);
    int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    sockaddr_in local;
    std::memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    ::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local));
    BIO* bio = BIO_new_dgram(fd, BIO_NOCLOSE);
    SSL_set_bio(ssl, bio, bio);
    std::vector<unsigned char> wire;
    wire.push_back(static_cast<unsigned char>(alpn.size()));
    wire.insert(wire.end(), alpn.begin(), alpn.end());
    SSL_set_alpn_protos(ssl, wire.data(), static_cast<unsigned int>(wire.size()));
    in_addr loopback;
    loopback.s_addr = htonl(INADDR_LOOPBACK);
    BIO_ADDR* peer = BIO_ADDR_new();
    BIO_ADDR_rawmake(peer, AF_INET, &loopback, sizeof(loopback), htons(port));
    SSL_set1_initial_peer_addr(ssl, peer);
    BIO_ADDR_free(peer);
    SSL_set_blocking_mode(ssl, 0);

    // OpenSSL 3.5's server does not get its no_application_protocol alert
    // to the client (observed with openssl s_client too), so a refusal shows
    // here as a handshake that never completes.
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (true) {
        int result = SSL_connect(ssl);
        if (result == 1)
            break;
        int code = SSL_get_error(ssl, result);
        if (code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE) {
            std::printf("probe: handshake failed: %s\n", openssl_error().c_str());
            return 1;
        }
        if (Clock::now() >= deadline) {
            std::printf("probe: handshake failed: timed out\n");
            return 1;
        }
        poll_in(fd, 20);
        SSL_handle_events(ssl);
    }
    const unsigned char* selected = nullptr;
    unsigned int length = 0;
    SSL_get0_alpn_selected(ssl, &selected, &length);
    std::printf("probe: handshake completed, alpn %.*s\n", static_cast<int>(length),
                reinterpret_cast<const char*>(selected));
    SSL_SHUTDOWN_EX_ARGS args;
    std::memset(&args, 0, sizeof(args));
    SSL_shutdown_ex(ssl, SSL_SHUTDOWN_FLAG_RAPID | SSL_SHUTDOWN_FLAG_NO_BLOCK,
                    &args, sizeof(args));
    SSL_handle_events(ssl);
    return 0;
}

[[noreturn]] void usage()
{
    std::fprintf(stderr,
                 "usage: quic_bridge_mock --token T --cert C --key K"
                 " --cookie-file F [--listen PORT] [--backend HOST:PORT]\n"
                 "       quic_bridge_mock --make-cert CERT KEY\n"
                 "       quic_bridge_mock --probe-alpn NAME [--port PORT]\n");
    std::exit(2);
}

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    WSADATA wsa {};
    ::WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::string probeAlpn;
    bool probing = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc)
                usage();
            return argv[++i];
        };
        if (a == "--make-cert") {
            if (i + 2 >= argc)
                usage();
            return make_cert(argv[i + 1], argv[i + 2]);
        } else if (a == "--listen" || a == "--port")
            gConfig.port = static_cast<uint16_t>(std::atoi(value().c_str()));
        else if (a == "--backend") {
            std::string spec = value();
            size_t colon = spec.rfind(':');
            if (colon == std::string::npos)
                usage();
            gConfig.backendHost = spec.substr(0, colon);
            gConfig.backendPort =
                static_cast<uint16_t>(std::atoi(spec.c_str() + colon + 1));
        } else if (a == "--token")
            gConfig.token = value();
        else if (a == "--cert")
            gConfig.cert = value();
        else if (a == "--key")
            gConfig.key = value();
        else if (a == "--cookie-file")
            gConfig.cookieFile = value();
        else if (a == "--probe-alpn") {
            probeAlpn = value();
            probing = true;
        } else
            usage();
    }
    if (probing) {
        if (gConfig.port == 0)
            usage(); // a probe needs the port a bridge reported
        return probe(probeAlpn, gConfig.port);
    }
    if (gConfig.token.empty() || gConfig.cert.empty() || gConfig.key.empty()
        || gConfig.cookieFile.empty())
        usage();
    SHA256(reinterpret_cast<const unsigned char*>(gConfig.token.data()),
           gConfig.token.size(), gTokenDigest);
    return run_server();
}
