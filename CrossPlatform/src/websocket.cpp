#include "haiku_remote/websocket.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>

#ifndef _WIN32
#include <cerrno>
#include <poll.h>
#endif

namespace haiku_remote {
namespace {

constexpr std::uint8_t opcode_continuation = 0x0;
constexpr std::uint8_t opcode_text = 0x1;
constexpr std::uint8_t opcode_binary = 0x2;
constexpr std::uint8_t opcode_close = 0x8;
constexpr std::uint8_t opcode_ping = 0x9;
constexpr std::uint8_t opcode_pong = 0xa;

constexpr std::size_t max_frame_payload = 64 * 1024 * 1024;
constexpr std::size_t max_buffered_bytes = 256 * 1024 * 1024;
constexpr int handshake_timeout_ms = 10000;
// The broker answers a denied token only after an anti-brute-force delay.
constexpr int auth_timeout_ms = 15000;

// Broker transport-security preamble opcodes (RemoteMessage.h).
constexpr std::uint16_t rp_authenticate = 10;
constexpr std::uint16_t rp_auth_result = 11;

std::string base64_encode(std::span<const std::uint8_t> bytes)
{
    std::string encoded((bytes.size() + 2) / 3 * 4 + 1, '\0');
    const int length = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), bytes.data(),
        static_cast<int>(bytes.size()));
    encoded.resize(length > 0 ? static_cast<std::size_t>(length) : 0);
    return encoded;
}

std::string to_lower(std::string_view text)
{
    std::string lowered(text);
    for (auto& character : lowered)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return lowered;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

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

// Decodes a pin given as hex or base64 into a 32-byte SHA-256 digest. Accepts
// the broker.fingerprint format (lower/upper hex, optionally colon-separated),
// an optional "sha256:" prefix, and curl's "sha256//".
bool decode_pin(std::string_view pin, std::array<std::uint8_t, 32>& digest,
                std::string& error)
{
    if (pin.rfind("sha256//", 0) == 0)
        pin.remove_prefix(8);
    else if (pin.rfind("sha256:", 0) == 0)
        pin.remove_prefix(7);

    std::string stripped;
    if (pin.find(':') != std::string_view::npos) {
        for (const char character : pin) {
            if (character != ':')
                stripped.push_back(character);
        }
        pin = stripped;
    }

    if (pin.size() == 64) {
        auto nibble = [](char character) -> int {
            if (character >= '0' && character <= '9') return character - '0';
            if (character >= 'a' && character <= 'f') return character - 'a' + 10;
            if (character >= 'A' && character <= 'F') return character - 'A' + 10;
            return -1;
        };
        bool valid = true;
        for (std::size_t i = 0; i < 32; ++i) {
            const int high = nibble(pin[2 * i]);
            const int low = nibble(pin[2 * i + 1]);
            if (high < 0 || low < 0) {
                valid = false;
                break;
            }
            digest[i] = static_cast<std::uint8_t>(high << 4 | low);
        }
        if (valid)
            return true;
    }

    if (pin.size() == 44 && pin.back() == '=') {
        std::array<std::uint8_t, 33> decoded {};
        const int length = EVP_DecodeBlock(
            decoded.data(), reinterpret_cast<const unsigned char*>(pin.data()),
            static_cast<int>(pin.size()));
        // EVP_DecodeBlock reports padding bytes as data; 44 base64 characters
        // with one '=' decode to 32 payload bytes.
        if (length == 33) {
            std::copy_n(decoded.begin(), 32, digest.begin());
            return true;
        }
    }

    error = "certificate pin must be a SHA-256 digest in hex or base64"
            " (optionally prefixed with sha256//)";
    return false;
}

} // namespace

struct WebSocketTransport::Tls {
    SSL_CTX* context = nullptr;
    SSL* session = nullptr;

    ~Tls()
    {
        if (session != nullptr)
            SSL_free(session);
        if (context != nullptr)
            SSL_CTX_free(context);
    }
};

