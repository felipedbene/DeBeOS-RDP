#include "haiku_remote/profile_store.hpp"

#include "haiku_remote/json.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace haiku_remote {

namespace {

// The profile members this client understands. Anything else in a profile
// object came from a newer client and is preserved verbatim.
constexpr std::string_view known_profile_keys[] = {
    "id",           "name",           "favorite",     "mode",
    "host",         "remotePort",     "sshUser",      "sshPort",
    "identityFile", "localPort",      "width",        "height",
    "autoReconnect", "knownHostsFile", "cookieSource", "lastConnectedAt",
    "lastError",    "brokerPort",
};

// The envelope members this client understands.
constexpr std::string_view known_envelope_keys[] = {"version", "profiles"};

template <std::size_t N>
bool is_known(std::string_view key, const std::string_view (&known)[N])
{
    return std::find(std::begin(known), std::end(known), key) != std::end(known);
}

// The members of `object` that are not in `known`, as compact JSON, in order.
template <std::size_t N>
ExtraFields collect_extra_fields(const json::Value& object,
                                 const std::string_view (&known)[N])
{
    ExtraFields extra;
    for (const auto& [key, value] : object.as_object()) {
        if (!is_known(key, known))
            extra.emplace_back(key, json::serialize(value, false));
    }
    return extra;
}

// Append preserved members after the known ones. A name that collides with a
// known member is skipped so it can never shadow a field this client owns, and
// text that does not parse (only possible if the in-memory value was edited) is
// skipped rather than written out as something that is not JSON.
template <std::size_t N>
void append_extra_fields(json::Object& object, const ExtraFields& extra,
                         const std::string_view (&known)[N])
{
    for (const auto& [key, text] : extra) {
        if (is_known(key, known))
            continue;
        json::ParseResult parsed = json::parse(text);
        if (parsed.ok)
            object.emplace_back(key, std::move(parsed.value));
    }
}

json::Value to_json(const ConnectionProfile& profile)
{
    json::Object object;
    object.emplace_back("id", json::Value(profile.id));
    object.emplace_back("name", json::Value(profile.name));
    object.emplace_back("favorite", json::Value(profile.favorite));
    object.emplace_back("mode", json::Value(std::string(mode_name(profile.mode))));
    object.emplace_back("host", json::Value(profile.host));
    object.emplace_back("remotePort", json::Value(profile.remote_port));
    object.emplace_back("sshUser", json::Value(profile.ssh_user));
    object.emplace_back("sshPort", json::Value(profile.ssh_port));
    object.emplace_back("identityFile", json::Value(profile.identity_file));
    object.emplace_back("localPort", profile.local_port
                                         ? json::Value(*profile.local_port)
                                         : json::Value(nullptr));
    object.emplace_back("width", json::Value(profile.width));
    object.emplace_back("height", json::Value(profile.height));
    object.emplace_back("autoReconnect", json::Value(profile.auto_reconnect));
    object.emplace_back("knownHostsFile", json::Value(profile.known_hosts_file));
    object.emplace_back("cookieSource", json::Value(profile.cookie_source));
    object.emplace_back("lastConnectedAt", json::Value(profile.last_connected_at));
    object.emplace_back("lastError", json::Value(profile.last_error));
    // Written only when it is not the default, so a file that never had it is
    // written back byte for byte (the goldens in profile_tests pin that).
    if (profile.broker_port != default_broker_port)
        object.emplace_back("brokerPort", json::Value(profile.broker_port));
    append_extra_fields(object, profile.extra_fields, known_profile_keys);
    return json::Value(std::move(object));
}

ConnectionProfile from_json(const json::Value& value)
{
    ConnectionProfile profile;
    if (const auto* f = value.find("id"))
        profile.id = f->as_string();
    if (const auto* f = value.find("name"))
        profile.name = f->as_string();
    if (const auto* f = value.find("favorite"))
        profile.favorite = f->as_bool(profile.favorite);
    if (const auto* f = value.find("mode")) {
        if (auto mode = mode_from_name(f->as_string()))
            profile.mode = *mode;
    }
    if (const auto* f = value.find("host"))
        profile.host = f->as_string();
    if (const auto* f = value.find("remotePort"))
        profile.remote_port = static_cast<int>(f->as_int(profile.remote_port));
    if (const auto* f = value.find("sshUser"))
        profile.ssh_user = f->as_string(profile.ssh_user);
    if (const auto* f = value.find("sshPort"))
        profile.ssh_port = static_cast<int>(f->as_int(profile.ssh_port));
    if (const auto* f = value.find("identityFile"))
        profile.identity_file = f->as_string();
    if (const auto* f = value.find("localPort")) {
        if (f->is_number())
            profile.local_port = static_cast<int>(f->as_int());
        else
            profile.local_port = std::nullopt;
    }
    if (const auto* f = value.find("width"))
        profile.width = static_cast<int>(f->as_int(profile.width));
    if (const auto* f = value.find("height"))
        profile.height = static_cast<int>(f->as_int(profile.height));
    if (const auto* f = value.find("autoReconnect"))
        profile.auto_reconnect = f->as_bool(profile.auto_reconnect);
    if (const auto* f = value.find("knownHostsFile"))
        profile.known_hosts_file = f->as_string();
    if (const auto* f = value.find("cookieSource"))
        profile.cookie_source = f->as_string();
    if (const auto* f = value.find("lastConnectedAt"))
        profile.last_connected_at = f->as_int(profile.last_connected_at);
    if (const auto* f = value.find("lastError"))
        profile.last_error = f->as_string();
    if (const auto* f = value.find("brokerPort"))
        profile.broker_port = static_cast<int>(f->as_int(profile.broker_port));
    profile.extra_fields = collect_extra_fields(value, known_profile_keys);
    return profile;
}

} // namespace

