#pragma once

// Trust-on-first-use for the remote_broker's self-signed TLS certificate,
// modelled on OpenSSH's known_hosts.
//
// The broker mints its own certificate on first run. It names only the host's
// internal DNS name and 127.0.0.1, so chain-plus-name verification (--ca-file)
// fails the moment the host is dialed by any other name -- by IP over a VPN,
// say -- and a self-signed certificate cannot be chain-verified in any
// meaningful sense anyway. What can be verified is *continuity*: that the
// certificate presented today is the one the user accepted the first time.
//
//   - known_brokers is a small text file, one `<host>:<port> sha256:<hex>` per
//     line, beside connections.json in the per-OS config directory. The value
//     is the SHA-256 of the certificate's DER encoding -- what the broker
//     writes to broker.fingerprint and what --pin-sha256 compares against. A
//     fingerprint is public, not a secret.
//   - lookup() classifies a presented certificate as known (matches a stored
//     entry), unknown (no entry for host:port) or changed (an entry exists and
//     does not match).
//   - resolve_broker_trust() is the decision table. It never proceeds on an
//     unknown or changed certificate without an explicit decision: an
//     interactive "trust", --trust-new-broker for an UNKNOWN certificate only
//     (StrictHostKeyChecking=accept-new), or an explicit "replace" for a
//     CHANGED one. A changed certificate is refused by default, and the
//     accept-new switch never applies to it.
//
// Nothing here opens a socket or depends on OpenSSL; the transport computes
// the fingerprint and hands it in.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

// A certificate's SHA-256 fingerprint (digest of its DER encoding).
using Fingerprint = std::array<std::uint8_t, 32>;

// "sha256:<64 lowercase hex>" -- the on-disk form.
[[nodiscard]] std::string format_fingerprint(const Fingerprint& fingerprint);
// "SHA256:<64 lowercase hex>" -- the form shown to the user, so it can be
// compared character for character against the broker's broker.fingerprint.
[[nodiscard]] std::string display_fingerprint(const Fingerprint& fingerprint);

// Parse a hex SHA-256 fingerprint: 64 hex digits, either case, optionally
// colon-separated, optionally prefixed "sha256:" / "SHA256:". Exactly the
// broker.fingerprint format plus the forms openssl prints.
bool parse_fingerprint(std::string_view text, Fingerprint& out, std::string& error);

// The store key for an endpoint: lowercased host, then ":port". An IPv6
// literal is bracketed ("[::1]:10902") so the port separator is unambiguous.
[[nodiscard]] std::string broker_key(std::string_view host, std::uint16_t port);

enum class BrokerTrust {
    known,   // a stored entry for host:port matches the presented certificate.
    unknown, // no entry for host:port.
    changed, // an entry exists for host:port and it does not match.
};

// Everything known about one verification, for the decision and for the UI.
struct BrokerCheck {
    BrokerTrust state = BrokerTrust::unknown;
    std::string host;
    std::uint16_t port = 0;
    Fingerprint presented {};
    std::optional<Fingerprint> stored; // set when state == changed (or known).
    std::filesystem::path store_file;
    // The presented certificate equals one fetched out of band over an
    // authenticated channel (the managed route fetches broker.pem over SSH).
    // Informational: shown to the user, never acted on by itself.
    bool vouched = false;

    [[nodiscard]] std::string key() const { return broker_key(host, port); }
};

class KnownBrokers {
public:
    // One line of the file. Comments, blank lines and malformed lines are kept
    // verbatim so a rewrite never destroys what the user wrote by hand.
    struct Line {
        std::string raw;
        bool is_entry = false;
        bool malformed = false;
        std::string key;
        Fingerprint fingerprint {};
    };

    // config_dir_for(host platform)/known_brokers -- the same per-OS directory
    // connections.json lives in, so there is one path rule (ProfileStore's).
    [[nodiscard]] static std::filesystem::path default_file();
    [[nodiscard]] static std::filesystem::path
    file_in(const std::filesystem::path& config_dir);

