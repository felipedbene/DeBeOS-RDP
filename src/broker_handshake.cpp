#include "haiku_remote/broker_handshake.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>

namespace haiku_remote {

std::vector<std::uint8_t> broker_authenticate_message(std::string_view token)
{
    std::vector<std::uint8_t> message;
    const std::uint32_t total = 6 + 8 + static_cast<std::uint32_t>(token.size());
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
    add_u32(static_cast<std::uint32_t>(token.size()));
    message.insert(message.end(), token.begin(), token.end());
    return message;
}

bool broker_auth_result(std::span<const std::uint8_t> reply, std::string& error)
{
    const auto u16_at = [&](std::size_t offset) {
        return static_cast<std::uint16_t>(reply[offset] | reply[offset + 1] << 8);
    };
    const auto u32_at = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(
            reply[offset] | reply[offset + 1] << 8 | reply[offset + 2] << 16
            | static_cast<std::uint32_t>(reply[offset + 3]) << 24);
    };
    if (reply.size() < rp_auth_result_size || u16_at(0) != rp_auth_result
        || u32_at(2) != rp_auth_result_size) {
        error = "broker sent an unexpected reply to RP_AUTHENTICATE";
        return false;
    }
    const std::uint32_t status = u32_at(6);
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
    case 3:
        // The token was accepted; what failed is entirely on the far side. The
        // broker must read app_server's per-boot session cookie before it can
        // open a session, and it could not: either app_server's listener is not
        // up yet, so no cookie file exists for the port the broker proxies to,
        // or the file is there and unreadable by the user this broker runs as
        // (it is mode 0600, owned by whoever app_server runs as). Distinguished
        // from status 1 deliberately: nothing about the token needs changing,
        // and the numeric fallback below used to implicate it.
        error = "broker accepted the token but could not read app_server's"
                " session cookie, so it has nothing to open a session with"
                " (the remote interface may not be listening yet, or its"
                " session_cookie file is not readable by the user the broker"
                " runs as) -- the remedy is on the server";
        return false;
    default:
        error = "broker reported authentication status "
            + std::to_string(status);
        return false;
    }
}

bool decode_certificate_pin(std::string_view pin,
                            std::array<std::uint8_t, 32>& digest,
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

bool verify_broker_certificate(const BrokerIdentity& identity,
                               const Fingerprint& presented, BrokerCheck& check,
                               ConnectFailure& failure, std::string& error)
{
    // Precedence (see TransportOptions): a pin is checked even alongside
    // --insecure; --insecure accepts anything else; a chain the TLS layer
    // verified against --ca-file is trusted; otherwise known_brokers decides.
    if (!identity.pin_sha256.empty()) {
        std::array<std::uint8_t, 32> expected {};
        if (!decode_certificate_pin(identity.pin_sha256, expected, error))
            return false;
        if (presented != expected) {
            std::ostringstream text;
            text << "certificate pin mismatch: server fingerprint sha256:"
                 << std::hex << std::setfill('0');
            for (const auto byte : presented)
                text << std::setw(2) << static_cast<unsigned>(byte);
            error = text.str();
            return false;
        }
        return true;
    }
    if (identity.insecure || identity.chain_verified)
        return true;

    check = BrokerCheck {};
    check.host = identity.host;
    check.port = identity.port;
    check.presented = presented;
    check.store_file = identity.known_brokers_file.empty()
        ? KnownBrokers::default_file()
        : std::filesystem::path(identity.known_brokers_file);

    std::optional<Fingerprint> seed;
    if (!identity.known_broker_fingerprint.empty()) {
        Fingerprint parsed {};
        if (!parse_fingerprint(identity.known_broker_fingerprint, parsed, error))
            return false;
        seed = parsed;
        bool added = false;
        if (!seed_known_broker(check.store_file, identity.host, identity.port,
                               parsed, added, error)) {
            error = "could not seed known_brokers: " + error;
            return false;
        }
    }

    KnownBrokers store;
    if (!KnownBrokers::load(check.store_file, store, error)) {
        error = "could not read known_brokers: " + error;
        return false;
    }
    check.state = store.lookup(identity.host, identity.port, presented,
                               &check.stored);
    check.vouched = seed && *seed == presented;
    switch (check.state) {
    case BrokerTrust::known:
        return true;
    case BrokerTrust::unknown:
        failure = ConnectFailure::broker_unknown;
        error = "broker certificate unknown: " + check.key() + " presented "
                + display_fingerprint(presented) + ", which is not in "
                + check.store_file.string();
        return false;
    case BrokerTrust::changed:
        failure = ConnectFailure::broker_changed;
        error = "BROKER IDENTIFICATION HAS CHANGED: " + check.key()
                + " presented " + display_fingerprint(presented) + " but "
                + check.store_file.string() + " has "
                + (check.stored ? display_fingerprint(*check.stored) : "?");
        return false;
    }
    return false;
}

} // namespace haiku_remote