std::size_t migrate_profile_object(json::Value& profile, std::int64_t from_version)
{
    if (from_version >= 2 || !profile.is_object())
        return 0;
    // v1 -> v2. Schema v1's "direct" was planned as broker-then-tunnel
    // (profile_launch.cpp before v2); v2 gives "direct" issue #1's meaning, a
    // plain TCP connection. Rewriting it to "auto" keeps every saved v1
    // profile on the route it already took. Only that one member changes: the
    // object is edited in place, so member order and every other member --
    // unknown ones included -- are untouched.
    const json::Value* mode = profile.find("mode");
    if (mode == nullptr || !mode->is_string() || mode->as_string() != "direct")
        return 0;
    profile.set("mode", json::Value(std::string("auto"))); // replaced in place
    return 1;
}

namespace {

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::filesystem::path move_aside(const std::filesystem::path& file)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(now).count();
    std::filesystem::path base = file;
    base += ".corrupt-" + std::to_string(seconds);

    std::error_code ec;
    std::filesystem::path candidate = base;
    for (int suffix = 1; std::filesystem::exists(candidate, ec); ++suffix) {
        candidate = base;
        candidate += "-" + std::to_string(suffix);
    }
    std::filesystem::rename(file, candidate, ec);
    if (ec)
        return {};
    return candidate;
}

} // namespace

bool atomic_write_file(const std::filesystem::path& path, const std::string& data,
                       std::string& error)
{
#ifndef _WIN32
    const std::string temp = path.string() + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        error = "cannot open temp file: " + std::string(std::strerror(errno));
        return false;
    }
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const ssize_t written = ::write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            error = "write failed: " + std::string(std::strerror(errno));
            ::close(fd);
            ::unlink(temp.c_str());
            return false;
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0) {
        error = "fsync failed: " + std::string(std::strerror(errno));
        ::close(fd);
        ::unlink(temp.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        error = "close failed: " + std::string(std::strerror(errno));
        ::unlink(temp.c_str());
        return false;
    }
    if (::rename(temp.c_str(), path.string().c_str()) != 0) {
        error = "rename failed: " + std::string(std::strerror(errno));
        ::unlink(temp.c_str());
        return false;
    }
    // fsync the containing directory so the rename itself is durable.
    const std::string dir =
        path.parent_path().empty() ? "." : path.parent_path().string();
    const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }
    return true;
#else
    // Windows wants MoveFileExW(temp, path,
    // MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) for an atomic,
    // durable replace. Until that is wired up, a temp-write plus
    // std::filesystem::rename keeps the same temp-then-swap data path.
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot open temp file";
            return false;
        }
        out << data;
        out.flush();
        if (!out) {
            error = "write failed";
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        error = "rename failed: " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
#endif
}

ConfigPlatform ProfileStore::host_config_platform()
{
#if defined(_WIN32)
    return ConfigPlatform::windows;
#elif defined(__APPLE__)
    return ConfigPlatform::macos;
#else
    return ConfigPlatform::linux_xdg;
#endif
}