WebSocketTransport::WebSocketTransport(std::string host, std::uint16_t port,
                                       std::string target, bool secure,
                                       const TransportOptions& options)
    : host_(std::move(host))
    , port_(port)
    , target_(std::move(target))
    , secure_(secure)
    , token_(options.token)
    , pin_sha256_(options.pin_sha256)
    , ca_file_(options.ca_file)
    , insecure_(options.insecure)
{
}

WebSocketTransport::~WebSocketTransport()
{
    close();
}

std::string WebSocketTransport::describe() const
{
    return std::string(secure_ ? "wss://" : "ws://") + host_ + ":"
        + std::to_string(port_) + target_;
}

bool WebSocketTransport::connect(std::string& error)
{
    close();
    if (!socket_.connect(host_, port_, error))
        return false;
    if (secure_ && !tls_connect(error)) {
        close();
        return false;
    }
    if (!upgrade(error)) {
        close();
        return false;
    }
    open_ = true;
    if (!token_.empty() && !authenticate(error)) {
        close();
        return false;
    }
    return true;
}


/*!	The broker's transport-security preamble: RP_AUTHENTICATE must be the
	first binary message on the fresh WebSocket, and nothing is proxied to the
	session until the broker answers RP_AUTH_RESULT with success. Both are
	RP-framed (u16 code, u32 total length including the 6-byte header, all
	little-endian); the authenticate body is u32 method (1 = shared token),
	u32 token length, then the token bytes.
*/
bool WebSocketTransport::authenticate(std::string& error)
{
    std::vector<std::uint8_t> message;
    const std::uint32_t total = 6 + 8 + static_cast<std::uint32_t>(token_.size());
    message.reserve(total);
    const auto add_u16 = [&](std::uint16_t value) {
        message.push_back(static_cast<std::uint8_t>(value));
        message.push_back(static_cast<std::uint8_t>(value >> 8));
    };
    const auto add_u32 = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            message.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    add_u16(rp_authenticate);
    add_u32(total);
    add_u32(1); // method: shared token
    add_u32(static_cast<std::uint32_t>(token_.size()));
    message.insert(message.end(), token_.begin(), token_.end());
    if (!send_frame(opcode_binary, message, error))
        return false;

    // Wait for the 10-byte RP_AUTH_RESULT. A denied attempt is answered only
    // after the broker's anti-brute-force delay, so allow for it. Session
    // bytes pipelined behind the result stay in incoming_ for the caller.
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(auth_timeout_ms);
    std::string receive_error;
    while (incoming_.size() < 10) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            error = "timed out waiting for the broker's authentication result";
            return false;
        }
        const int count = raw_receive(static_cast<int>(remaining), receive_error);
        const bool drained = count >= 0 && drain_frames(receive_error);
        if (incoming_.size() >= 10)
            break; // the result arrived, even if a close followed it
        if (count < 0 || !drained) {
            error = "connection lost before the broker's authentication result"
                    " (" + receive_error + ")";
            return false;
        }
    }

    const auto u16_at = [&](std::size_t offset) {
        return static_cast<std::uint16_t>(incoming_[offset]
                                          | incoming_[offset + 1] << 8);
    };
    const auto u32_at = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(
            incoming_[offset] | incoming_[offset + 1] << 8
            | incoming_[offset + 2] << 16
            | static_cast<std::uint32_t>(incoming_[offset + 3]) << 24);
    };
    if (u16_at(0) != rp_auth_result || u32_at(2) != 10) {
        error = "broker sent an unexpected reply to RP_AUTHENTICATE";
        return false;
    }
    const std::uint32_t status = u32_at(6);
    incoming_.erase(incoming_.begin(), incoming_.begin() + 10);
    switch (status) {
    case 0:
        return true;
    case 1:
        error = "broker denied the authentication token";
        return false;
    case 2:
        error = "broker has no session to attach (the remote interface is not"
                " reachable behind it)";
        return false;
    default:
        error = "broker reported authentication status "
            + std::to_string(status);
        return false;
    }
}

