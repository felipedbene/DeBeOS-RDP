// Tests for trust on first use of the remote_broker's self-signed certificate
// (known_brokers, modelled on OpenSSH's known_hosts).
//
// Covered here:
//   - fingerprint formatting/parsing against hardcoded golden strings,
//   - the known_brokers file: tolerant parse, byte-for-byte round trip with
//     comments and malformed lines kept, lookup (known / unknown / changed),
//     add-never-overwrites, replace, and the atomic save,
//   - the decision table (resolve_broker_trust): nothing proceeds on an unknown
//     or changed certificate without an explicit decision; --trust-new-broker
//     accepts unknown but never changed; "trust" is not enough to replace,
//   - the ssh-style terminal prompt (full word "yes" / "replace" only),
//   - the GUI trust prompt's hit regions and keys (Enter never accepts),
//   - and, with OpenSSL, an in-process TLS broker on loopback exercising the
//     real WebSocketTransport: unknown -> refused / rejected / trusted ->
//     reconnect matches -> swapped certificate -> changed -> refused, replaced;
//     plus --pin-sha256 bypassing the store and seeding.
//
// House rules (CLAUDE.md): exact values, expectations independent of the code
// under test (golden strings; the integration test fingerprints certificates
// with its own X509_digest call, not the transport's).

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/connect_screen.hpp"
#include "haiku_remote/known_brokers.hpp"
#include "haiku_remote/png_writer.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/profile_store.hpp"
#include "haiku_remote/transport.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef HAIKU_REMOTE_HAVE_WSS
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <atomic>
#include <csignal>
#include <thread>
#include <vector>
#endif