    // Tolerant parse: never fails. Anything that is not a comment, blank, or a
    // well-formed `<host>:<port> <fingerprint>` line is kept as malformed and
    // ignored for lookups.
    [[nodiscard]] static KnownBrokers parse(std::string_view text);
    [[nodiscard]] std::string serialize() const;

    // Read `file`. A missing file is an empty store (not an error); an
    // unreadable one is an error -- trust decisions fail closed.
    static bool load(const std::filesystem::path& file, KnownBrokers& out,
                     std::string& error);
    // Atomic write (temp file + fsync + rename) into `file`, creating its
    // directory if needed.
    bool save(const std::filesystem::path& file, std::string& error) const;

    [[nodiscard]] BrokerTrust lookup(std::string_view host, std::uint16_t port,
                                     const Fingerprint& presented,
                                     std::optional<Fingerprint>* stored = nullptr) const;
    [[nodiscard]] bool has_entry(std::string_view host, std::uint16_t port) const;

    // Append an entry; refuses (returns false, store unchanged) when any entry
    // for host:port already exists -- adding never overwrites.
    bool add(std::string_view host, std::uint16_t port, const Fingerprint& fingerprint);
    // Drop every entry for host:port and record `fingerprint` in place of the
    // first one (appended when there was none).
    void replace(std::string_view host, std::uint16_t port,
                 const Fingerprint& fingerprint);

    [[nodiscard]] const std::vector<Line>& lines() const { return lines_; }
    [[nodiscard]] std::size_t entry_count() const;
    [[nodiscard]] std::size_t malformed_count() const;

private:
    std::vector<Line> lines_;
};

// --- the decision -----------------------------------------------------------

// What the user said when asked.
enum class TrustChoice {
    reject,  // do not connect.
    trust,   // unknown certificate: accept it and record it.
    replace, // changed certificate: overwrite the stored fingerprint.
};

// Asks the user about a check whose state is unknown or changed. Interactive
// frontends supply one; a null prompt means "non-interactive".
using TrustPrompt = std::function<TrustChoice(const BrokerCheck&)>;

struct TrustPolicy {
    // --trust-new-broker: accept and record an UNKNOWN certificate without
    // asking. Never applies to a changed one.
    bool accept_new = false;
    TrustPrompt prompt;
};

enum class TrustAction {
    proceed,        // known: connect, store untouched.
    store_new,      // unknown, accepted: record it, then connect.
    replace_stored, // changed, explicitly replaced: overwrite, then connect.
    refuse,         // do not connect; `error` says why and what to do.
};

struct TrustResolution {
    TrustAction action = TrustAction::refuse;
    std::string error;
};

[[nodiscard]] TrustResolution resolve_broker_trust(const BrokerCheck& check,
                                                   const TrustPolicy& policy);

// Apply store_new / replace_stored to check.store_file (load, modify, atomic
// save). proceed and refuse are no-ops that return true / false respectively.
bool record_broker_trust(const BrokerCheck& check, TrustAction action,
                         std::string& error);

// Pre-seed: record `fingerprint` for host:port in `file` if there is no entry
// for it yet. An existing entry -- matching or not -- is left alone, so seeding
// can never paper over a changed certificate. `added` says whether it wrote.
bool seed_known_broker(const std::filesystem::path& file, std::string_view host,
                       std::uint16_t port, const Fingerprint& fingerprint,
                       bool& added, std::string& error);

// The user-facing text, shared by the terminal prompt and the GUI so both say
// the same thing.
[[nodiscard]] std::string unknown_broker_text(const BrokerCheck& check);
[[nodiscard]] std::string changed_broker_text(const BrokerCheck& check);

// ssh-style terminal prompt. Unknown: "Are you sure you want to continue
// connecting (yes/no)?" -- only the full word "yes" trusts. Changed: the loud
// warning, then only the full word "replace" replaces; anything else (including
// a bare Enter or EOF) rejects. Pure over the two streams so it is testable.
[[nodiscard]] TrustChoice terminal_trust_prompt(const BrokerCheck& check,
                                                std::istream& in, std::ostream& out);

} // namespace haiku_remote