std::filesystem::path ProfileStore::config_dir_for(ConfigPlatform platform,
                                                   const EnvLookup& env)
{
    // Read a variable as a string, treating "unset" and "set but empty" alike
    // (an empty XDG_CONFIG_HOME or APPDATA is not a usable directory).
    const auto value = [&env](const char* name) -> std::string {
        const char* v = env ? env(name) : nullptr;
        return (v != nullptr && *v != '\0') ? std::string(v) : std::string();
    };

    switch (platform) {
        case ConfigPlatform::windows: {
            const std::string appdata = value("APPDATA");
            if (!appdata.empty())
                return std::filesystem::path(appdata) / "Haiku Remote";
            return std::filesystem::path("Haiku Remote");
        }
        case ConfigPlatform::macos: {
            const std::string home = value("HOME");
            const std::filesystem::path base =
                home.empty() ? std::filesystem::path(".") : std::filesystem::path(home);
            return base / "Library" / "Application Support" / "Haiku Remote";
        }
        case ConfigPlatform::linux_xdg:
            break;
    }
    // linux_xdg (and the default).
    const std::string xdg = value("XDG_CONFIG_HOME");
    if (!xdg.empty())
        return std::filesystem::path(xdg) / "haiku-remote";
    const std::string home = value("HOME");
    const std::filesystem::path base =
        home.empty() ? std::filesystem::path(".") : std::filesystem::path(home);
    return base / ".config" / "haiku-remote";
}

std::filesystem::path ProfileStore::config_dir()
{
    return config_dir_for(host_config_platform(),
                          [](const char* name) { return std::getenv(name); });
}

std::filesystem::path ProfileStore::config_file()
{
    return config_dir() / "connections.json";
}

std::vector<ConnectionProfile>
ProfileStore::order(std::vector<ConnectionProfile> profiles)
{
    std::stable_sort(profiles.begin(), profiles.end(),
                     [](const ConnectionProfile& a, const ConnectionProfile& b) {
                         if (a.favorite != b.favorite)
                             return a.favorite; // favorites first
                         return a.last_connected_at > b.last_connected_at;
                     });
    return profiles;
}

std::string ProfileStore::serialize(const std::vector<ConnectionProfile>& profiles,
                                    const ExtraFields& envelope_extra)
{
    json::Object root;
    root.emplace_back("version", json::Value(profile_schema_version));
    json::Array array;
    array.reserve(profiles.size());
    for (const auto& profile : profiles)
        array.push_back(to_json(profile));
    root.emplace_back("profiles", json::Value(std::move(array)));
    append_extra_fields(root, envelope_extra, known_envelope_keys);
    return json::serialize(json::Value(std::move(root)), true);
}

ParsedLibrary ProfileStore::parse(std::string_view text)
{
    ParsedLibrary out;
    json::ParseResult parsed = json::parse(text);
    if (!parsed.ok) {
        out.error = parsed.error;
        return out;
    }
    const json::Value& root = parsed.value;
    if (!root.is_object()) {
        out.error = "root is not an object";
        return out;
    }
    const json::Value* version = root.find("version");
    if (version == nullptr || !version->is_number()) {
        out.error = "missing or invalid schema version";
        return out;
    }
    const std::int64_t found = version->as_int();
    if (found > profile_schema_version) {
        out.error = "schema version " + std::to_string(found)
                    + " is newer than supported (" + std::to_string(profile_schema_version) + ")";
        out.newer_version = true;
        return out;
    }
    const json::Value* profiles = root.find("profiles");
    if (profiles == nullptr || !profiles->is_array()) {
        out.error = "missing profiles array";
        return out;
    }
    out.version = found;
    for (const auto& element : profiles->as_array()) {
        if (!element.is_object())
            continue;
        json::Value current = element;
        out.migrated += migrate_profile_object(current, found);
        out.profiles.push_back(from_json(current));
    }
    out.extra_fields = collect_extra_fields(root, known_envelope_keys);
    out.ok = true;
    return out;
}