bool WebSocketTransport::tls_connect(std::string& error)
{
    tls_ = new Tls;
    tls_->context = SSL_CTX_new(TLS_client_method());
    if (tls_->context == nullptr) {
        error = openssl_error("SSL_CTX_new failed");
        return false;
    }
    SSL_CTX_set_min_proto_version(tls_->context, TLS1_2_VERSION);

    // A pin authenticates the server by key identity, so chain verification is
    // not additionally required (the broker typically runs on a self-signed
    // certificate). Without a pin, the chain and host name are verified
    // against the system store or --ca-file, unless --insecure.
    const bool verify_chain = pin_sha256_.empty() && !insecure_;
    if (verify_chain) {
        if (!ca_file_.empty()) {
            if (SSL_CTX_load_verify_locations(
                    tls_->context, ca_file_.c_str(), nullptr) != 1) {
                error = openssl_error("could not load --ca-file " + ca_file_);
                return false;
            }
        } else if (SSL_CTX_set_default_verify_paths(tls_->context) != 1) {
            error = openssl_error("could not load system trust store");
            return false;
        }
        SSL_CTX_set_verify(tls_->context, SSL_VERIFY_PEER, nullptr);
    }

    tls_->session = SSL_new(tls_->context);
    if (tls_->session == nullptr) {
        error = openssl_error("SSL_new failed");
        return false;
    }
    SSL_set_tlsext_host_name(tls_->session, host_.c_str());
    if (verify_chain && SSL_set1_host(tls_->session, host_.c_str()) != 1) {
        error = "could not configure host name verification";
        return false;
    }
    if (SSL_set_fd(tls_->session, static_cast<int>(socket_.native_handle())) != 1) {
        error = openssl_error("SSL_set_fd failed");
        return false;
    }
    if (SSL_connect(tls_->session) != 1) {
        error = openssl_error("TLS handshake with " + host_ + " failed");
        return false;
    }
    if (!pin_sha256_.empty() && !verify_pin(error))
        return false;
    return true;
}

bool WebSocketTransport::verify_pin(std::string& error)
{
    std::array<std::uint8_t, 32> expected {};
    if (!decode_pin(pin_sha256_, expected, error))
        return false;

    X509* certificate = SSL_get1_peer_certificate(tls_->session);
    if (certificate == nullptr) {
        error = "server presented no certificate to pin against";
        return false;
    }

    // The broker's pin is its certificate's SHA-256 fingerprint -- the digest
    // of the whole certificate in DER form, exactly what it writes to
    // broker.fingerprint on first run (and what
    // `openssl x509 -in cert.pem -fingerprint -sha256` prints).
    std::array<std::uint8_t, 32> actual {};
    unsigned int digest_length = 0;
    const int digested
        = X509_digest(certificate, EVP_sha256(), actual.data(), &digest_length);
    X509_free(certificate);
    if (digested != 1 || digest_length != actual.size()) {
        error = openssl_error("could not fingerprint server certificate");
        return false;
    }

    if (actual != expected) {
        std::ostringstream text;
        text << "certificate pin mismatch: server fingerprint sha256:"
             << std::hex << std::setfill('0');
        for (const auto byte : actual)
            text << std::setw(2) << static_cast<unsigned>(byte);
        error = text.str();
        return false;
    }
    return true;
}

