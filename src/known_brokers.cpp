#include "haiku_remote/known_brokers.hpp"

#include "haiku_remote/profile_store.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <system_error>

namespace haiku_remote {

namespace {

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

std::string hex(const Fingerprint& fingerprint)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const auto byte : fingerprint) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

// Parse "<host>:<port>" (or "[v6]:<port>") into the normalised key. Returns
// false for anything else.
bool parse_key(std::string_view text, std::string& key)
{
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size())
        return false;
    std::string_view host = text.substr(0, colon);
    const std::string_view port_text = text.substr(colon + 1);
    unsigned int port = 0;
    const auto [end, status] = std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), port);
    if (status != std::errc() || end != port_text.data() + port_text.size()
        || port == 0 || port > 65535)
        return false;
    if (host.front() == '[') {
        if (host.size() < 3 || host.back() != ']')
            return false;
        host = host.substr(1, host.size() - 2);
    } else if (host.find(':') != std::string_view::npos) {
        return false; // an unbracketed IPv6 literal is ambiguous.
    }
    for (const char c : host) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '[' || c == ']')
            return false;
    }
    key = broker_key(host, static_cast<std::uint16_t>(port));
    return true;
}

} // namespace

std::string format_fingerprint(const Fingerprint& fingerprint)
{
    return "sha256:" + hex(fingerprint);
}

std::string display_fingerprint(const Fingerprint& fingerprint)
{
    return "SHA256:" + hex(fingerprint);
}

bool parse_fingerprint(std::string_view text, Fingerprint& out, std::string& error)
{
    text = trim(text);
    if (text.size() >= 7) {
        std::string prefix(text.substr(0, 7));
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        if (prefix == "sha256:")
            text.remove_prefix(7);
    }
    std::string digits;
    digits.reserve(64);
    for (const char c : text) {
        if (c != ':')
            digits.push_back(c);
    }
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Fingerprint parsed {};
    bool valid = digits.size() == 64;
    for (std::size_t i = 0; valid && i < 32; ++i) {
        const int high = nibble(digits[2 * i]);
        const int low = nibble(digits[2 * i + 1]);
        if (high < 0 || low < 0)
            valid = false;
        else
            parsed[i] = static_cast<std::uint8_t>(high << 4 | low);
    }
    if (!valid) {
        error = "a broker fingerprint must be a SHA-256 digest: 64 hex digits"
                " (optionally prefixed sha256: and/or colon-separated), as in"
                " the broker's broker.fingerprint";
        return false;
    }
    out = parsed;
    return true;
}

std::string broker_key(std::string_view host, std::uint16_t port)
{
    std::string lowered(host);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    if (lowered.find(':') != std::string::npos)
        lowered = "[" + lowered + "]";
    return lowered + ":" + std::to_string(port);
}

// ---------------------------------------------------------------------------
// KnownBrokers
// ---------------------------------------------------------------------------

std::filesystem::path KnownBrokers::file_in(const std::filesystem::path& config_dir)
{
    return config_dir / "known_brokers";
}

std::filesystem::path KnownBrokers::default_file()
{
    return file_in(ProfileStore::config_dir());
}

KnownBrokers KnownBrokers::parse(std::string_view text)
{
    KnownBrokers store;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view raw = text.substr(start, end - start);
        if (!raw.empty() && raw.back() == '\r')
            raw.remove_suffix(1);
        start = end + 1;

        Line line;
        line.raw = std::string(raw);
        const std::string_view body = trim(raw);
        if (!body.empty() && body.front() != '#') {
            // Exactly two whitespace-separated fields.
            std::istringstream fields {std::string(body)};
            std::string key_text;
            std::string fingerprint_text;
            std::string extra;
            fields >> key_text >> fingerprint_text;
            const bool two_fields = !key_text.empty() && !fingerprint_text.empty()
                                    && !(fields >> extra);
            std::string key;
            std::string ignored;
            Fingerprint fingerprint {};
            if (two_fields && parse_key(key_text, key)
                && parse_fingerprint(fingerprint_text, fingerprint, ignored)) {
                line.is_entry = true;
                line.key = key;
                line.fingerprint = fingerprint;
            } else {
                line.malformed = true;
            }
        }
        store.lines_.push_back(std::move(line));
    }
    return store;
}

std::string KnownBrokers::serialize() const
{
    std::string out;
    for (const Line& line : lines_) {
        out += line.raw;
        out += '\n';
    }
    return out;
}

bool KnownBrokers::load(const std::filesystem::path& file, KnownBrokers& out,
                        std::string& error)
{
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        if (ec) {
            error = "cannot check " + file.string() + ": " + ec.message();
            return false;
        }
        out = KnownBrokers {};
        return true;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot read " + file.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        error = "cannot read " + file.string();
        return false;
    }
    out = parse(buffer.str());
    return true;
}

