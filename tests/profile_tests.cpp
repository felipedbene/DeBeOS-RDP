// Tests for the connection library core: ConnectionProfile validation, the
// hand-rolled JSON seam, and ProfileStore load/save/ordering/recovery.
//
// Following the house rules in CLAUDE.md: assert exact values, never
// inequalities; give ordering an asymmetric subject; and keep every expectation
// independent of the code that produces it (the JSON strings and route formats
// below are written down, not read back from the serializer).

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/json.hpp"
#include "haiku_remote/profile_store.hpp"
#include "haiku_remote/surface.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

bool contains(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

void write_text(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

ConnectionProfile sample_profile()
{
    ConnectionProfile profile;
    profile.id = "p-1";
    profile.name = "Graviton box";
    profile.favorite = true;
    profile.mode = ConnectionMode::ssh;
    profile.host = "ec2-203-0-113-7.example";
    profile.remote_port = 10900;
    profile.ssh_user = "baron";
    profile.ssh_port = 22;
    profile.identity_file = "~/.ssh/id_ed25519";
    profile.local_port = 15900;
    profile.width = 1280;
    profile.height = 800;
    profile.auto_reconnect = true;
    profile.known_hosts_file = "~/.config/haiku-remote/known_hosts";
    profile.cookie_source = "/boot/system/settings/remote_desktop/session_cookie.10900";
    profile.last_connected_at = 1700000000;
    profile.last_error = "Local port is already in use.";
    return profile;
}

// A disposable config directory under the system temp dir, removed on scope
// exit so a test run leaves nothing behind.
struct TempDir {
    std::filesystem::path path;

    TempDir()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
               / ("haiku-remote-profile-test-" + std::to_string(now));
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    [[nodiscard]] std::filesystem::path file() const
    {
        return path / "connections.json";
    }
};

// ---------------------------------------------------------------------------

void test_json_round_trips_the_flat_schema()
{
    const std::string text =
        R"({"a":"x","b":12,"c":true,"d":null,"e":[1,2,3],"f":{"g":"h"}})";
    const json::ParseResult parsed = json::parse(text);
    check(parsed.ok, "json parses a flat document");
    const json::Value& v = parsed.value;
    check(v.is_object(), "json root is an object");
    check(v.find("a") != nullptr && v.find("a")->as_string() == "x",
          "json string member reads back");
    check(v.find("b") != nullptr && v.find("b")->as_int() == 12,
          "json integer member reads back");
    check(v.find("c") != nullptr && v.find("c")->as_bool() == true,
          "json bool member reads back");
    check(v.find("d") != nullptr && v.find("d")->is_null(),
          "json null member reads back");
    check(v.find("e") != nullptr && v.find("e")->as_array().size() == 3,
          "json array member reads back");
    check(v.find("f") != nullptr && v.find("f")->find("g") != nullptr
              && v.find("f")->find("g")->as_string() == "h",
          "json nested object reads back");
}

void test_json_preserves_member_order_and_escapes()
{
    json::Object object;
    object.emplace_back("z", json::Value(1));
    object.emplace_back("a", json::Value(std::string("he said \"hi\"\n\t")));
    const std::string out = json::serialize(json::Value(std::move(object)), false);
    // Order is insertion order (z before a), not sorted; and the control/quote
    // characters are escaped exactly.
    check(out == R"({"z":1,"a":"he said \"hi\"\n\t"})",
          "json serialize preserves order and escapes control characters");
}

void test_json_rejects_malformed_input()
{
    check(!json::parse("{").ok, "json rejects an unterminated object");
    check(!json::parse("{\"a\":}").ok, "json rejects a missing value");
    check(!json::parse("[1,2").ok, "json rejects an unterminated array");
    check(!json::parse("nul").ok, "json rejects a truncated literal");
    check(!json::parse("{} junk").ok, "json rejects trailing data");
    check(!json::parse("").ok, "json rejects empty input");
}

