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
#include "haiku_remote/json.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

// Bump when the on-disk envelope or the meaning of a stored value changes. A
// file whose version is newer than this is left untouched (not recovered-aside)
// so a downgrade cannot silently discard a library written by a newer client.
//
// The format is deliberately *portable*: serialize()/parse() contain no
// platform conditionals, so the connections.json a client writes on one OS is
// byte-identical to what it writes on another for the same profiles, and loads
// unchanged on all three. Only the directory it lives in differs per OS (see
// config_dir_for below).
//
// Versions:
//   1  the original schema. Its "direct" mode was planned as broker first,
//      then the SSH tunnel.
//   2  "direct" means issue #1's plain TCP connection to host:remotePort, and
//      that broker-then-tunnel route is stored as "auto". parse() migrates a v1
//      file in memory (migrate_profile_object): every v1 "direct" becomes
//      "auto", so no saved profile changes the route it takes. The bump is what
//      keeps a v1-only client from misreading a v2 file: it sees a newer
//      version and leaves the file alone, instead of taking a v2 "direct" for
//      broker-then-tunnel and dropping the "auto" it does not know.
inline constexpr int profile_schema_version = 2;

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
    // Unknown top-level members of the envelope, for ConnectionLibrary to hand
    // back to save() so a newer client's additions survive this one.
    ExtraFields extra_fields;
    // The schema version the file was written with (loaded only). Older than
    // profile_schema_version means parse() migrated it in memory.
    std::int64_t migrated_from = 0;

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
    // Unknown top-level envelope members (see LoadResult::extra_fields).
    ExtraFields extra_fields;
    // The version the text declared, and how many profiles migration changed.
    std::int64_t version = 0;
    std::size_t migrated = 0;
};

// Outcome of importing a profile export. `ok` means the text was a readable
// profile envelope; an individual profile that fails validation is skipped and
// named in `skipped` rather than failing the whole import.
struct ImportResult {
    bool ok = false;
    std::vector<ConnectionProfile> profiles;
    std::vector<std::string> skipped; // "<name>: <field> <message>[; ...]"
    std::string error;
};

// Migrate one profile object, as parsed from a file of schema `from_version`,
// to profile_schema_version, in place. Returns how many members it changed.
// Edits only what a version bump redefined -- for v1 -> v2, a "mode" of
// "direct" becomes "auto" -- and never adds, removes or reorders a member, so
// unknown members survive exactly. Exposed for the migration tests.
std::size_t migrate_profile_object(json::Value& profile, std::int64_t from_version);

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
    // `envelope_extra` are unknown top-level members to write back unchanged
    // (LoadResult::extra_fields); per-profile unknown members travel inside
    // each ConnectionProfile::extra_fields.
    [[nodiscard]] SaveResult save(const std::vector<ConnectionProfile>& profiles,
                                  const ExtraFields& envelope_extra = {}) const;

    // Favorite-first, then most-recently-connected first (lastConnectedAt
    // descending). Stable within each group, so equal keys keep input order.
    [[nodiscard]] static std::vector<ConnectionProfile>
    order(std::vector<ConnectionProfile> profiles);

    // Filesystem-free (de)serialization seams, exposed for tests and reuse.
    [[nodiscard]] static std::string
    serialize(const std::vector<ConnectionProfile>& profiles,
              const ExtraFields& envelope_extra = {});
    [[nodiscard]] static ParsedLibrary parse(std::string_view text);

    // Profile export/import without secrets (issue #1, Persistence). An export
    // is an ordinary schema-v1 envelope -- so it is also a valid
    // connections.json -- holding only the bookmarks: last-connected time, last
    // error and every unknown member are stripped. A profile cannot carry a
    // secret by construction (see ConnectionProfile); dropping unknown members
    // means a newer client's field cannot leak out through an older one.
    // Import applies the same stripping and validates each profile, skipping
    // and naming the invalid ones. Ids are kept; what a collision means is the
    // caller's decision (ConnectionLibrary::import_profiles re-ids them).
    [[nodiscard]] static std::string
    export_text(const std::vector<ConnectionProfile>& profiles);
    [[nodiscard]] static ImportResult import_text(std::string_view text);
    // File forms; export_file writes atomically, like save().
    [[nodiscard]] static SaveResult
    export_file(const std::filesystem::path& path,
                const std::vector<ConnectionProfile>& profiles);
    [[nodiscard]] static ImportResult import_file(const std::filesystem::path& path);

private:
    std::filesystem::path file_;
};

} // namespace haiku_remote
