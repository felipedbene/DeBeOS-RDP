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
    return profile;
}

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

bool atomic_write(const std::filesystem::path& path, const std::string& data,
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

} // namespace

std::filesystem::path ProfileStore::config_dir()
{
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return std::filesystem::path(appdata) / "Haiku Remote";
    return std::filesystem::path("Haiku Remote");
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = (home && *home) ? home : ".";
    return base / "Library" / "Application Support" / "Haiku Remote";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "haiku-remote";
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = (home && *home) ? home : ".";
    return base / ".config" / "haiku-remote";
#endif
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

std::string ProfileStore::serialize(const std::vector<ConnectionProfile>& profiles)
{
    json::Object root;
    root.emplace_back("version", json::Value(profile_schema_version));
    json::Array array;
    array.reserve(profiles.size());
    for (const auto& profile : profiles)
        array.push_back(to_json(profile));
    root.emplace_back("profiles", json::Value(std::move(array)));
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
    for (const auto& element : profiles->as_array()) {
        if (element.is_object())
            out.profiles.push_back(from_json(element));
    }
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
        result.message =
            "loaded " + std::to_string(result.profiles.size()) + " profile(s)";
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

SaveResult ProfileStore::save(const std::vector<ConnectionProfile>& profiles) const
{
    SaveResult result;
    for (const auto& profile : profiles) {
        const ValidationResult validation = validate(profile);
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
    const std::string text = serialize(ordered);

    std::error_code ec;
    if (!file_.parent_path().empty())
        std::filesystem::create_directories(file_.parent_path(), ec);

    if (!atomic_write(file_, text, result.message)) {
        result.ok = false;
        return result;
    }
    result.ok = true;
    result.message = "saved " + std::to_string(ordered.size()) + " profile(s)";
    return result;
}

} // namespace haiku_remote