void test_profile_round_trips_through_the_store()
{
    std::vector<ConnectionProfile> profiles;
    ConnectionProfile a = sample_profile();
    ConnectionProfile b = sample_profile();
    b.id = "p-2";
    b.name = "Direct box";
    b.favorite = false;
    b.mode = ConnectionMode::direct;
    b.local_port = std::nullopt; // exercise the null localPort path
    b.last_connected_at = 1699000000;
    profiles.push_back(a);
    profiles.push_back(b);

    const std::string text = ProfileStore::serialize(profiles);
    const ParsedLibrary parsed = ProfileStore::parse(text);
    check(parsed.ok, "store parses what it serialized");
    check(parsed.profiles.size() == 2, "round-trip preserves profile count");
    // serialize() does not reorder (order() is applied by save()/load()), so
    // the parsed order equals the input order.
    check(parsed.profiles.at(0) == a, "round-trip preserves profile a exactly");
    check(parsed.profiles.at(1) == b, "round-trip preserves profile b exactly");
    check(!parsed.profiles.at(1).local_port.has_value(),
          "a null localPort round-trips as nullopt, not 0");
}

void test_serialized_library_carries_the_schema_version()
{
    const std::string text = ProfileStore::serialize({sample_profile()});
    check(contains(text, "\"version\": 1"),
          "serialized library records schema version 1");
    check(contains(text, "\"profiles\""),
          "serialized library has a profiles array");
}

void test_validation_accepts_a_good_profile()
{
    const ValidationResult result = validate(sample_profile());
    check(result.ok(), "a well-formed profile validates");
    check(result.errors.empty(), "a valid profile reports no field errors");
}

void test_validation_names_the_offending_field()
{
    ConnectionProfile p = sample_profile();
    p.remote_port = 0;
    check(validate(p).find("remotePort") != nullptr,
          "remote port 0 is rejected and named");

    p = sample_profile();
    p.remote_port = 65536;
    check(validate(p).find("remotePort") != nullptr,
          "remote port 65536 is rejected and named");

    p = sample_profile();
    p.ssh_port = -1;
    check(validate(p).find("sshPort") != nullptr,
          "negative ssh port is rejected and named");

    p = sample_profile();
    p.local_port = 70000;
    check(validate(p).find("localPort") != nullptr,
          "out-of-range local port is rejected and named");

    p = sample_profile();
    p.width = 0;
    check(validate(p).find("width") != nullptr, "width 0 is rejected and named");

    p = sample_profile();
    p.height = Surface::max_dimension + 1;
    check(validate(p).find("height") != nullptr,
          "height past the renderer limit is rejected and named");

    p = sample_profile();
    p.name.clear();
    check(validate(p).find("name") != nullptr, "an empty name is rejected");
}

void test_validation_requires_a_host_in_both_modes()
{
    ConnectionProfile ssh = sample_profile();
    ssh.mode = ConnectionMode::ssh;
    ssh.host.clear();
    const ValidationResult ssh_result = validate(ssh);
    check(ssh_result.find("host") != nullptr,
          "ssh mode requires a host");
    check(ssh_result.find("host") != nullptr
              && contains(ssh_result.find("host")->message, "SSH"),
          "the ssh-mode host error mentions the tunnel");

    ConnectionProfile direct = sample_profile();
    direct.mode = ConnectionMode::direct;
    direct.host.clear();
    check(validate(direct).find("host") != nullptr,
          "direct mode also requires a host");
}

void test_the_local_port_is_optional_when_null()
{
    ConnectionProfile p = sample_profile();
    p.local_port = std::nullopt;
    check(validate(p).ok(), "a null (auto) local port is valid");
}

void test_route_summary_matches_the_editor_format()
{
    ConnectionProfile ssh = sample_profile();
    ssh.mode = ConnectionMode::ssh;
    ssh.host = "host.example";
    ssh.ssh_port = 22;
    ssh.remote_port = 10900;
    ssh.local_port = 15900;
    check(ssh.route_summary()
              == "client -> 127.0.0.1:15900 -> ssh -> host.example:22 "
                 "-> 127.0.0.1:10900",
          "ssh route summary matches the documented format");

    ConnectionProfile autop = ssh;
    autop.local_port = std::nullopt;
    check(autop.route_summary()
              == "client -> 127.0.0.1:auto -> ssh -> host.example:22 "
                 "-> 127.0.0.1:10900",
          "an auto local port renders as 'auto' in the route");

    ConnectionProfile direct = sample_profile();
    direct.mode = ConnectionMode::direct;
    direct.host = "host.example";
    direct.remote_port = 10900;
    check(direct.route_summary() == "client -> host.example:10900",
          "direct route summary matches the documented format");

    // validate() also reports the route for the editor to show live.
    check(validate(direct).route == "client -> host.example:10900",
          "validate() carries the route summary");
}

