// Tests for DeBeOS-RDP issue #1 Part 4: cross-platform profile-format parity
// and per-OS config-path resolution.
//
// The point of this suite is that the portable behaviour is verified HERE, on a
// single Linux host, without being on macOS or Windows:
//
//   - config_dir_for() takes the OS as a parameter and reads the environment
//     through a supplied lookup, so each platform's rule is exercised with a
//     fake environment. No #ifdef, no "trust me on the other OS".
//   - the on-disk format is pinned to an exact golden byte string, so any drift
//     in field names, order, or JSON shape fails -- the same bytes a client
//     writes on any OS.
//   - a profile carrying Windows-style backslash paths round-trips byte-for-
//     byte through serialize()/parse(), proving paths are stored verbatim and
//     the format does not depend on the host's path convention.
//
// House rules (CLAUDE.md): assert exact values, never inequalities; pin golden
// vectors by hand rather than reading them back from the serializer; and give
// the environment-resolution tests asymmetric inputs so a swapped branch shows.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/profile_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
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

// Build an EnvLookup over an explicit map so a test drives resolution with a
// fake environment instead of the host's. A missing key reads back as unset.
EnvLookup fake_env(std::map<std::string, std::string> vars)
{
    return [vars = std::move(vars)](const char* name) -> const char* {
        const auto it = vars.find(name);
        return it == vars.end() ? nullptr : it->second.c_str();
    };
}

// ---------------------------------------------------------------------------
// Per-OS config path resolution (driven by a fake environment).

void test_linux_uses_xdg_config_home_when_set()
{
    const auto env = fake_env({{"XDG_CONFIG_HOME", "/xdg/cfg"},
                               {"HOME", "/home/tester"}});
    const auto dir = ProfileStore::config_dir_for(ConfigPlatform::linux_xdg, env);
    check(dir == std::filesystem::path("/xdg/cfg/haiku-remote"),
          "Linux resolves to $XDG_CONFIG_HOME/haiku-remote when XDG is set");
}

void test_linux_falls_back_to_home_config_when_xdg_unset()
{
    const auto env = fake_env({{"HOME", "/home/tester"}});
    const auto dir = ProfileStore::config_dir_for(ConfigPlatform::linux_xdg, env);
    check(dir == std::filesystem::path("/home/tester/.config/haiku-remote"),
          "Linux falls back to $HOME/.config/haiku-remote with no XDG");
}

void test_linux_treats_empty_xdg_as_unset()
{
    // An empty XDG_CONFIG_HOME is not a usable directory; fall back, don't
    // produce "/haiku-remote".
    const auto env = fake_env({{"XDG_CONFIG_HOME", ""}, {"HOME", "/home/tester"}});
    const auto dir = ProfileStore::config_dir_for(ConfigPlatform::linux_xdg, env);
    check(dir == std::filesystem::path("/home/tester/.config/haiku-remote"),
          "an empty XDG_CONFIG_HOME is treated as unset");
}

void test_linux_with_no_home_uses_cwd_relative()
{
    const auto env = fake_env({});
    const auto dir = ProfileStore::config_dir_for(ConfigPlatform::linux_xdg, env);
    check(dir == std::filesystem::path(".") / ".config" / "haiku-remote",
          "Linux with neither XDG nor HOME resolves relative to the cwd");
}

void test_macos_uses_application_support()
{
    const auto env = fake_env({{"HOME", "/Users/tester"},
                               // macOS must ignore XDG even if present.
                               {"XDG_CONFIG_HOME", "/xdg/cfg"}});
    const auto dir = ProfileStore::config_dir_for(ConfigPlatform::macos, env);
    check(dir
              == std::filesystem::path(
                  "/Users/tester/Library/Application Support/Haiku Remote"),
          "macOS resolves to ~/Library/Application Support/Haiku Remote");
    check(dir.filename() == "Haiku Remote",
          "the macOS config dir is the 'Haiku Remote' folder");
}

void test_windows_uses_appdata()
{
    // A realistic APPDATA with backslashes. On a POSIX host backslashes are not
    // path separators, so the whole value is one component; assert structurally
    // (parent + filename) to stay exact regardless of the host separator.
    const auto env =
        fake_env({{"APPDATA", "C:\\Users\\tester\\AppData\\Roaming"}});
    const auto dir = ProfileStore::config_dir_for(ConfigPlatform::windows, env);
    check(dir.filename() == "Haiku Remote",
          "the Windows config dir is the 'Haiku Remote' folder");
    check(dir.parent_path()
              == std::filesystem::path("C:\\Users\\tester\\AppData\\Roaming"),
          "the Windows config dir sits directly under %APPDATA%");
}

void test_windows_without_appdata_is_relative()
{
    const auto env = fake_env({});
    const auto dir = ProfileStore::config_dir_for(ConfigPlatform::windows, env);
    check(dir == std::filesystem::path("Haiku Remote"),
          "Windows with no %APPDATA% resolves to a bare 'Haiku Remote'");
}

void test_config_file_name_is_identical_across_platforms()
{
    const auto env = fake_env({{"XDG_CONFIG_HOME", "/x"},
                               {"HOME", "/h"},
                               {"APPDATA", "C:\\a"}});
    for (ConfigPlatform platform :
         {ConfigPlatform::linux_xdg, ConfigPlatform::macos, ConfigPlatform::windows}) {
        const auto file =
            ProfileStore::config_dir_for(platform, env) / "connections.json";
        check(file.filename() == "connections.json",
              "the library file is connections.json on every platform");
    }
    // And the host wrapper agrees.
    check(ProfileStore::config_file().filename() == "connections.json",
          "config_file() on this host is also connections.json");
}

