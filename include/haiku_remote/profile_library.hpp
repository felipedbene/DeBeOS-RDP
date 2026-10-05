#pragma once

// The connection library's in-memory model and its CRUD operations, sitting one
// layer above ProfileStore. ProfileStore knows how to read and write the JSON
// file; ConnectionLibrary owns the vector of profiles the application edits and
// turns "add / edit / duplicate / delete / mark-connected / search / order"
// into operations on it, each of which leaves the model consistent and ready to
// save().
//
// It has no UI and no protocol: LibraryScreen drives it, and Part 2's managed
// tunnel will call mark_connected()/set_last_error() on it, but neither is
// visible from here. Keeping the data model this side of the UI is what lets the
// same library back an SDL window, an X11 window, or a test with no window at
// all.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/profile_store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

class ConnectionLibrary {
public:
    // Default store (ProfileStore::config_file()); or an injected one, which is
    // how tests point the library at a scratch directory.
    ConnectionLibrary() = default;
    explicit ConnectionLibrary(ProfileStore store) : store_(std::move(store)) {}

    [[nodiscard]] const ProfileStore& store() const { return store_; }

    // Read the backing file into the model, replacing whatever was held. The
    // returned status distinguishes a fresh library (missing) from a recovered
    // one so the UI can show the recovery note. Profiles are kept in the store's
    // canonical favorite-first order.
    LoadResult load();

    // Persist the current model. All-or-nothing: a single invalid profile
    // aborts the write and names the field (see ProfileStore::save).
    [[nodiscard]] SaveResult save() const;

    // The model in storage order (favorite-first, then most-recent). A copy, so
    // a caller iterating it can freely mutate the library.
    [[nodiscard]] const std::vector<ConnectionProfile>& profiles() const
    {
        return profiles_;
    }
    [[nodiscard]] std::size_t size() const { return profiles_.size(); }
    [[nodiscard]] bool empty() const { return profiles_.empty(); }

    // Case-insensitive substring match on name, host and ssh_user, returned in
    // storage order. An empty query returns every profile.
    [[nodiscard]] std::vector<ConnectionProfile>
    search(std::string_view query) const;

    [[nodiscard]] const ConnectionProfile* find(std::string_view id) const;

    // Insert a profile, assigning a fresh id when it has none, and return a copy
    // of what was stored (so the caller learns the generated id). The model is
    // re-ordered afterwards. Does not save.
    ConnectionProfile add(ConnectionProfile profile);

    // Replace the profile with the same id. Returns false when no profile has
    // that id. Re-orders (favorite/last-connected may have changed).
    bool update(const ConnectionProfile& profile);

    // Copy an existing profile under a fresh id and a "(copy)" name, cleared of
    // the original's favorite flag, last-connected time and last error -- a
    // duplicate is a new, never-yet-connected connection. Returns the new
    // profile, or nullopt when the id is unknown.
    std::optional<ConnectionProfile> duplicate(std::string_view id);

    bool remove(std::string_view id);

    // Record a successful connection: stamp last_connected_at and clear
    // last_error. Called by the frontend (and, in Part 2, by the managed
    // tunnel) right before/after a session begins. Re-orders.
    bool mark_connected(std::string_view id, std::int64_t when = now_seconds());

    // Record why the last connection attempt failed, for the library card.
    bool set_last_error(std::string_view id, std::string message);

    // A collision-resistant profile id ("p-" + 16 hex digits).
    [[nodiscard]] static std::string new_id();

    // Current Unix time in seconds; the default stamp for mark_connected.
    [[nodiscard]] static std::int64_t now_seconds();

private:
    void reorder();
    [[nodiscard]] ConnectionProfile* find_mutable(std::string_view id);

    ProfileStore store_;
    std::vector<ConnectionProfile> profiles_;
};

} // namespace haiku_remote