bool WebSocketTransport::raw_send(std::span<const std::uint8_t> bytes,
                                  std::string& error)
{
    if (tls_ == nullptr)
        return socket_.send_all(bytes, error);

    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int count = SSL_write(tls_->session, bytes.data() + sent,
                                    static_cast<int>(bytes.size() - sent));
        if (count <= 0) {
            error = openssl_error("TLS send failed");
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

int WebSocketTransport::raw_receive(int timeout_ms, std::string& error)
{
    std::array<std::uint8_t, 64 * 1024> buffer;
    if (tls_ == nullptr) {
        const int count = socket_.receive(buffer, timeout_ms, error);
        if (count > 0)
            frame_buffer_.insert(frame_buffer_.end(), buffer.begin(),
                                 buffer.begin() + count);
        return count;
    }

    // Bytes may already be decrypted and buffered inside the TLS session, in
    // which case the descriptor never polls readable for them.
    if (SSL_pending(tls_->session) == 0) {
#ifdef _WIN32
        fd_set set;
        FD_ZERO(&set);
        FD_SET(static_cast<SOCKET>(socket_.native_handle()), &set);
        timeval timeout {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        const int ready = select(0, &set, nullptr, nullptr, &timeout);
#else
        pollfd descriptor {static_cast<int>(socket_.native_handle()), POLLIN, 0};
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
    }

    const int count = SSL_read(tls_->session, buffer.data(),
                               static_cast<int>(buffer.size()));
    if (count <= 0) {
        const int status = SSL_get_error(tls_->session, count);
        if (status == SSL_ERROR_ZERO_RETURN)
            error = "connection closed by peer";
        else
            error = openssl_error("TLS receive failed");
        return -1;
    }
    frame_buffer_.insert(frame_buffer_.end(), buffer.begin(), buffer.begin() + count);
    return count;
}

bool WebSocketTransport::upgrade(std::string& error)
{
    std::array<std::uint8_t, 16> nonce {};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        error = openssl_error("could not generate WebSocket key");
        return false;
    }
    const std::string key = base64_encode(nonce);

    std::ostringstream request;
    request << "GET " << target_ << " HTTP/1.1\r\n"
            << "Host: " << host_ << ":" << port_ << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n"
            << "Sec-WebSocket-Protocol: binary\r\n"
            << "\r\n";
    const std::string request_text = request.str();
    if (!raw_send(std::span(reinterpret_cast<const std::uint8_t*>(
                                request_text.data()), request_text.size()),
                  error))
        return false;

    // Read until the end of the response headers; anything after them is the
    // beginning of the frame stream.
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(handshake_timeout_ms);
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            error = "timed out waiting for WebSocket handshake response";
            return false;
        }
        const int count = raw_receive(static_cast<int>(remaining), error);
        if (count < 0)
            return false;
        const std::string_view view(
            reinterpret_cast<const char*>(frame_buffer_.data()), frame_buffer_.size());
        header_end = view.find("\r\n\r\n");
        if (frame_buffer_.size() > 64 * 1024 && header_end == std::string::npos) {
            error = "oversized WebSocket handshake response";
            return false;
        }
    }

    const std::string headers(
        reinterpret_cast<const char*>(frame_buffer_.data()), header_end);
    frame_buffer_.erase(frame_buffer_.begin(),
                        frame_buffer_.begin()
                            + static_cast<std::ptrdiff_t>(header_end + 4));

    std::istringstream lines(headers);
    std::string status_line;
    std::getline(lines, status_line);
    if (status_line.find(" 101") == std::string::npos) {
        error = "WebSocket upgrade refused: " + std::string(trim(status_line));
        return false;
    }

    std::string accept_header;
    std::string line;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string name = to_lower(trim(std::string_view(line).substr(0, colon)));
        if (name == "sec-websocket-accept")
            accept_header = std::string(trim(std::string_view(line).substr(colon + 1)));
    }

    // RFC 6455 section 4.2.2: accept = base64(SHA1(key + GUID)).
    const std::string accept_source = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<std::uint8_t, SHA_DIGEST_LENGTH> digest {};
    SHA1(reinterpret_cast<const unsigned char*>(accept_source.data()),
         accept_source.size(), digest.data());
    if (accept_header != base64_encode(digest)) {
        error = "WebSocket handshake failed: bad Sec-WebSocket-Accept";
        return false;
    }
    return true;
}

bool WebSocketTransport::send_frame(std::uint8_t opcode,
                                    std::span<const std::uint8_t> payload,
                                    std::string& error)
{
    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<std::uint8_t>(0x80 | opcode)); // FIN, no fragmentation

    std::array<std::uint8_t, 4> mask {};
    if (RAND_bytes(mask.data(), static_cast<int>(mask.size())) != 1) {
        error = openssl_error("could not generate frame mask");
        return false;
    }

    const std::uint64_t length = payload.size();
    if (length < 126) {
        frame.push_back(static_cast<std::uint8_t>(0x80 | length));
    } else if (length <= 0xffff) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<std::uint8_t>(length >> 8));
        frame.push_back(static_cast<std::uint8_t>(length));
    } else {
        frame.push_back(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<std::uint8_t>(length >> shift));
    }
    frame.insert(frame.end(), mask.begin(), mask.end());
    for (std::size_t i = 0; i < payload.size(); ++i)
        frame.push_back(static_cast<std::uint8_t>(payload[i] ^ mask[i % 4]));

    return raw_send(frame, error);
}