void test_host_config_dir_honours_real_xdg_override()
{
    // Exercise the actual compiled config_dir() on this (Linux) host through a
    // real environment override -- the production path, not just the pure helper.
    setenv("XDG_CONFIG_HOME", "/tmp/haiku-remote-xdg-probe", 1);
    const auto dir = ProfileStore::config_dir();
    check(dir == std::filesystem::path("/tmp/haiku-remote-xdg-probe/haiku-remote"),
          "config_dir() honours a real XDG_CONFIG_HOME on this host");
    unsetenv("XDG_CONFIG_HOME");
}

// ---------------------------------------------------------------------------
// On-disk format parity.

// A profile with simple, fully-specified values. Its serialization is pinned
// below as a golden byte string; this is the exact format every OS writes.
ConnectionProfile golden_profile()
{
    ConnectionProfile p;
    p.id = "p";
    p.name = "N";
    p.favorite = false;
    p.mode = ConnectionMode::ssh;
    p.host = "h";
    p.remote_port = 10900;
    p.ssh_user = "baron";
    p.ssh_port = 22;
    p.identity_file = "";
    p.local_port = std::nullopt;
    p.width = 1280;
    p.height = 800;
    p.auto_reconnect = true;
    p.known_hosts_file = "";
    p.cookie_source = "";
    p.last_connected_at = 0;
    p.last_error = "";
    return p;
}

void test_serialized_format_is_a_fixed_portable_golden()
{
    // Written by hand from the schema and json.cpp's pretty-printer (two-space
    // indent, ": " after keys, trailing newline). If a field is renamed,
    // reordered, retyped, or the layout drifts, this fails -- the on-disk format
    // is the cross-platform contract, so it is pinned, not derived.
    const std::string expected =
        "{\n"
        "  \"version\": 1,\n"
        "  \"profiles\": [\n"
        "    {\n"
        "      \"id\": \"p\",\n"
        "      \"name\": \"N\",\n"
        "      \"favorite\": false,\n"
        "      \"mode\": \"ssh\",\n"
        "      \"host\": \"h\",\n"
        "      \"remotePort\": 10900,\n"
        "      \"sshUser\": \"baron\",\n"
        "      \"sshPort\": 22,\n"
        "      \"identityFile\": \"\",\n"
        "      \"localPort\": null,\n"
        "      \"width\": 1280,\n"
        "      \"height\": 800,\n"
        "      \"autoReconnect\": true,\n"
        "      \"knownHostsFile\": \"\",\n"
        "      \"cookieSource\": \"\",\n"
        "      \"lastConnectedAt\": 0,\n"
        "      \"lastError\": \"\"\n"
        "    }\n"
        "  ]\n"
        "}\n";
    const std::string actual = ProfileStore::serialize({golden_profile()});
    check(actual == expected,
          "the serialized library matches the pinned portable golden exactly");
}

void test_windows_style_paths_round_trip_byte_for_byte()
{
    // A profile written by a Windows user: backslash identity and known_hosts
    // paths. The format must store these verbatim (JSON-escaped) and read them
    // back unchanged -- the schema does not care which OS wrote it.
    ConnectionProfile p = golden_profile();
    p.id = "win";
    p.name = "Windows box";
    p.identity_file = "C:\\Users\\tester\\.ssh\\id_ed25519";
    p.known_hosts_file = "C:\\Users\\tester\\.ssh\\known_hosts";

    const std::string text = ProfileStore::serialize({p});
    // A backslash is escaped as \\ in JSON; the literal below is a single
    // backslash pair in the file.
    check(contains(text, "C:\\\\Users\\\\tester\\\\.ssh\\\\id_ed25519"),
          "a Windows identity path is stored with JSON-escaped backslashes");

    const ParsedLibrary parsed = ProfileStore::parse(text);
    check(parsed.ok, "a library with Windows paths parses");
    check(parsed.profiles.size() == 1, "the single Windows profile is kept");
    check(parsed.profiles.at(0) == p,
          "a Windows-authored profile round-trips byte-for-byte");
    check(parsed.profiles.at(0).identity_file
              == "C:\\Users\\tester\\.ssh\\id_ed25519",
          "the backslash identity path survives unescaped on read-back");
}

void test_round_trip_is_stable_across_a_reserialize()
{
    // serialize -> parse -> serialize must be a fixed point: the bytes a second
    // writer produces equal the first, so a profile written on one OS and
    // rewritten on another does not churn the file.
    ConnectionProfile p = golden_profile();
    p.identity_file = "~/.ssh/id_ed25519";
    p.local_port = 15900;
    const std::string once = ProfileStore::serialize({p});
    const ParsedLibrary parsed = ProfileStore::parse(once);
    check(parsed.ok, "the first serialization parses");
    const std::string twice = ProfileStore::serialize(parsed.profiles);
    check(once == twice,
          "re-serializing a parsed library reproduces identical bytes");
}

} // namespace

int main()
{
    test_linux_uses_xdg_config_home_when_set();
    test_linux_falls_back_to_home_config_when_xdg_unset();
    test_linux_treats_empty_xdg_as_unset();
    test_linux_with_no_home_uses_cwd_relative();
    test_macos_uses_application_support();
    test_windows_uses_appdata();
    test_windows_without_appdata_is_relative();
    test_config_file_name_is_identical_across_platforms();
    test_host_config_dir_honours_real_xdg_override();
    test_serialized_format_is_a_fixed_portable_golden();
    test_windows_style_paths_round_trip_byte_for_byte();
    test_round_trip_is_stable_across_a_reserialize();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " cross-platform checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " cross-platform checks FAILED\n";
    return 1;
}