bool KnownBrokers::save(const std::filesystem::path& file, std::string& error) const
{
    std::error_code ec;
    if (!file.parent_path().empty())
        std::filesystem::create_directories(file.parent_path(), ec);
    if (!atomic_write_file(file, serialize(), error)) {
        error = "cannot write " + file.string() + ": " + error;
        return false;
    }
    return true;
}

BrokerTrust KnownBrokers::lookup(std::string_view host, std::uint16_t port,
                                 const Fingerprint& presented,
                                 std::optional<Fingerprint>* stored) const
{
    const std::string key = broker_key(host, port);
    std::optional<Fingerprint> first;
    for (const Line& line : lines_) {
        if (!line.is_entry || line.key != key)
            continue;
        if (line.fingerprint == presented) {
            if (stored != nullptr)
                *stored = line.fingerprint;
            return BrokerTrust::known;
        }
        if (!first)
            first = line.fingerprint;
    }
    if (stored != nullptr)
        *stored = first;
    return first ? BrokerTrust::changed : BrokerTrust::unknown;
}

bool KnownBrokers::has_entry(std::string_view host, std::uint16_t port) const
{
    const std::string key = broker_key(host, port);
    return std::any_of(lines_.begin(), lines_.end(), [&](const Line& line) {
        return line.is_entry && line.key == key;
    });
}

bool KnownBrokers::add(std::string_view host, std::uint16_t port,
                       const Fingerprint& fingerprint)
{
    if (has_entry(host, port))
        return false;
    Line line;
    line.is_entry = true;
    line.key = broker_key(host, port);
    line.fingerprint = fingerprint;
    line.raw = line.key + " " + format_fingerprint(fingerprint);
    lines_.push_back(std::move(line));
    return true;
}

void KnownBrokers::replace(std::string_view host, std::uint16_t port,
                           const Fingerprint& fingerprint)
{
    const std::string key = broker_key(host, port);
    Line fresh;
    fresh.is_entry = true;
    fresh.key = key;
    fresh.fingerprint = fingerprint;
    fresh.raw = key + " " + format_fingerprint(fingerprint);

    std::vector<Line> kept;
    bool placed = false;
    for (Line& line : lines_) {
        if (line.is_entry && line.key == key) {
            if (!placed) {
                kept.push_back(fresh);
                placed = true;
            }
            continue;
        }
        kept.push_back(std::move(line));
    }
    if (!placed)
        kept.push_back(std::move(fresh));
    lines_ = std::move(kept);
}

std::size_t KnownBrokers::entry_count() const
{
    return static_cast<std::size_t>(std::count_if(
        lines_.begin(), lines_.end(), [](const Line& l) { return l.is_entry; }));
}

std::size_t KnownBrokers::malformed_count() const
{
    return static_cast<std::size_t>(std::count_if(
        lines_.begin(), lines_.end(), [](const Line& l) { return l.malformed; }));
}

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

namespace {

std::string refuse_unknown_text(const BrokerCheck& check)
{
    return "the authenticity of broker " + check.key()
           + " can't be established (certificate "
           + display_fingerprint(check.presented)
           + ") and there is no one to ask, so the connection was refused."
             " Verify the fingerprint against the broker's broker.fingerprint,"
             " then connect interactively and answer yes, pass"
             " --trust-new-broker to accept and record it, or pre-seed it with"
             " --known-broker-fingerprint <sha256> (it is recorded in "
           + check.store_file.string() + ")";
}

std::string refuse_changed_text(const BrokerCheck& check)
{
    return "BROKER IDENTIFICATION HAS CHANGED for " + check.key()
           + ": the stored certificate fingerprint is "
           + (check.stored ? display_fingerprint(*check.stored) : "(none)")
           + " but the broker presented " + display_fingerprint(check.presented)
           + ". The connection was refused. If the broker's certificate was"
             " legitimately regenerated, remove the line for " + check.key()
           + " from " + check.store_file.string()
           + " (or connect interactively and choose to replace it);"
             " --trust-new-broker never applies to a changed certificate";
}

} // namespace

