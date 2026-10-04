#pragma once

// The application shell's state, with no UI and no SDL. It lives in
// haiku_remote_core so the SDL and any future front end share one notion of
// "what screen am I on" and one mapping from the coordinator's connection
// lifecycle onto it. The front end owns the ConnectionCoordinator and the SDL
// window; this type only tracks which view is current, the loaded library, the
// selected profile, and a non-owning pointer to the live coordinator.

#include "haiku_remote/connection_coordinator.hpp"
#include "haiku_remote/connection_profile.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace haiku_remote {

// The top-level screen the application is showing. Library is the home screen
// issue #1 asks for instead of a black desktop; Editor is the profile editor;
// Connecting shows lifecycle progress; Connected is the desktop view; Failed is
// the actionable error screen.
enum class AppMode {
    Library,
    Editor,
    Connecting,
    Connected,
    Failed,
};

[[nodiscard]] std::string_view app_mode_name(AppMode mode);

// Map a coordinator snapshot onto the application screen. PURE and testable:
// the whole state machine lands on exactly one screen for any snapshot.
//   connected                          -> Connected
//   failed                             -> Failed
//   disconnecting + finished           -> Library (the session returned home)
//   anything else (idle..reconnecting) -> Connecting
[[nodiscard]] AppMode app_mode_for(CoordinatorState state, bool finished);

struct ApplicationState {
    AppMode mode = AppMode::Library;
    // The connection library, favorite-first (see ProfileStore::order).
    std::vector<ConnectionProfile> profiles;
    // Index into `profiles` of the selected/connecting profile, if any.
    std::optional<std::size_t> selected;
    // The live connection, owned by the front end -- not by this struct.
    ConnectionCoordinator* coordinator = nullptr;

    // Drive `mode` from the coordinator's current snapshot. A no-op (stays in
    // Library) when there is no coordinator.
    void sync_from(const Snapshot& snapshot);

    // The selected profile, or nullptr when none is selected or the index is
    // stale.
    [[nodiscard]] const ConnectionProfile* selected_profile() const;
};

} // namespace haiku_remote