using namespace haiku_remote;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool has(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

struct TempDir {
    std::filesystem::path path;

    TempDir()
    {
        static int counter = 0;
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
               / ("haiku-remote-trust-test-" + std::to_string(now) + "-"
                  + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        std::filesystem::remove_all(path, ec);
    }
};

std::string read_all(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void write_all(const std::filesystem::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

Fingerprint sequence(std::uint8_t start)
{
    Fingerprint f {};
    for (std::size_t i = 0; i < f.size(); ++i)
        f[i] = static_cast<std::uint8_t>(start + i);
    return f;
}

// 00 01 02 ... 1f, written out by hand.
constexpr std::string_view golden_seq0_hex =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
// a0 a1 ... bf.
constexpr std::string_view golden_seqa0_hex =
    "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf";

// ---------------------------------------------------------------------------

void test_fingerprint_format_and_parse()
{
    const Fingerprint f = sequence(0);
    check(format_fingerprint(f) == "sha256:" + std::string(golden_seq0_hex),
          "on-disk fingerprint is sha256:<lowercase hex>");
    check(display_fingerprint(f) == "SHA256:" + std::string(golden_seq0_hex),
          "displayed fingerprint is SHA256:<lowercase hex>");

    Fingerprint parsed {};
    std::string error;
    check(parse_fingerprint(golden_seqa0_hex, parsed, error) && parsed == sequence(0xa0),
          "bare broker.fingerprint hex parses");
    std::string upper(golden_seqa0_hex);
    for (auto& c : upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    check(parse_fingerprint("SHA256:" + upper, parsed, error) && parsed == sequence(0xa0),
          "upper-case hex with an SHA256: prefix parses");
    std::string colons;
    for (std::size_t i = 0; i < golden_seqa0_hex.size(); i += 2) {
        if (i != 0)
            colons += ':';
        colons += golden_seqa0_hex.substr(i, 2);
    }
    check(parse_fingerprint("sha256:" + colons, parsed, error) && parsed == sequence(0xa0),
          "openssl's colon-separated form parses");
    check(parse_fingerprint(std::string(golden_seqa0_hex) + "\n", parsed, error),
          "a trailing newline (as in broker.fingerprint) is tolerated");

    Fingerprint untouched = sequence(7);
    check(!parse_fingerprint(golden_seqa0_hex.substr(0, 62), untouched, error),
          "a short digest is rejected");
    check(!error.empty(), "and says why");
    check(!parse_fingerprint(std::string(golden_seqa0_hex.substr(0, 63)) + "g",
                             untouched, error),
          "a non-hex digit is rejected");
    check(untouched == sequence(7), "a failed parse leaves the output alone");

    check(broker_key("Broker.Example", 10902) == "broker.example:10902",
          "store keys lowercase the host");
    check(broker_key("::1", 10902) == "[::1]:10902",
          "an IPv6 literal is bracketed in the key");
    check(broker_key("10.0.0.9", 443) == "10.0.0.9:443", "an IPv4 key is host:port");
}

void test_store_parse_and_round_trip()
{
    const std::string a = "sha256:" + std::string(golden_seq0_hex);
    const std::string text =
        "# known_brokers -- comment kept\n"
        "\n"
        "10.0.0.9:10902 " + a + "\n"
        "  Broker.Example:10902\tSHA256:" + std::string(golden_seqa0_hex) + "  \n"
        "[::1]:10902 " + a + "\n"
        "no-port " + a + "\n"                 // malformed: no port
        "h:0 " + a + "\n"                     // malformed: port 0
        "h:70000 " + a + "\n"                 // malformed: port out of range
        "h:10902 sha256:zz\n"                 // malformed: bad digest
        "h:10902\n"                           // malformed: one field
        "h:10902 " + a + " extra\n"           // malformed: three fields
        "::1:10902 " + a + "\n"               // malformed: unbracketed v6
        "crlf:10902 " + a + "\r\n";           // CRLF line ending

    const KnownBrokers store = KnownBrokers::parse(text);
    check(store.lines().size() == 13, "every line is kept, one Line each");
    check(store.entry_count() == 4, "four well-formed entries (incl. CRLF)");
    check(store.malformed_count() == 7, "seven malformed lines, kept and ignored");

    // Round trip is byte-for-byte except the CRLF, normalised to LF.
    std::string expected = text;
    expected.replace(expected.size() - 2, 2, "\n");
    check(store.serialize() == expected,
          "serialize() reproduces the file, comments and malformed lines intact");

    const KnownBrokers empty = KnownBrokers::parse("");
    check(empty.lines().empty() && empty.serialize().empty(),
          "an empty file is an empty store");
    check(KnownBrokers::parse("x:1 " + a).serialize() == "x:1 " + a + "\n",
          "a missing final newline is added on rewrite");
}

void test_store_lookup()
{
    const Fingerprint a = sequence(0);
    const Fingerprint b = sequence(0xa0);
    KnownBrokers store = KnownBrokers::parse(
        "broker.example:10902 sha256:" + std::string(golden_seq0_hex) + "\n");

    std::optional<Fingerprint> stored;
    check(store.lookup("broker.example", 10902, a, &stored) == BrokerTrust::known,
          "a matching entry is known");
    check(stored == a, "and reports the stored fingerprint");
    check(store.lookup("BROKER.example", 10902, a) == BrokerTrust::known,
          "host matching is case-insensitive");
    stored.reset();
    check(store.lookup("broker.example", 10902, b, &stored) == BrokerTrust::changed,
          "a different fingerprint for the same host:port is changed");
    check(stored == a, "and reports what was stored");
    stored = b;
    check(store.lookup("broker.example", 10903, a, &stored) == BrokerTrust::unknown,
          "another port of the same host is a different broker: unknown");
    check(!stored.has_value(), "unknown reports no stored fingerprint");
    check(store.lookup("10.0.0.9", 10902, a) == BrokerTrust::unknown,
          "the same broker dialed by IP is its own entry: unknown");

    check(!store.add("broker.example", 10902, b),
          "add() refuses to touch an existing host:port");
    check(store.lookup("broker.example", 10902, a) == BrokerTrust::known,
          "and the existing entry is unchanged");
    check(store.add("10.0.0.9", 10902, b), "add() records a new host:port");
    check(store.lookup("10.0.0.9", 10902, b) == BrokerTrust::known,
          "which is then known");

    // A duplicate for the same key: any matching line makes it known.
    KnownBrokers dup = KnownBrokers::parse(
        "# hdr\nh:1 sha256:" + std::string(golden_seq0_hex) + "\nmid:2 sha256:"
        + std::string(golden_seq0_hex) + "\nh:1 sha256:" + std::string(golden_seqa0_hex)
        + "\n# tail\n");
    check(dup.lookup("h", 1, b) == BrokerTrust::known,
          "any matching line for host:port makes it known");
    dup.replace("h", 1, sequence(0x40));
    check(dup.serialize()
              == "# hdr\nh:1 " + format_fingerprint(sequence(0x40)) + "\nmid:2 sha256:"
                     + std::string(golden_seq0_hex) + "\n# tail\n",
          "replace() keeps the first line's position, drops duplicates, keeps the rest");
    check(dup.lookup("h", 1, a) == BrokerTrust::changed,
          "after replace the old fingerprint is changed");
}

void test_store_files_and_atomic_save()
{
    TempDir dir;
    const auto file = dir.path / "nested" / "config" / "known_brokers";

    KnownBrokers loaded;
    std::string error;
    check(KnownBrokers::load(file, loaded, error) && loaded.lines().empty(),
          "a missing store loads as empty, not as an error");

    KnownBrokers store;
    store.add("h", 10902, sequence(0));
    check(store.save(file, error), "save() creates the directory and writes");
    check(read_all(file) == "h:10902 sha256:" + std::string(golden_seq0_hex) + "\n",
          "the file holds exactly one host:port sha256:<hex> line");
    check(!std::filesystem::exists(file.string() + ".tmp"),
          "no temp file is left behind");

    check(KnownBrokers::load(file, loaded, error)
              && loaded.lookup("h", 10902, sequence(0)) == BrokerTrust::known,
          "a saved store loads back");

    check(KnownBrokers::file_in("/x/y") == std::filesystem::path("/x/y/known_brokers"),
          "the store is named known_brokers inside the config directory");
    check(KnownBrokers::default_file().filename() == "known_brokers"
              && KnownBrokers::default_file().parent_path()
                  == ProfileStore::config_dir(),
          "the default store sits beside connections.json");

#ifndef _WIN32
    if (::geteuid() != 0) {
        // Atomicity, observably: when the write cannot complete, the existing
        // file is left exactly as it was rather than truncated.
        const std::string before = read_all(file);
        ::chmod(file.parent_path().c_str(), 0500);
        KnownBrokers other;
        other.add("z", 1, sequence(0xa0));
        check(!other.save(file, error), "save() into a read-only dir fails");
        check(has(error, "cannot write"), "and says it could not write");
        ::chmod(file.parent_path().c_str(), 0700);
        check(read_all(file) == before,
              "a failed save leaves the previous store intact");

        ::chmod(file.c_str(), 0000);
        check(!KnownBrokers::load(file, loaded, error),
              "an unreadable store is an error (trust fails closed)");
        ::chmod(file.c_str(), 0600);
    } else {
        std::cerr << "note: running as root, skipping permission-based checks\n";
    }
#endif
}

BrokerCheck make_check(BrokerTrust state, const std::filesystem::path& store)
{
    BrokerCheck c;
    c.state = state;
    c.host = "10.0.0.9";
    c.port = 10902;
    c.presented = sequence(0xa0);
    if (state != BrokerTrust::unknown)
        c.stored = state == BrokerTrust::known ? sequence(0xa0) : sequence(0);
    c.store_file = store;
    return c;
}

void test_decision_table()
{
    int asked = 0;
    const auto answering = [&asked](TrustChoice choice) {
        return [&asked, choice](const BrokerCheck&) {
            ++asked;
            return choice;
        };
    };
    const std::filesystem::path store = "/nonexistent/known_brokers";

    TrustPolicy p;
    p.prompt = answering(TrustChoice::trust);
    asked = 0;
    check(resolve_broker_trust(make_check(BrokerTrust::known, store), p).action
              == TrustAction::proceed,
          "known: proceed");
    check(asked == 0, "known: the user is not asked");

    // Unknown.
    asked = 0;
    check(resolve_broker_trust(make_check(BrokerTrust::unknown, store), p).action
              == TrustAction::store_new,
          "unknown + trust: store, then proceed");
    check(asked == 1, "unknown: the user is asked exactly once");
    p.prompt = answering(TrustChoice::reject);
    TrustResolution r = resolve_broker_trust(make_check(BrokerTrust::unknown, store), p);
    check(r.action == TrustAction::refuse, "unknown + reject: abort");
    check(has(r.error, "not to trust"), "and says the user declined");
    p.prompt = answering(TrustChoice::replace);
    check(resolve_broker_trust(make_check(BrokerTrust::unknown, store), p).action
              == TrustAction::refuse,
          "unknown + an answer to the other question: refuse, not guess");

    TrustPolicy none;
    r = resolve_broker_trust(make_check(BrokerTrust::unknown, store), none);
    check(r.action == TrustAction::refuse, "unknown, non-interactive: refuse");
    check(has(r.error, "--trust-new-broker"), "and names --trust-new-broker");
    check(has(r.error, "--known-broker-fingerprint"),
          "and names --known-broker-fingerprint");
    check(has(r.error, "SHA256:" + std::string(golden_seqa0_hex)),
          "and shows the presented fingerprint");

    TrustPolicy accept_new;
    accept_new.accept_new = true;
    check(resolve_broker_trust(make_check(BrokerTrust::unknown, store), accept_new).action
              == TrustAction::store_new,
          "--trust-new-broker accepts an unknown certificate without asking");

    // Changed.
    for (const TrustChoice choice : {TrustChoice::trust, TrustChoice::reject}) {
        p.prompt = answering(choice);
        r = resolve_broker_trust(make_check(BrokerTrust::changed, store), p);
        check(r.action == TrustAction::refuse,
              choice == TrustChoice::trust
                  ? "changed + 'trust' is NOT enough: refuse"
                  : "changed + reject: refuse");
    }
    p.prompt = answering(TrustChoice::replace);
    check(resolve_broker_trust(make_check(BrokerTrust::changed, store), p).action
              == TrustAction::replace_stored,
          "changed + an explicit replace: replace, then proceed");

    r = resolve_broker_trust(make_check(BrokerTrust::changed, store), none);
    check(r.action == TrustAction::refuse, "changed, non-interactive: refuse");
    check(has(r.error, "BROKER IDENTIFICATION HAS CHANGED"), "loudly");
    check(has(r.error, "SHA256:" + std::string(golden_seq0_hex))
              && has(r.error, "SHA256:" + std::string(golden_seqa0_hex)),
          "with both the stored and the presented fingerprint");
    r = resolve_broker_trust(make_check(BrokerTrust::changed, store), accept_new);
    check(r.action == TrustAction::refuse,
          "--trust-new-broker NEVER accepts a changed certificate");
    check(has(r.error, "--trust-new-broker never applies"),
          "and the refusal says so");
}

void test_record_and_seed()
{
    TempDir dir;
    const auto file = dir.path / "known_brokers";
    std::string error;

    const BrokerCheck unknown = make_check(BrokerTrust::unknown, file);
    check(!record_broker_trust(unknown, TrustAction::refuse, error),
          "refuse records nothing");
    check(!std::filesystem::exists(file), "and creates no file");
    check(record_broker_trust(unknown, TrustAction::store_new, error),
          "store_new records");
    check(read_all(file) == "10.0.0.9:10902 sha256:" + std::string(golden_seqa0_hex) + "\n",
          "the accepted fingerprint, keyed by host:port");

    write_all(file, "# mine\n10.0.0.9:10902 sha256:" + std::string(golden_seq0_hex) + "\n");
    const BrokerCheck changed = make_check(BrokerTrust::changed, file);
    check(!record_broker_trust(changed, TrustAction::store_new, error),
          "store_new never overwrites an entry that appeared meanwhile");
    check(record_broker_trust(changed, TrustAction::replace_stored, error),
          "replace_stored overwrites");
    check(read_all(file)
              == "# mine\n10.0.0.9:10902 sha256:" + std::string(golden_seqa0_hex) + "\n",
          "in place, keeping the comment");

    bool added = true;
    const auto seeded = dir.path / "seeded";
    check(seed_known_broker(seeded, "h", 1, sequence(0), added, error) && added,
          "seeding an absent host:port records it");
    check(seed_known_broker(seeded, "h", 1, sequence(0xa0), added, error) && !added,
          "seeding a present host:port with another fingerprint does nothing");
    check(read_all(seeded) == "h:1 sha256:" + std::string(golden_seq0_hex) + "\n",
          "so a seed can never paper over a changed certificate");
}

void test_terminal_prompt()
{
    const BrokerCheck unknown = make_check(BrokerTrust::unknown, "/s/known_brokers");
    const BrokerCheck changed = make_check(BrokerTrust::changed, "/s/known_brokers");
    const auto run = [](const BrokerCheck& c, const std::string& input,
                        std::string* output = nullptr) {
        std::istringstream in(input);
        std::ostringstream out;
        const TrustChoice choice = terminal_trust_prompt(c, in, out);
        if (output != nullptr)
            *output = out.str();
        return choice;
    };

    std::string out;
    check(run(unknown, "yes\n", &out) == TrustChoice::trust, "unknown: 'yes' trusts");
    check(has(out, "The authenticity of broker '10.0.0.9:10902' can't be established."),
          "the unknown prompt names host:port");
    check(has(out, "SHA256:" + std::string(golden_seqa0_hex)),
          "the unknown prompt shows the fingerprint");
    check(has(out, "Are you sure you want to continue connecting (yes/no)? "),
          "the unknown prompt asks ssh's question");
    check(run(unknown, "no\n") == TrustChoice::reject, "unknown: 'no' rejects");
    check(run(unknown, "") == TrustChoice::reject, "unknown: EOF rejects");
    check(run(unknown, "y\n", &out) == TrustChoice::reject,
          "unknown: 'y' is not 'yes' (and EOF after it rejects)");
    check(has(out, "Please type 'yes' or 'no'."), "an unclear answer is re-asked");
    check(run(unknown, "y\nyes\n") == TrustChoice::trust,
          "after re-asking, the full word is accepted");

    check(run(changed, "\n", &out) == TrustChoice::reject,
          "changed: a bare Enter refuses");
    check(has(out, "WARNING: BROKER IDENTIFICATION HAS CHANGED!"),
          "the changed prompt is the loud warning");
    check(has(out, "SHA256:" + std::string(golden_seq0_hex))
              && has(out, "SHA256:" + std::string(golden_seqa0_hex)),
          "the changed prompt shows both fingerprints");
    check(run(changed, "yes\n") == TrustChoice::reject,
          "changed: 'yes' does not replace");
    check(run(changed, "replace\n") == TrustChoice::replace,
          "changed: only the word 'replace' replaces");
}

std::optional<ConnectScreen::Region>
find_region(const ConnectScreen& screen, ConnectScreen::Region::Kind kind)
{
    for (const auto& r : screen.regions())
        if (r.kind == kind)
            return r;
    return std::nullopt;
}

std::filesystem::path output_dir()
{
    const char* tmp = std::getenv("TMPDIR");
    return tmp != nullptr && *tmp != '\0' ? std::filesystem::path(tmp)
                                          : std::filesystem::path("/tmp");
}

void click(ConnectScreen& screen, const ConnectScreen::Region& r)
{
    screen.pointer_press(r.x + r.w / 2, r.y + r.h / 2);
}

void test_gui_prompt()
{
    ConnectionProfile profile;
    profile.name = "Graviton";
    profile.host = "10.0.0.9";
    profile.mode = ConnectionMode::wss;
    ConnectFlow flow(plan_launch(profile));
    std::string png_error;

    // Unknown.
    ConnectScreen ui(flow, "Graviton", 900, 560);
    ui.show_trust_prompt(make_check(BrokerTrust::unknown, "/s/known_brokers"));
    check(ui.trust_prompt_active(), "the prompt is showing");
    write_png(ui.render(), (output_dir() / "rdp-trust-unknown.png").string(), png_error);
    const auto trust = find_region(ui, ConnectScreen::Region::Kind::trust);
    const auto no = find_region(ui, ConnectScreen::Region::Kind::dont_trust);
    check(trust && no, "unknown offers Trust and Don't trust");
    check(!find_region(ui, ConnectScreen::Region::Kind::replace),
          "unknown offers no Replace");
    ui.key(ConnectScreen::Key::enter);
    check(ui.take_action() == ConnectScreen::Action::reject,
          "Enter on the unknown prompt refuses, it never trusts");
    ui.key(ConnectScreen::Key::escape);
    check(ui.take_action() == ConnectScreen::Action::reject, "Escape refuses");
    if (no) {
        click(ui, *no);
        check(ui.take_action() == ConnectScreen::Action::reject,
              "clicking Don't trust refuses");
    }
    if (trust) {
        click(ui, *trust);
        check(ui.take_action() == ConnectScreen::Action::trust,
              "clicking Trust trusts");
    }

    // Changed, at the narrowest size the screen allows.
    for (const int width : {900, 480}) {
        ConnectScreen cui(flow, "Graviton", width, 560);
        cui.show_trust_prompt(make_check(BrokerTrust::changed, "/s/known_brokers"));
        write_png(cui.render(),
                  (output_dir()
                   / ("rdp-trust-changed-" + std::to_string(width) + ".png"))
                      .string(),
                  png_error);
        const auto replace = find_region(cui, ConnectScreen::Region::Kind::replace);
        const auto cancel = find_region(cui, ConnectScreen::Region::Kind::dont_trust);
        check(replace && cancel, "changed offers Cancel and Replace");
        check(!find_region(cui, ConnectScreen::Region::Kind::trust),
              "changed offers no plain Trust");
        if (replace)
            check(replace->x + replace->w <= cui.width() && replace->x >= 0,
                  "the Replace button fits on screen");
        cui.key(ConnectScreen::Key::enter);
        check(cui.take_action() == ConnectScreen::Action::reject,
              "Enter on the changed warning refuses -- Replace is never the default");
        if (replace) {
            click(cui, *replace);
            check(cui.take_action() == ConnectScreen::Action::replace,
                  "only clicking Replace replaces");
        }
        cui.clear_trust_prompt();
        check(!cui.trust_prompt_active(), "the prompt clears");
        cui.render();
        check(!find_region(cui, ConnectScreen::Region::Kind::replace),
              "and its regions go with it");
    }
}

void test_classification_and_arguments()
{
    const ConnectError unknown = classify_connect_failure(
        ConnectFailPoint::transport_broker, ConnectFailure::broker_unknown, "x");
    check(unknown.cause == ConnectErrorCause::broker_cert_unknown,
          "a refused unknown certificate classifies as broker_cert_unknown");
    check(unknown.can_retry, "Retry is offered (it asks again)");
    const ConnectError changed = classify_connect_failure(
        ConnectFailPoint::transport_broker, ConnectFailure::broker_changed, "x");
    check(changed.cause == ConnectErrorCause::broker_cert_changed,
          "a changed certificate classifies as broker_cert_changed");
    check(changed.title == "BROKER IDENTIFICATION HAS CHANGED", "with a loud title");
    check(!changed.can_retry, "and no Retry: Enter on it goes Back");

    TransportOptions parsed;
    const auto feed = [&](std::string_view name, std::string value) {
        return parse_transport_argument(parsed, name, [value] { return value; });
    };
    check(feed("--trust-new-broker", "") && parsed.trust_new_broker,
          "--trust-new-broker is parsed");
    check(feed("--known-brokers", "/k") && parsed.known_brokers_file == "/k",
          "--known-brokers is parsed");
    check(feed("--known-broker-fingerprint", std::string(golden_seq0_hex))
              && parsed.known_broker_fingerprint == golden_seq0_hex,
          "--known-broker-fingerprint is parsed");
    bool threw = false;
    try {
        feed("--known-broker-fingerprint", "abc");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "a malformed --known-broker-fingerprint is a usage error");
    check(has(std::string(transport_usage()), "--trust-new-broker"),
          "--help documents --trust-new-broker");
    check(has(std::string(exit_status_usage()), "  4  "),
          "--help documents exit status 4");
}

#ifdef HAIKU_REMOTE_HAVE_WSS
// ---------------------------------------------------------------------------
// Integration: a TLS "broker" on loopback with swappable certificates.
// ---------------------------------------------------------------------------

struct Identity {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    Fingerprint fingerprint {};
    std::string pem;
};

Identity make_identity(const char* common_name, long serial)
{
    Identity id;
    id.key = EVP_EC_gen("P-256");
    id.cert = X509_new();
    X509_set_version(id.cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(id.cert), serial);
    X509_gmtime_adj(X509_getm_notBefore(id.cert), -60);
    X509_gmtime_adj(X509_getm_notAfter(id.cert), 3600);
    X509_set_pubkey(id.cert, id.key);
    X509_NAME* name = X509_get_subject_name(id.cert);
    // Like the real broker: a name that is NOT what the client dials.
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(common_name),
                               -1, -1, 0);
    X509_set_issuer_name(id.cert, name);
    X509_sign(id.cert, id.key, EVP_sha256());
    unsigned int length = 0;
    X509_digest(id.cert, EVP_sha256(), id.fingerprint.data(), &length);
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, id.cert);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    id.pem.assign(data, static_cast<std::size_t>(size));
    BIO_free(bio);
    return id;
}

class TlsBroker {
public:
    explicit TlsBroker(std::vector<Identity*> identities)
        : identities_(std::move(identities))
    {
        for (Identity* id : identities_) {
            SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
            SSL_CTX_use_certificate(ctx, id->cert);
            SSL_CTX_use_PrivateKey(ctx, id->key);
            contexts_.push_back(ctx);
        }
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listener_, 8);
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~TlsBroker()
    {
        stop_ = true;
        thread_.join();
        ::close(listener_);
        for (SSL_CTX* ctx : contexts_)
            SSL_CTX_free(ctx);
    }

    void use(std::size_t index) { current_ = index; }

    std::uint16_t port = 0;
    std::atomic<int> handshakes {0};
    std::atomic<int> requests {0};      // HTTP upgrade requests received
    std::atomic<int> authenticated {0}; // RP_AUTHENTICATE answered

private:
    static bool read_exact(SSL* ssl, std::uint8_t* out, std::size_t n)
    {
        std::size_t got = 0;
        while (got < n) {
            const int r = SSL_read(ssl, out + got, static_cast<int>(n - got));
            if (r <= 0)
                return false;
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    void handle(int fd)
    {
        SSL* ssl = SSL_new(contexts_[current_]);
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) == 1) {
            ++handshakes;
            session(ssl);
        }
        SSL_free(ssl);
        ::close(fd);
    }

    void session(SSL* ssl)
    {
        std::string request;
        char c = 0;
        while (request.find("\r\n\r\n") == std::string::npos) {
            if (SSL_read(ssl, &c, 1) != 1)
                return; // the client left after the handshake (refused cert)
            request.push_back(c);
            if (request.size() > 8192)
                return;
        }
        ++requests;
        const std::string marker = "Sec-WebSocket-Key: ";
        const auto at = request.find(marker);
        if (at == std::string::npos)
            return;
        const auto end = request.find("\r\n", at);
        const std::string key = request.substr(at + marker.size(),
                                               end - at - marker.size());
        const std::string source = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char digest[SHA_DIGEST_LENGTH];
        SHA1(reinterpret_cast<const unsigned char*>(source.data()), source.size(),
             digest);
        unsigned char accept[64] {};
        EVP_EncodeBlock(accept, digest, SHA_DIGEST_LENGTH);
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: "
            + std::string(reinterpret_cast<char*>(accept)) + "\r\n\r\n";
        SSL_write(ssl, response.data(), static_cast<int>(response.size()));

        // One masked client frame: RP_AUTHENTICATE.
        std::uint8_t header[2];
        if (!read_exact(ssl, header, 2))
            return;
        std::uint64_t length = header[1] & 0x7f;
        if (length == 126) {
            std::uint8_t ext[2];
            if (!read_exact(ssl, ext, 2))
                return;
            length = static_cast<std::uint64_t>(ext[0]) << 8 | ext[1];
        } else if (length == 127) {
            return;
        }
        std::uint8_t mask[4];
        if (!read_exact(ssl, mask, 4))
            return;
        std::vector<std::uint8_t> payload(length);
        if (!read_exact(ssl, payload.data(), payload.size()))
            return;
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= mask[i % 4];
        if (payload.size() < 2 || (payload[0] | payload[1] << 8) != 10)
            return;
        // RP_AUTH_RESULT (11), total 10, status 0, as one binary frame.
        const std::uint8_t reply[] = {0x82, 10, 11, 0, 10, 0, 0, 0, 0, 0, 0, 0};
        SSL_write(ssl, reply, sizeof(reply));
        ++authenticated;
        // Wait for the client's close (or EOF) before tearing down.
        std::uint8_t sink[256];
        while (SSL_read(ssl, sink, sizeof(sink)) > 0) {
        }
    }

    void serve()
    {
        while (!stop_) {
            pollfd p {listener_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0)
                continue;
            const int fd = ::accept(listener_, nullptr, nullptr);
            if (fd >= 0)
                handle(fd);
        }
    }

    std::vector<Identity*> identities_;
    std::vector<SSL_CTX*> contexts_;
    int listener_ = -1;
    std::atomic<std::size_t> current_ {0};
    std::atomic<bool> stop_ {false};
    std::thread thread_;
};

struct Attempt {
    bool ok = false;
    ConnectFailure failure = ConnectFailure::none;
    int exit = 0;
    BrokerCheck check;
    std::string error;
};

Attempt attempt(const TransportOptions& options, const TrustPolicy& policy)
{
    Attempt a;
    std::string error;
    auto transport = make_transport(options, error);
    if (transport == nullptr) {
        a.error = error;
        return a;
    }
    a.ok = connect_with_broker_trust(*transport, policy, error);
    a.failure = transport->connect_failure();
    a.exit = a.ok ? exit_status::ok : connect_exit_status(*transport);
    a.check = transport->broker_check();
    a.error = error;
    transport->close();
    return a;
}

std::string hexdigest(const Fingerprint& f)
{
    return format_fingerprint(f).substr(7);
}

void test_tls_integration()
{
    Identity a = make_identity("debeos-broker.internal", 1);
    Identity b = make_identity("debeos-broker.internal", 2);
    check(a.fingerprint != b.fingerprint, "two distinct test certificates");

    Fingerprint from_pem {};
    std::string error;
    check(fingerprint_pem_certificate(a.pem, from_pem, error)
              && from_pem == a.fingerprint,
          "fingerprint_pem_certificate() matches X509_digest of the DER");
    check(!fingerprint_pem_certificate("-----BEGIN CERTIFICATE-----\nMIIBdummy\n"
                                       "-----END CERTIFICATE-----\n",
                                       from_pem, error),
          "an unparsable PEM is rejected");

    TlsBroker broker({&a, &b});
    TempDir dir;
    const auto store = dir.path / "known_brokers";

    TransportOptions options;
    options.url = "wss://127.0.0.1:" + std::to_string(broker.port) + "/";
    options.token = "token";
    options.known_brokers_file = store.string();
    const std::string key = "127.0.0.1:" + std::to_string(broker.port);
    const std::string line_a = key + " sha256:" + hexdigest(a.fingerprint) + "\n";
    const std::string line_b = key + " sha256:" + hexdigest(b.fingerprint) + "\n";

    int asked = 0;
    TrustChoice answer = TrustChoice::reject;
    TrustPolicy interactive;
    interactive.prompt = [&](const BrokerCheck&) {
        ++asked;
        return answer;
    };
    const TrustPolicy non_interactive;

    // 1. Unknown, nobody to ask: refused after the handshake, before anything
    //    is sent -- no HTTP request ever reaches the broker.
    Attempt r = attempt(options, non_interactive);
    check(!r.ok, "unknown + non-interactive: refused");
    check(r.failure == ConnectFailure::broker_unknown, "as broker_unknown");
    check(r.exit == exit_status::untrusted_broker && r.exit == 4,
          "exit status 4 (untrusted broker)");
    check(r.check.presented == a.fingerprint,
          "the reported fingerprint is the served certificate's");
    check(has(r.error, "--trust-new-broker"), "the error names the flag to pre-trust");
    check(!std::filesystem::exists(store), "nothing was recorded");
    // The server thread counts asynchronously; give it a moment to catch up.
    for (int i = 0; i < 100 && broker.handshakes < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(broker.handshakes == 1 && broker.requests == 0,
          "the TLS handshake ran but nothing was sent over it");

    // 2. Unknown, user says no.
    answer = TrustChoice::reject;
    r = attempt(options, interactive);
    check(!r.ok && asked == 1, "unknown + reject: asked once, refused");
    check(!std::filesystem::exists(store), "and nothing was recorded");

    // 3. Unknown, user trusts: recorded, reconnected, authenticated.
    answer = TrustChoice::trust;
    asked = 0;
    r = attempt(options, interactive);
    check(r.ok, "unknown + trust: connected");
    check(asked == 1, "after asking once");
    check(read_all(store) == line_a, "and recorded host:port sha256:<A>");
    check(broker.authenticated == 1, "the reconnect reached RP_AUTH_RESULT");

    // 4. Reconnect: known, no question.
    asked = 0;
    r = attempt(options, interactive);
    check(r.ok && asked == 0, "known: connects without asking");
    check(r.check.state == BrokerTrust::known, "and reports known");
    check(attempt(options, non_interactive).ok, "known: fine non-interactively too");

    // 5. The broker's certificate is swapped.
    broker.use(1);
    r = attempt(options, non_interactive);
    check(!r.ok && r.failure == ConnectFailure::broker_changed,
          "swapped certificate: broker_changed");
    check(r.exit == exit_status::untrusted_broker, "exit status 4");
    check(r.check.stored == a.fingerprint && r.check.presented == b.fingerprint,
          "with the stored and the presented fingerprint");
    check(has(r.error, "BROKER IDENTIFICATION HAS CHANGED"), "said loudly");
    TrustPolicy accept_new;
    accept_new.accept_new = true;
    r = attempt(options, accept_new);
    check(!r.ok && r.failure == ConnectFailure::broker_changed,
          "--trust-new-broker does not accept a changed certificate");
    answer = TrustChoice::trust;
    r = attempt(options, interactive);
    check(!r.ok, "changed + 'trust': refused");
    answer = TrustChoice::reject;
    r = attempt(options, interactive);
    check(!r.ok, "changed + reject: refused");
    check(read_all(store) == line_a, "the stored fingerprint is untouched by all of that");
    const int authed_before = broker.authenticated;
    answer = TrustChoice::replace;
    r = attempt(options, interactive);
    check(r.ok, "changed + explicit replace: connected");
    check(read_all(store) == line_b, "and B replaced A");
    check(broker.authenticated == authed_before + 1, "the session authenticated");

    // 6. --trust-new-broker on a genuinely unknown broker.
    TransportOptions fresh = options;
    const auto store2 = dir.path / "store2";
    fresh.known_brokers_file = store2.string();
    r = attempt(fresh, accept_new);
    check(r.ok, "--trust-new-broker accepts an unknown certificate");
    check(read_all(store2) == line_b, "and records it");

    // 7. An explicit pin bypasses the store entirely.
    TransportOptions pinned = options;
    const auto store3 = dir.path / "store3";
    pinned.known_brokers_file = store3.string();
    pinned.pin_sha256 = hexdigest(b.fingerprint);
    r = attempt(pinned, non_interactive);
    check(r.ok, "--pin-sha256 matching: connects with an empty store, no prompt");
    check(!std::filesystem::exists(store3), "and the store is neither read nor written");
    pinned.pin_sha256 = hexdigest(a.fingerprint);
    r = attempt(pinned, interactive);
    check(!r.ok && has(r.error, "pin mismatch"), "--pin-sha256 mismatching: refused");
    check(r.failure == ConnectFailure::other, "as a pin failure, not a TOFU one");
    check(!std::filesystem::exists(store3), "still without touching the store");

    // 8. Seeding (what haiku-remote-connect.sh and the managed route do).
    TransportOptions seeded = options;
    const auto store4 = dir.path / "store4";
    seeded.known_brokers_file = store4.string();
    seeded.known_broker_fingerprint = hexdigest(b.fingerprint);
    r = attempt(seeded, non_interactive);
    check(r.ok, "a correct seed connects non-interactively");
    check(r.check.vouched, "and is reported as vouched for");
    check(read_all(store4) == line_b, "and was recorded");
    const auto store5 = dir.path / "store5";
    seeded.known_brokers_file = store5.string();
    seeded.known_broker_fingerprint = hexdigest(a.fingerprint); // wrong seed
    r = attempt(seeded, non_interactive);
    check(!r.ok && r.failure == ConnectFailure::broker_changed,
          "a seed that does not match what is served is refused as changed");
    check(!r.check.vouched, "and not vouched for");
    write_all(store5, line_a);
    seeded.known_broker_fingerprint = hexdigest(b.fingerprint);
    r = attempt(seeded, non_interactive);
    check(!r.ok && r.failure == ConnectFailure::broker_changed,
          "a seed never overrides an existing entry");
    check(r.check.vouched, "though the warning can say the seed vouches for it");
    check(read_all(store5) == line_a, "the stored entry is untouched");

    // 9. --insecure stays the only blanket bypass.
    TransportOptions insecure = options;
    const auto store6 = dir.path / "store6";
    insecure.known_brokers_file = store6.string();
    insecure.insecure = true;
    check(attempt(insecure, non_interactive).ok, "--insecure connects");
    check(!std::filesystem::exists(store6), "without touching the store");

    X509_free(a.cert);
    EVP_PKEY_free(a.key);
    X509_free(b.cert);
    EVP_PKEY_free(b.key);
}
#endif

} // namespace

int main()
{
    test_fingerprint_format_and_parse();
    test_store_parse_and_round_trip();
    test_store_lookup();
    test_store_files_and_atomic_save();
    test_decision_table();
    test_record_and_seed();
    test_terminal_prompt();
    test_gui_prompt();
    test_classification_and_arguments();
#ifdef HAIKU_REMOTE_HAVE_WSS
    // The test broker writes session tickets to clients that may already have
    // hung up on a refused certificate; EPIPE is expected there, not fatal.
    std::signal(SIGPIPE, SIG_IGN);
    // A hang here must fail loudly, not wedge `make test` forever. On DeBeOS /
    // Haiku arm64 a recv() blocked on a socket whose peer then resets it never
    // returns (a kernel bug, DeBeOS issue #605 -- not something the client
    // should paper over), and this suite exercises exactly that shape: the
    // broker refuses a certificate and hangs up mid-handshake. The suite takes
    // under a second on a healthy host; the default deadline is generous, and
    // HAIKU_REMOTE_TRUST_TIMEOUT (seconds) overrides it.
    {
        int seconds = 120;
        if (const char* value = std::getenv("HAIKU_REMOTE_TRUST_TIMEOUT")) {
            const int parsed = std::atoi(value);
            if (parsed > 0)
                seconds = parsed;
        }
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            std::cerr << "FAIL: TLS integration did not finish within "
                      << seconds << "s -- hung (on DeBeOS/Haiku, see kernel"
                      " issue #605: recv() never returns after a peer reset)\n";
            std::cerr.flush();
            std::_Exit(1);
        }).detach();
    }
    test_tls_integration();
#else
    std::cerr << "SKIPPED: TLS integration (built without OpenSSL)\n";
#endif

    if (failures == 0) {
        std::cout << "PASS - " << checks << " broker-trust checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " broker-trust checks FAILED\n";
    return 1;
}