TrustResolution resolve_broker_trust(const BrokerCheck& check,
                                     const TrustPolicy& policy)
{
    TrustResolution r;
    switch (check.state) {
    case BrokerTrust::known:
        r.action = TrustAction::proceed;
        return r;

    case BrokerTrust::unknown:
        if (policy.accept_new) {
            r.action = TrustAction::store_new;
            return r;
        }
        if (!policy.prompt) {
            r.action = TrustAction::refuse;
            r.error = refuse_unknown_text(check);
            return r;
        }
        // Only an explicit "trust" accepts. "replace" is not an answer to this
        // question and is treated as a refusal rather than guessed at.
        if (policy.prompt(check) == TrustChoice::trust) {
            r.action = TrustAction::store_new;
            return r;
        }
        r.action = TrustAction::refuse;
        r.error = "you chose not to trust the certificate presented by broker "
                  + check.key() + " (" + display_fingerprint(check.presented)
                  + "); nothing was recorded";
        return r;

    case BrokerTrust::changed:
        // accept_new deliberately plays no part here.
        if (!policy.prompt) {
            r.action = TrustAction::refuse;
            r.error = refuse_changed_text(check);
            return r;
        }
        // Only an explicit "replace" overwrites. A "trust" -- the answer to the
        // other question -- must not be enough to replace a stored identity.
        if (policy.prompt(check) == TrustChoice::replace) {
            r.action = TrustAction::replace_stored;
            return r;
        }
        r.action = TrustAction::refuse;
        r.error = refuse_changed_text(check);
        return r;
    }
    r.action = TrustAction::refuse;
    r.error = "unrecognised broker trust state";
    return r;
}

bool record_broker_trust(const BrokerCheck& check, TrustAction action,
                         std::string& error)
{
    if (action == TrustAction::proceed)
        return true;
    if (action == TrustAction::refuse) {
        error = "refusing to record a certificate the user did not accept";
        return false;
    }
    KnownBrokers store;
    if (!KnownBrokers::load(check.store_file, store, error))
        return false;
    if (action == TrustAction::store_new) {
        if (!store.add(check.host, check.port, check.presented)) {
            // Someone recorded an entry since we looked. Do not overwrite it.
            if (store.lookup(check.host, check.port, check.presented)
                == BrokerTrust::known)
                return true;
            error = "an entry for " + check.key() + " appeared in "
                    + check.store_file.string()
                    + " while you were deciding; not overwriting it";
            return false;
        }
    } else {
        store.replace(check.host, check.port, check.presented);
    }
    return store.save(check.store_file, error);
}

bool seed_known_broker(const std::filesystem::path& file, std::string_view host,
                       std::uint16_t port, const Fingerprint& fingerprint,
                       bool& added, std::string& error)
{
    added = false;
    KnownBrokers store;
    if (!KnownBrokers::load(file, store, error))
        return false;
    if (!store.add(host, port, fingerprint))
        return true; // an entry exists; seeding never overrides it.
    if (!store.save(file, error))
        return false;
    added = true;
    return true;
}

std::string unknown_broker_text(const BrokerCheck& check)
{
    return "The authenticity of broker '" + check.key()
           + "' can't be established.\nIts certificate fingerprint is "
           + display_fingerprint(check.presented) + ".";
}

std::string changed_broker_text(const BrokerCheck& check)
{
    return "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
           "@    WARNING: BROKER IDENTIFICATION HAS CHANGED!          @\n"
           "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
           "IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!\n"
           "Someone could be eavesdropping on you right now"
           " (man-in-the-middle attack)!\n"
           "It is also possible that the broker's certificate was just"
           " regenerated.\n"
           "Broker:               " + check.key() + "\n"
           "Stored fingerprint:   "
           + (check.stored ? display_fingerprint(*check.stored) : "(none)") + "\n"
           "Presented fingerprint: " + display_fingerprint(check.presented) + "\n"
           "Stored in: " + check.store_file.string();
}

TrustChoice terminal_trust_prompt(const BrokerCheck& check, std::istream& in,
                                  std::ostream& out)
{
    const auto read_answer = [&]() -> std::string {
        std::string answer;
        if (!std::getline(in, answer))
            return {};
        return std::string(trim(answer));
    };

    if (check.state == BrokerTrust::unknown) {
        out << unknown_broker_text(check) << '\n';
        if (check.vouched)
            out << "This matches the certificate fetched over SSH from the host.\n";
        for (;;) {
            out << "Are you sure you want to continue connecting (yes/no)? "
                << std::flush;
            const std::string answer = read_answer();
            if (answer == "yes")
                return TrustChoice::trust;
            if (answer == "no" || !in)
                return TrustChoice::reject;
            out << "Please type 'yes' or 'no'.\n";
        }
    }
    if (check.state == BrokerTrust::changed) {
        out << changed_broker_text(check) << '\n';
        if (check.vouched)
            out << "The presented certificate matches the one fetched over SSH"
                   " from the host, which is consistent with the broker's"
                   " certificate having been regenerated.\n";
        out << "To replace the stored fingerprint and connect, type 'replace';"
               " anything else aborts: "
            << std::flush;
        return read_answer() == "replace" ? TrustChoice::replace
                                          : TrustChoice::reject;
    }
    return TrustChoice::reject;
}

} // namespace haiku_remote