LoadResult ProfileStore::load() const
{
    LoadResult result;
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec) || ec) {
        result.status = LoadStatus::missing;
        result.message = "no connection library yet";
        return result;
    }

    const ParsedLibrary parsed = parse(read_file(file_));
    if (parsed.ok) {
        result.status = LoadStatus::loaded;
        result.profiles = order(parsed.profiles);
        result.extra_fields = parsed.extra_fields;
        result.migrated_from = parsed.version;
        result.message =
            "loaded " + std::to_string(result.profiles.size()) + " profile(s)";
        // The file itself is left as it is: the next save writes the current
        // version. Until then an older client still reads it as it was.
        if (parsed.version < profile_schema_version)
            result.message += " (schema v" + std::to_string(parsed.version)
                              + ", migrated to v"
                              + std::to_string(profile_schema_version) + ": "
                              + std::to_string(parsed.migrated)
                              + " direct profile(s) are now auto)";
        return result;
    }

    result.status = LoadStatus::recovered;
    if (parsed.newer_version) {
        // Intact but from the future: never discard it on a downgrade.
        result.message = "left newer-version library in place: " + parsed.error;
        return result;
    }
    result.moved_aside = move_aside(file_);
    result.message = "recovered from an unreadable library (" + parsed.error + ")";
    return result;
}

SaveResult ProfileStore::save(const std::vector<ConnectionProfile>& profiles,
                              const ExtraFields& envelope_extra) const
{
    SaveResult result;
    for (const auto& profile : profiles) {
        // The schema, not this build: a wss profile saved by a TLS build must
        // survive a save from one without TLS, which only refuses to USE it.
        const ValidationResult validation = validate(profile, /*wss_available=*/true);
        if (!validation.ok()) {
            result.ok = false;
            result.invalid_profile_id = profile.id;
            result.invalid_field = validation.errors.front();
            const std::string label = profile.name.empty() ? profile.id : profile.name;
            result.message = "profile '" + label + "' is invalid: field "
                             + result.invalid_field.field + ": "
                             + result.invalid_field.message;
            return result;
        }
    }

    const std::vector<ConnectionProfile> ordered = order(profiles);
    const std::string text = serialize(ordered, envelope_extra);

    std::error_code ec;
    if (!file_.parent_path().empty())
        std::filesystem::create_directories(file_.parent_path(), ec);

    if (!atomic_write_file(file_, text, result.message)) {
        result.ok = false;
        return result;
    }
    result.ok = true;
    result.message = "saved " + std::to_string(ordered.size()) + " profile(s)";
    return result;
}

namespace {

// What leaves (or enters) the machine through export/import: the bookmark
// itself. History (last connection, last error) is machine-local, and unknown
// members are dropped because this client cannot vouch that a newer client's
// field is not a secret. Paths (identity file, known_hosts, cookie source) are
// kept verbatim, tilde included: they name where a secret lives, never its
// bytes, and the identity file is never opened here.
ConnectionProfile portable_copy(ConnectionProfile profile)
{
    profile.last_connected_at = 0;
    profile.last_error.clear();
    profile.extra_fields.clear();
    return profile;
}

} // namespace

std::string ProfileStore::export_text(const std::vector<ConnectionProfile>& profiles)
{
    std::vector<ConnectionProfile> portable;
    portable.reserve(profiles.size());
    for (const auto& profile : profiles)
        portable.push_back(portable_copy(profile));
    return serialize(portable);
}

ImportResult ProfileStore::import_text(std::string_view text)
{
    ImportResult result;
    const ParsedLibrary parsed = parse(text);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }
    for (const auto& profile : parsed.profiles) {
        ConnectionProfile portable = portable_copy(profile);
        const ValidationResult validation = validate(portable);
        if (!validation.ok()) {
            std::string reason =
                portable.name.empty() ? std::string("(unnamed)") : portable.name;
            reason += ":";
            for (const auto& error : validation.errors)
                reason += " " + error.field + " " + error.message + ";";
            reason.pop_back();
            result.skipped.push_back(std::move(reason));
            continue;
        }
        result.profiles.push_back(std::move(portable));
    }
    result.ok = true;
    return result;
}

SaveResult ProfileStore::export_file(const std::filesystem::path& path,
                                     const std::vector<ConnectionProfile>& profiles)
{
    SaveResult result;
    std::error_code ec;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), ec);
    if (!atomic_write_file(path, export_text(profiles), result.message))
        return result;
    result.ok = true;
    result.message = "exported " + std::to_string(profiles.size()) + " profile(s)";
    return result;
}

ImportResult ProfileStore::import_file(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        ImportResult result;
        result.error = "no such file: " + path.string();
        return result;
    }
    return import_text(read_file(path));
}

} // namespace haiku_remote
