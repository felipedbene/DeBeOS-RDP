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
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

// Bump when the on-disk envelope changes shape. A file whose version is newer
// than this is left untouched (not recovered-aside) so a downgrade cannot
// silently discard a library written by a newer client.
inline constexpr int profile_schema_version = 1;

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

class ProfileStore {
public:
    // Per-OS config directory. Pure path computation; creates nothing.
    //   Linux:   $XDG_CONFIG_HOME/haiku-remote, else ~/.config/haiku-remote
    //   macOS:   ~/Library/Application Support/Haiku Remote
    //   Windows: %APPDATA%/Haiku Remote
    [[nodiscard]] static std::filesystem::path config_dir();
    // config_dir()/connections.json
    [[nodiscard]] static std::filesystem::path config_file();

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
