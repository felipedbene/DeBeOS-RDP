#pragma once

// The application shell's connection-library driver: a UI-free state machine
// that turns user intent (browse the library, edit a profile, connect, cancel)
// into ProfileStore writes and a live ConnectionCoordinator. It lives in
// haiku_remote_core, which has NO SDL on its link line, so the SDL front end and
// any future one drive exactly the same flow and the whole thing is unit-tested
// without a window (see tests/gui_flow_tests.cpp).
//
// The front end is thin: each frame it calls poll(), reads mode() to pick a
// view, renders that view, and translates widget actions into the controller's
// methods. The controller owns the in-memory library, the editor's working
// copy, and -- across a connect -- the ConnectionCoordinator; it never owns a
// window or any SDL object.
//
// Lifecycle/reaper posture is inherited from the coordinator and the SshTunnel
// it owns: a verified-ours orphan is reaped before every connect, and a front
// end also reaps at launch. Cancel/disconnect/exit stop the ssh child (the
// coordinator's destructor joins the worker, which stops the tunnel); an
// unverifiable pid is never signalled. See ssh_tunnel.hpp.

#include "haiku_remote/application_state.hpp"
#include "haiku_remote/connection_coordinator.hpp"
#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/profile_store.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace haiku_remote {

// Build a CoordinatorConfig for a saved/ephemeral profile. cookie is the
// direct-mode session cookie (ignored in ssh mode, where the cookie is fetched
// over the same ssh identity). Reconnect follows the profile's own preference.
[[nodiscard]] CoordinatorConfig coordinator_config_for(const ConnectionProfile& profile,
                                                       std::string cookie = {});

// A short, collision-resistant id for a freshly created profile, e.g.
// "profile-3f9a1c7b". Exposed for tests; commit_profile() assigns one when a
// created profile arrives without an id.
[[nodiscard]] std::string make_profile_id();

class LibraryController {
public:
    // Build an unconnected ConnectionCoordinator for a config. Defaults to a
    // real ConnectionCoordinator; a test injects one driven by fakes.
    using CoordinatorFactory =
        std::function<std::unique_ptr<ConnectionCoordinator>(CoordinatorConfig)>;

    explicit LibraryController(ProfileStore store = ProfileStore{},
                               CoordinatorFactory factory = {});
    ~LibraryController();

    LibraryController(const LibraryController&) = delete;
    LibraryController& operator=(const LibraryController&) = delete;

    // Read the library from the store into memory, favorite-first. Returns the
    // store's LoadResult so a UI can surface a recovered or corrupt file. Resets
    // the view to the library.
    LoadResult reload();

    [[nodiscard]] AppMode mode() const { return state_.mode; }
    [[nodiscard]] const std::vector<ConnectionProfile>& profiles() const
    {
        return state_.profiles;
    }
    [[nodiscard]] std::optional<std::size_t> selected() const { return state_.selected; }
    [[nodiscard]] const ConnectionProfile* selected_profile() const
    {
        return state_.selected_profile();
    }
    void select(std::optional<std::size_t> index);

    // Open the profile editor. The no-argument form seeds a new default profile;
    // the index form seeds a copy of an existing one. Returns the working copy
    // the UI should bind its form to. A no-op (returns the current working copy)
    // unless the view is Library or Editor.
    const ConnectionProfile& open_new_editor();
    const ConnectionProfile& open_editor(std::size_t index);
    [[nodiscard]] const ConnectionProfile& editing() const { return editing_; }
    void close_editor(); // back to the library without saving

    // Persist the editor's working copy. An empty id is a create (a fresh id is
    // assigned); otherwise it replaces the profile carrying that id. Validates
    // and writes through the store atomically; on failure returns the SaveResult
    // naming the offending field and leaves both memory and disk unchanged. On
    // success it reloads/reorders, selects the saved profile, and returns to the
    // library.
    SaveResult commit_profile(ConnectionProfile profile);

    // Library mutations, each validated-and-persisted then reloaded/reordered.
    // Index is into the current favorite-first profiles() ordering.
    SaveResult duplicate(std::size_t index);
    SaveResult toggle_favorite(std::size_t index);
    SaveResult remove(std::size_t index); // the UI owns the confirm step

    // Start connecting a saved profile (by index) or the editor's current
    // selection. Builds the coordinator through the factory and moves to the
    // Connecting view. cookie is the direct-mode session cookie.
    void connect(std::size_t index, std::string cookie = {});

    // Connect an ephemeral, never-saved config -- the launch_options CLI bypass
    // (an explicit --host/--ssh-host/--url). No profile is touched; a later
    // outcome is not written back to the library.
    void connect_config(CoordinatorConfig config);

    // Ask the live connection to tear down. Non-blocking; poll() observes the
    // worker finishing and returns to the library. Safe when not connected.
    void cancel();

    // Dismiss the Failed view and return to the library, tearing the coordinator
    // down. A no-op unless the view is Failed.
    void dismiss_failure();

    // Advance the state machine one step: refresh the view from the live
    // coordinator's snapshot, and on first completion write the outcome back to
    // the connecting profile (last-connected time on success, the actionable
    // reason on failure). A clean finish returns to the library; a failure holds
    // on the Failed view (its snapshot/diag stay available) until dismissed.
    void poll();

    // The live coordinator's snapshot, or a default (idle) Snapshot when there
    // is no live connection.
    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] ConnectionCoordinator* coordinator() { return coordinator_.get(); }

    // The profile currently being connected, or nullptr for an ephemeral
    // connection or when idle. Its last_error/last_connected_at are what poll()
    // writes on completion.
    [[nodiscard]] const ConnectionProfile* connecting_profile() const;

    [[nodiscard]] const ProfileStore& store() const { return store_; }

private:
    SaveResult persist_and_reload(std::vector<ConnectionProfile> next,
                                  const std::string& select_id);
    void begin_connection(CoordinatorConfig config,
                          std::optional<std::string> profile_id);
    void record_outcome(const Snapshot& snapshot);

    ProfileStore store_;
    CoordinatorFactory factory_;
    ApplicationState state_;
    std::unique_ptr<ConnectionCoordinator> coordinator_;
    ConnectionProfile editing_;                 // the editor's working copy
    std::optional<std::string> connecting_id_;  // profile being connected
    bool saw_connected_ = false;                // reached connected at least once
    bool outcome_recorded_ = false;             // guards the one write-back
};

} // namespace haiku_remote