void test_tilde_is_kept_literal_until_expanded()
{
    ConnectionProfile p = sample_profile();
    p.identity_file = "~/.ssh/id_ed25519";
    const std::string text = ProfileStore::serialize({p});
    check(contains(text, "~/.ssh/id_ed25519"),
          "the stored identity path keeps the tilde literal");
    check(!contains(text, "/home/"),
          "serialization never expands the tilde to a home directory");

    setenv("HOME", "/home/tester", 1);
    check(expand_user_path("~/.ssh/id_ed25519") == "/home/tester/.ssh/id_ed25519",
          "expand_user_path expands a leading ~/ at use");
    check(expand_user_path("~") == "/home/tester",
          "expand_user_path expands a bare ~");
    check(expand_user_path("/absolute/path") == "/absolute/path",
          "expand_user_path leaves an absolute path alone");
    check(expand_user_path("relative/~/path") == "relative/~/path",
          "expand_user_path only touches a leading tilde");
}

void test_the_store_never_persists_a_secret()
{
    // The schema has no field for a passphrase, a private key, or a cookie
    // value, so a serialized library cannot carry one. identityFile and
    // cookieSource are references (a path, a source), not secrets.
    ConnectionProfile p = sample_profile();
    p.identity_file = "~/.ssh/id_ed25519";
    p.cookie_source = "/boot/system/settings/remote_desktop/session_cookie.10900";
    const std::string text = ProfileStore::serialize({p});

    check(contains(text, "identityFile"), "identityFile is stored (as a path)");
    check(contains(text, "cookieSource"), "cookieSource is stored (as a source)");
    for (std::string_view secret :
         {"passphrase", "privateKey", "private_key", "password", "secret",
          "BEGIN OPENSSH", "BEGIN RSA", "-----BEGIN"}) {
        check(!contains(text, secret),
              "serialized library contains no secret material");
    }
}

void test_ordering_is_favorites_first_then_most_recent()
{
    std::vector<ConnectionProfile> profiles;
    auto make = [](std::string id, bool fav, std::int64_t when) {
        ConnectionProfile p;
        p.id = std::move(id);
        p.name = p.id;
        p.host = "h";
        p.favorite = fav;
        p.last_connected_at = when;
        return p;
    };
    // Asymmetric subject: favorites and non-favorites interleaved by recency.
    profiles.push_back(make("old-fav", true, 100));
    profiles.push_back(make("recent-plain", false, 900));
    profiles.push_back(make("new-fav", true, 500));
    profiles.push_back(make("old-plain", false, 200));

    const std::vector<ConnectionProfile> ordered = ProfileStore::order(profiles);
    check(ordered.size() == 4, "ordering keeps every profile");
    check(ordered.at(0).id == "new-fav", "favorite ordered by recency comes first");
    check(ordered.at(1).id == "old-fav", "older favorite comes second");
    check(ordered.at(2).id == "recent-plain",
          "most-recent non-favorite follows the favorites");
    check(ordered.at(3).id == "old-plain", "oldest non-favorite is last");
}

void test_load_on_a_missing_file_is_an_empty_library()
{
    TempDir dir;
    ProfileStore store(dir.file());
    const LoadResult result = store.load();
    check(result.status == LoadStatus::missing,
          "a missing file reports status missing, not an error");
    check(result.ok(), "a missing file is not treated as a failure");
    check(result.profiles.empty(), "a missing file yields an empty library");
    check(result.moved_aside.empty(), "a missing file moves nothing aside");
}

void test_atomic_save_then_load_round_trips_on_disk()
{
    TempDir dir;
    ProfileStore store(dir.file());
    std::vector<ConnectionProfile> profiles = {sample_profile()};
    const SaveResult saved = store.save(profiles);
    check(saved.ok, "a valid library saves");
    check(std::filesystem::exists(dir.file()), "the connections file now exists");
    check(!std::filesystem::exists(
              std::filesystem::path(dir.file().string() + ".tmp")),
          "the temp file is gone after an atomic rename");

    const LoadResult loaded = store.load();
    check(loaded.status == LoadStatus::loaded, "the saved library loads back");
    check(loaded.profiles.size() == 1, "the saved profile count round-trips");
    check(loaded.profiles.at(0) == sample_profile(),
          "the saved profile round-trips through the filesystem exactly");
}

