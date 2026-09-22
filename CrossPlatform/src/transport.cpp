#include "haiku_remote/transport.hpp"

#include "haiku_remote/tcp_socket.hpp"
#ifdef HAIKU_REMOTE_HAVE_WSS
#include "haiku_remote/websocket.hpp"
#endif

#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace haiku_remote {
namespace {

// The classic raw TCP connection to app_server's remote interface. Used on
// loopback or through an SSH tunnel; carries no authentication of its own.
class TcpTransport final : public Transport {
public:
    TcpTransport(std::string host, std::uint16_t port)
        : host_(std::move(host))
        , port_(port)
    {
    }

    bool connect(std::string& error) override
    {
        return socket_.connect(host_, port_, error);
    }

    bool send_all(std::span<const std::uint8_t> bytes, std::string& error) override
    {
        return socket_.send_all(bytes, error);
    }

    int receive(std::span<std::uint8_t> destination, int timeout_ms,
                std::string& error) override
    {
        return socket_.receive(destination, timeout_ms, error);
    }

    void close() override { socket_.close(); }

    [[nodiscard]] std::string describe() const override
    {
        return host_ + ":" + std::to_string(port_);
    }

private:
    std::string host_;
    std::uint16_t port_;
    TcpSocket socket_;
};

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::uint16_t port = 0;
    std::string target; // path + query, at least "/"
};

bool parse_url(std::string_view url, ParsedUrl& parsed, std::string& error)
{
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        error = "URL has no scheme: " + std::string(url);
        return false;
    }
    parsed.scheme = std::string(url.substr(0, scheme_end));
    for (auto& character : parsed.scheme)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

    std::string_view rest = url.substr(scheme_end + 3);
    const auto authority_end = rest.find_first_of("/?");
    std::string_view authority = rest.substr(0, authority_end);
    if (authority_end == std::string_view::npos)
        parsed.target = "/";
    else if (rest[authority_end] == '?')
        parsed.target = "/" + std::string(rest.substr(authority_end));
    else
        parsed.target = std::string(rest.substr(authority_end));

    std::string_view host = authority;
    std::string_view port_text;
    if (!authority.empty() && authority.front() == '[') {
        const auto bracket = authority.find(']');
        if (bracket == std::string_view::npos) {
            error = "unterminated IPv6 literal in URL";
            return false;
        }
        host = authority.substr(1, bracket - 1);
        if (bracket + 1 < authority.size()) {
            if (authority[bracket + 1] != ':') {
                error = "malformed authority in URL";
                return false;
            }
            port_text = authority.substr(bracket + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            host = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
        }
    }
    if (host.empty()) {
        error = "URL has no host: " + std::string(url);
        return false;
    }
    parsed.host = std::string(host);

    if (!port_text.empty()) {
        unsigned int port = 0;
        const auto [pointer, status] = std::from_chars(
            port_text.data(), port_text.data() + port_text.size(), port);
        if (status != std::errc() || pointer != port_text.data() + port_text.size()
            || port == 0 || port > 65535) {
            error = "invalid port in URL: " + std::string(port_text);
            return false;
        }
        parsed.port = static_cast<std::uint16_t>(port);
    }
    return true;
}

} // namespace

bool parse_transport_argument(TransportOptions& options, std::string_view argument,
                              const std::function<std::string()>& value)
{
    if (argument == "--host") {
        options.host = value();
    } else if (argument == "--port") {
        const std::string text = value();
        unsigned int port = 0;
        const auto [pointer, status]
            = std::from_chars(text.data(), text.data() + text.size(), port);
        if (status != std::errc() || pointer != text.data() + text.size()
            || port == 0 || port > 65535)
            throw std::runtime_error("invalid port: " + text);
        options.port = static_cast<std::uint16_t>(port);
    } else if (argument == "--url") {
        options.url = value();
    } else if (argument == "--token") {
        options.token = value();
    } else if (argument == "--token-file") {
        // A token on the command line is readable by any local process through
        // `ps` and lands in shell history; reading it from the broker's own
        // 0600 token file avoids both. graviton/scripts/rdcapture.py offers the
        // same option, so keep the two instruments interchangeable.
        const std::string path = value();
        std::ifstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("cannot read --token-file " + path);
        std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
        // The broker writes the token followed by a newline.
        while (!text.empty()
               && (text.back() == '\n' || text.back() == '\r'
                   || text.back() == ' ' || text.back() == '\t'))
            text.pop_back();
        if (text.empty())
            throw std::runtime_error("--token-file " + path + " is empty");
        options.token = std::move(text);
    } else if (argument == "--pin-sha256") {
        options.pin_sha256 = value();
    } else if (argument == "--ca-file") {
        options.ca_file = value();
    } else if (argument == "--insecure") {
        options.insecure = true;
    } else {
        return false;
    }
    return true;
}

std::string_view transport_usage()
{
    return " [--host HOST] [--port PORT]\n"
           "  [--url tcp://|ws://|wss://HOST[:PORT][/PATH]]\n"
           "  [--token TOKEN | --token-file FILE]\n"
           "  [--pin-sha256 DIGEST] [--ca-file FILE.pem] [--insecure]";
}

std::unique_ptr<Transport> make_transport(const TransportOptions& options,
                                          std::string& error)
{
    if (options.url.empty())
        return std::make_unique<TcpTransport>(options.host, options.port);

    ParsedUrl parsed;
    if (!parse_url(options.url, parsed, error))
        return nullptr;

    if (parsed.scheme == "tcp") {
        return std::make_unique<TcpTransport>(
            parsed.host, parsed.port != 0 ? parsed.port : options.port);
    }
    if (parsed.scheme == "ws" || parsed.scheme == "wss") {
        const bool secure = parsed.scheme == "wss";
#ifdef HAIKU_REMOTE_HAVE_WSS
        const std::uint16_t port = parsed.port != 0 ? parsed.port
                                                    : (secure ? 443 : 80);
        return std::make_unique<WebSocketTransport>(
            parsed.host, port, parsed.target, secure, options);
#else
        (void)secure;
        error = "this build has no WebSocket support (OpenSSL development"
                " files were not available)";
        return nullptr;
#endif
    }
    error = "unsupported URL scheme: " + parsed.scheme;
    return nullptr;
}

} // namespace haiku_remote
