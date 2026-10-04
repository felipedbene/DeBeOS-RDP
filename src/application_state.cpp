#include "haiku_remote/application_state.hpp"

namespace haiku_remote {

std::string_view app_mode_name(AppMode mode)
{
    switch (mode) {
    case AppMode::Library:    return "library";
    case AppMode::Editor:     return "editor";
    case AppMode::Connecting: return "connecting";
    case AppMode::Connected:  return "connected";
    case AppMode::Failed:     return "failed";
    }
    return "library";
}

AppMode app_mode_for(CoordinatorState state, bool finished)
{
    switch (state) {
    case CoordinatorState::connected:
        return AppMode::Connected;
    case CoordinatorState::failed:
        return AppMode::Failed;
    case CoordinatorState::disconnecting:
        // A finished disconnect returns to the library; a disconnect still in
        // flight is still a connecting-style transient.
        return finished ? AppMode::Library : AppMode::Connecting;
    case CoordinatorState::idle:
    case CoordinatorState::starting_tunnel:
    case CoordinatorState::waiting_for_forward:
    case CoordinatorState::connecting_transport:
    case CoordinatorState::waiting_for_protocol_handshake:
    case CoordinatorState::requesting_display:
    case CoordinatorState::reconnecting:
        return AppMode::Connecting;
    }
    return AppMode::Connecting;
}

void ApplicationState::sync_from(const Snapshot& snapshot)
{
    if (coordinator == nullptr)
        return;
    mode = app_mode_for(snapshot.state, snapshot.finished);
}

const ConnectionProfile* ApplicationState::selected_profile() const
{
    if (!selected || *selected >= profiles.size())
        return nullptr;
    return &profiles[*selected];
}

} // namespace haiku_remote