void test_save_refuses_an_invalid_profile_and_names_the_field()
{
    TempDir dir;
    ProfileStore store(dir.file());
    ConnectionProfile bad = sample_profile();
    bad.remote_port = 0;
    const SaveResult result = store.save({bad});
    check(!result.ok, "save refuses a profile that fails validation");
    check(result.invalid_field.field == "remotePort",
          "the save failure names the offending field");
    check(result.invalid_profile_id == bad.id,
          "the save failure names the offending profile");
    check(!std::filesystem::exists(dir.file()),
          "a refused save writes no file at all");
}

void test_recovery_moves_a_corrupt_file_aside()
{
    TempDir dir;
    write_text(dir.file(), "{ this is not json");
    ProfileStore store(dir.file());
    const LoadResult result = store.load();
    check(result.status == LoadStatus::recovered,
          "a corrupt file is recovered, not loaded");
    check(result.profiles.empty(), "recovery starts from an empty library");
    check(!result.moved_aside.empty(), "recovery records where the bad file went");
    check(std::filesystem::exists(result.moved_aside),
          "the corrupt file is preserved under its moved-aside name");
    check(!std::filesystem::exists(dir.file()),
          "the corrupt file no longer occupies the canonical path");
}

void test_recovery_handles_an_empty_file()
{
    TempDir dir;
    write_text(dir.file(), "");
    ProfileStore store(dir.file());
    const LoadResult result = store.load();
    check(result.status == LoadStatus::recovered,
          "an empty file is recovered");
    check(!result.moved_aside.empty(), "the empty file is moved aside");
}

void test_a_newer_version_file_is_preserved_not_discarded()
{
    TempDir dir;
    write_text(dir.file(), R"({"version":99,"profiles":[]})");
    ProfileStore store(dir.file());
    const LoadResult result = store.load();
    check(result.status == LoadStatus::recovered,
          "a newer-version file yields an empty library");
    check(result.moved_aside.empty(),
          "a newer-version file is left in place, never moved aside");
    check(std::filesystem::exists(dir.file()),
          "a newer-version file survives a load by an older client");
}

void test_unknown_fields_and_missing_fields_are_tolerated()
{
    // Forward/backward compatibility: an unknown key is ignored, and a missing
    // key falls back to its default rather than failing the parse.
    const std::string text =
        R"({"version":1,"profiles":[{"id":"x","name":"n","host":"h","future":42}]})";
    const ParsedLibrary parsed = ProfileStore::parse(text);
    check(parsed.ok, "a profile with an unknown field still parses");
    check(parsed.profiles.size() == 1, "the profile is kept");
    check(parsed.profiles.at(0).remote_port == 10900,
          "a missing remotePort falls back to the default");
    check(parsed.profiles.at(0).ssh_user == "baron",
          "a missing sshUser falls back to the default");
    check(parsed.profiles.at(0).mode == ConnectionMode::ssh,
          "a missing mode falls back to ssh");
}

} // namespace

int main()
{
    test_json_round_trips_the_flat_schema();
    test_json_preserves_member_order_and_escapes();
    test_json_rejects_malformed_input();
    test_profile_round_trips_through_the_store();
    test_serialized_library_carries_the_schema_version();
    test_validation_accepts_a_good_profile();
    test_validation_names_the_offending_field();
    test_validation_requires_a_host_in_both_modes();
    test_the_local_port_is_optional_when_null();
    test_route_summary_matches_the_editor_format();
    test_tilde_is_kept_literal_until_expanded();
    test_the_store_never_persists_a_secret();
    test_ordering_is_favorites_first_then_most_recent();
    test_load_on_a_missing_file_is_an_empty_library();
    test_atomic_save_then_load_round_trips_on_disk();
    test_save_refuses_an_invalid_profile_and_names_the_field();
    test_recovery_moves_a_corrupt_file_aside();
    test_recovery_handles_an_empty_file();
    test_a_newer_version_file_is_preserved_not_discarded();
    test_unknown_fields_and_missing_fields_are_tolerated();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " profile checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " profile checks failed\n";
    return 1;
}
