#pragma once

// Persistence for the connection library: load/save connections.json.
//
// Everything a saved library needs that is not the profile schema itself lives
// here -- the on-disk JSON envelope (a schema version plus the profile array),
// the atomic write that never leaves a half-written file, graceful recovery
// from a missing or corrupt file, the per-OS config location, and the
// favorite-first/most-recent ordering the library view wants. The JSON itself
// goes through json.hpp, the one swap-in seam.

#include "haiku_remote/connection_profile.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

// Bump when the on-disk envelope changes shape. A file whose version is newer
// than this is left untouched (not recovered-aside) so a downgrade cannot
// silently discard a library written by a newer client.
//
// Schema version 1 is deliberately *portable*: serialize()/parse() contain no
// platform conditionals, so the connections.json a client writes on one OS is
// byte-identical to what it writes on another for the same profiles, and loads
// unchanged on all three. Only the directory it lives in differs per OS (see
// config_dir_for below). Part 4 confirmed this parity rather than needing to
// change the format, so there is no version bump -- a v1 file written by any
// earlier part still loads.
inline constexpr int profile_schema_version = 1;

// The three per-user config-directory conventions DeBeOS-RDP issue #1 names.
// host_config_platform() reports the one this build targets; config_dir_for()
// applies ANY of them to an explicit environment, so each OS's rule can be
// unit-tested from any host (a Linux CI box can assert the macOS and Windows
// rules without being on those systems).
enum class ConfigPlatform {
    linux_xdg, // $XDG_CONFIG_HOME/haiku-remote, else $HOME/.config/haiku-remote
    macos,     // $HOME/Library/Application Support/Haiku Remote
    windows,   // %APPDATA%\Haiku Remote
};

// A lookup over the environment: returns the value of `name`, or nullptr/empty
// when unset. std::getenv has exactly this shape; a test supplies a fake so the
// resolution is driven by data rather than by the host it runs on.
using EnvLookup = std::function<const char*(const char* name)>;

enum class LoadStatus {
    loaded,    // file read and parsed; `profiles` populated
    missing,   // no file yet -- a fresh, empty library (not an error)
    recovered, // file was empty/corrupt or a newer version; started empty
};

struct LoadResult {
    LoadStatus status = LoadStatus::missing;
    std::vector<ConnectionProfile> profiles;
    std::string message; // human-readable detail for a diagnostic log
    // When a corrupt file was renamed out of the way, where it went. Empty
    // otherwise (including for a newer-version file, which is left in place).
    std::filesystem::path moved_aside;

    [[nodiscard]] bool ok() const { return status != LoadStatus::recovered; }
};

struct SaveResult {
    bool ok = false;
    std::string message;
    // When a profile failed validation, which one and which field. `save` is
    // all-or-nothing: a single invalid profile aborts the write so a bad entry
    // can never corrupt the stored library.
    std::string invalid_profile_id;
    FieldError invalid_field;

    [[nodiscard]] explicit operator bool() const { return ok; }
};

// Outcome of parsing the JSON envelope, with no filesystem involved.
struct ParsedLibrary {
    bool ok = false;
    std::vector<ConnectionProfile> profiles;
    std::string error;
    // The file is intact but its version is newer than we understand; the
    // caller must preserve it rather than move it aside.
    bool newer_version = false;
};

// Write `data` to `path` atomically: a temp file in the same directory, fsync,
// rename over the target, fsync the directory. Shared by connections.json and
// known_brokers so both get the same never-half-written guarantee.
bool atomic_write_file(const std::filesystem::path& path, const std::string& data,
                       std::string& error);

class ProfileStore {
public:
    // Per-OS config directory for the host this build targets. Pure path
    // computation; creates nothing. Resolves against the real environment via
    // config_dir_for(host_config_platform(), std::getenv).
    //   Linux:   $XDG_CONFIG_HOME/haiku-remote, else ~/.config/haiku-remote
    //   macOS:   ~/Library/Application Support/Haiku Remote
    //   Windows: %APPDATA%/Haiku Remote
    [[nodiscard]] static std::filesystem::path config_dir();
    // config_dir()/connections.json. The file name is "connections.json" on
    // every platform; only the directory differs.
    [[nodiscard]] static std::filesystem::path config_file();

    // The config-path convention this build targets (picked at compile time).
    [[nodiscard]] static ConfigPlatform host_config_platform();

    // Resolve a given platform's config directory from an explicit environment.
    // Pure: creates nothing, touches no real environment of its own. An unset
    // or empty variable falls back exactly as config_dir() does -- Linux to
    // $HOME/.config (or "." when even $HOME is unset), macOS to $HOME (or "."),
    // Windows to a bare "Haiku Remote" when %APPDATA% is unset. This is the one
    // place the per-OS rule lives; config_dir() is a thin wrapper over it.
    [[nodiscard]] static std::filesystem::path
    config_dir_for(ConfigPlatform platform, const EnvLookup& env);

    ProfileStore() : file_(config_file()) {}
    explicit ProfileStore(std::filesystem::path file) : file_(std::move(file)) {}

    [[nodiscard]] const std::filesystem::path& file() const { return file_; }

    // Read the library. A missing file yields an empty library and status
    // `missing`; an empty, malformed, or wrong-type file is recovered by moving
    // it aside and starting empty; a newer-version file is left in place and an
    // empty library is returned. Never throws for a bad file.
    [[nodiscard]] LoadResult load() const;

    // Validate every profile, then write atomically: a temp file in the same
    // directory, fsync, rename over the target, fsync the directory. A single
    // invalid profile aborts the whole write and names the field.
    [[nodiscard]] SaveResult save(const std::vector<ConnectionProfile>& profiles) const;

    // Favorite-first, then most-recently-connected first (lastConnectedAt
    // descending). Stable within each group, so equal keys keep input order.
    [[nodiscard]] static std::vector<ConnectionProfile>
    order(std::vector<ConnectionProfile> profiles);

    // Filesystem-free (de)serialization seams, exposed for tests and reuse.
    [[nodiscard]] static std::string
    serialize(const std::vector<ConnectionProfile>& profiles);
    [[nodiscard]] static ParsedLibrary parse(std::string_view text);

private:
    std::filesystem::path file_;
};

} // namespace haiku_remote