bool WebSocketTransport::send_all(std::span<const std::uint8_t> bytes,
                                  std::string& error)
{
    if (!open_) {
        error = "WebSocket is not connected";
        return false;
    }
    // One send is one RP_ message from the session, so it maps naturally onto
    // one binary frame, mirroring the HTML5 client.
    return send_frame(opcode_binary, bytes, error);
}

bool WebSocketTransport::drain_frames(std::string& error)
{
    std::size_t offset = 0;
    while (true) {
        const std::size_t available = frame_buffer_.size() - offset;
        if (available < 2)
            break;
        const std::uint8_t* header = frame_buffer_.data() + offset;
        const std::uint8_t opcode = header[0] & 0x0f;
        const bool masked = (header[1] & 0x80) != 0;
        std::uint64_t length = header[1] & 0x7f;
        std::size_t header_size = 2;
        if (length == 126) {
            if (available < 4)
                break;
            length = static_cast<std::uint64_t>(header[2]) << 8 | header[3];
            header_size = 4;
        } else if (length == 127) {
            if (available < 10)
                break;
            length = 0;
            for (int i = 0; i < 8; ++i)
                length = length << 8 | header[2 + i];
            header_size = 10;
        }
        if (masked)
            header_size += 4;
        if (length > max_frame_payload) {
            error = "oversized WebSocket frame";
            return false;
        }
        if (available < header_size + length)
            break;

        const std::uint8_t* payload = header + header_size;
        std::vector<std::uint8_t> unmasked;
        if (masked) {
            // Servers must not mask (RFC 6455 section 5.1), but tolerate it.
            unmasked.assign(payload, payload + length);
            for (std::size_t i = 0; i < unmasked.size(); ++i)
                unmasked[i] ^= header[header_size - 4 + i % 4];
            payload = unmasked.data();
        }

        switch (opcode) {
        case opcode_continuation:
        case opcode_text:
        case opcode_binary:
            incoming_.insert(incoming_.end(), payload, payload + length);
            if (incoming_.size() > max_buffered_bytes) {
                error = "receive buffer overflow";
                return false;
            }
            break;
        case opcode_ping:
            if (!send_frame(opcode_pong,
                            std::span(payload, static_cast<std::size_t>(length)),
                            error))
                return false;
            break;
        case opcode_pong:
            break;
        case opcode_close:
            (void)send_frame(opcode_close, {}, error);
            peer_closed_ = true;
            error = "connection closed by peer";
            return false;
        default:
            error = "unsupported WebSocket frame opcode "
                + std::to_string(opcode);
            return false;
        }
        offset += header_size + static_cast<std::size_t>(length);
    }
    if (offset != 0)
        frame_buffer_.erase(frame_buffer_.begin(),
                            frame_buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    return true;
}

int WebSocketTransport::receive(std::span<std::uint8_t> destination, int timeout_ms,
                                std::string& error)
{
    if (!open_ || peer_closed_) {
        error = "WebSocket is not connected";
        return -1;
    }
    if (incoming_.empty()) {
        const int count = raw_receive(timeout_ms, error);
        if (count < 0)
            return -1;
        if (!drain_frames(error))
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

void WebSocketTransport::close()
{
    if (open_ && !peer_closed_) {
        std::string ignored;
        (void)send_frame(opcode_close, {}, ignored);
    }
    if (tls_ != nullptr) {
        if (open_ && !peer_closed_)
            SSL_shutdown(tls_->session);
        delete tls_;
        tls_ = nullptr;
    }
    socket_.close();
    open_ = false;
    peer_closed_ = false;
    frame_buffer_.clear();
    incoming_.clear();
}

} // namespace haiku_remote
