#include "haiku_remote/library_controller.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <random>

namespace haiku_remote {

CoordinatorConfig coordinator_config_for(const ConnectionProfile& profile,
                                         std::string cookie)
{
    CoordinatorConfig config;
    config.profile = profile;
    config.cookie = std::move(cookie);
    // Reconnect follows the profile's own preference; a front end that wants a
    // different policy sets it on the config afterwards.
    config.reconnect.enabled = profile.auto_reconnect;
    return config;
}

std::string make_profile_id()
{
    static std::mt19937_64 engine(std::random_device{}());
    std::uniform_int_distribution<std::uint64_t> dist;
    const std::uint64_t value = dist(engine);
    static constexpr std::array<char, 16> hex = {'0', '1', '2', '3', '4', '5',
                                                 '6', '7', '8', '9', 'a', 'b',
                                                 'c', 'd', 'e', 'f'};
    std::string id = "profile-";
    for (int shift = 60; shift >= 0; shift -= 4)
        id.push_back(hex[(value >> shift) & 0xf]);
    return id;
}

namespace {

std::int64_t now_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

LibraryController::LibraryController(ProfileStore store, CoordinatorFactory factory)
    : store_(std::move(store)), factory_(std::move(factory))
{
}

LibraryController::~LibraryController() = default;

LoadResult LibraryController::reload()
{
    LoadResult loaded = store_.load();
    state_.profiles = loaded.profiles; // load() already orders favorite-first
    state_.selected.reset();
    state_.mode = AppMode::Library;
    return loaded;
}

void LibraryController::select(std::optional<std::size_t> index)
{
    if (index && *index >= state_.profiles.size())
        return;
    state_.selected = index;
}

const ConnectionProfile& LibraryController::open_new_editor()
{
    if (state_.mode != AppMode::Library && state_.mode != AppMode::Editor)
        return editing_;
    editing_ = ConnectionProfile{}; // schema defaults; empty id marks a create
    state_.mode = AppMode::Editor;
    return editing_;
}

const ConnectionProfile& LibraryController::open_editor(std::size_t index)
{
    if (state_.mode != AppMode::Library && state_.mode != AppMode::Editor)
        return editing_;
    if (index >= state_.profiles.size())
        return editing_;
    editing_ = state_.profiles[index];
    state_.selected = index;
    state_.mode = AppMode::Editor;
    return editing_;
}

void LibraryController::close_editor()
{
    if (state_.mode == AppMode::Editor)
        state_.mode = AppMode::Library;
}

SaveResult LibraryController::persist_and_reload(std::vector<ConnectionProfile> next,
                                                 const std::string& select_id)
{
    const SaveResult result = store_.save(next);
    if (!result.ok)
        return result; // a validation failure leaves memory and disk untouched
    const LoadResult loaded = store_.load();
    state_.profiles = loaded.profiles;
    state_.selected.reset();
    if (!select_id.empty()) {
        for (std::size_t i = 0; i < state_.profiles.size(); ++i) {
            if (state_.profiles[i].id == select_id) {
                state_.selected = i;
                break;
            }
        }
    }
    return result;
}

SaveResult LibraryController::commit_profile(ConnectionProfile profile)
{
    std::vector<ConnectionProfile> next = state_.profiles;
    if (profile.id.empty())
        profile.id = make_profile_id();
    const std::string id = profile.id;

    bool replaced = false;
    for (auto& existing : next) {
        if (existing.id == id) {
            existing = profile;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        next.push_back(profile);

    const SaveResult result = persist_and_reload(std::move(next), id);
    if (result.ok)
        state_.mode = AppMode::Library;
    return result;
}

SaveResult LibraryController::duplicate(std::size_t index)
{
    if (index >= state_.profiles.size())
        return SaveResult{};
    ConnectionProfile copy = state_.profiles[index];
    copy.id = make_profile_id();
    copy.name = copy.name.empty() ? "copy" : copy.name + " (copy)";
    copy.favorite = false;        // a duplicate is not pinned by default
    copy.last_connected_at = 0;   // and has its own, fresh history
    copy.last_error.clear();
    std::vector<ConnectionProfile> next = state_.profiles;
    next.push_back(copy);
    return persist_and_reload(std::move(next), copy.id);
}

SaveResult LibraryController::toggle_favorite(std::size_t index)
{
    if (index >= state_.profiles.size())
        return SaveResult{};
    std::vector<ConnectionProfile> next = state_.profiles;
    next[index].favorite = !next[index].favorite;
    const std::string id = next[index].id;
    return persist_and_reload(std::move(next), id);
}

SaveResult LibraryController::remove(std::size_t index)
{
    if (index >= state_.profiles.size())
        return SaveResult{};
    std::vector<ConnectionProfile> next = state_.profiles;
    next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
    return persist_and_reload(std::move(next), {});
}

void LibraryController::begin_connection(CoordinatorConfig config,
                                         std::optional<std::string> profile_id)
{
    if (coordinator_)
        return; // a connection is already live; ignore a second request
    connecting_id_ = std::move(profile_id);
    saw_connected_ = false;
    outcome_recorded_ = false;
    coordinator_ = factory_
        ? factory_(std::move(config))
        : std::make_unique<ConnectionCoordinator>(std::move(config));
    state_.coordinator = coordinator_.get();
    state_.mode = AppMode::Connecting;
    if (coordinator_)
        coordinator_->connect();
}

void LibraryController::connect(std::size_t index, std::string cookie)
{
    if (index >= state_.profiles.size())
        return;
    const ConnectionProfile& profile = state_.profiles[index];
    state_.selected = index;
    begin_connection(coordinator_config_for(profile, std::move(cookie)), profile.id);
}

void LibraryController::connect_config(CoordinatorConfig config)
{
    begin_connection(std::move(config), std::nullopt);
}

void LibraryController::cancel()
{
    if (coordinator_)
        coordinator_->disconnect();
}

void LibraryController::dismiss_failure()
{
    if (state_.mode != AppMode::Failed)
        return;
    coordinator_.reset();
    state_.coordinator = nullptr;
    connecting_id_.reset();
    reload(); // picks up the recorded last_error in the new ordering
}

void LibraryController::record_outcome(const Snapshot& snapshot)
{
    if (outcome_recorded_)
        return;
    outcome_recorded_ = true;
    if (!connecting_id_)
        return; // an ephemeral connection has nothing to write back

    auto it = std::find_if(state_.profiles.begin(), state_.profiles.end(),
                           [&](const ConnectionProfile& p) {
                               return p.id == *connecting_id_;
                           });
    if (it == state_.profiles.end())
        return;

    if (saw_connected_) {
        it->last_connected_at = now_seconds();
        it->last_error.clear();
    }
    if (snapshot.state == CoordinatorState::failed) {
        std::string reason(reason_code(snapshot.reason));
        if (!snapshot.message.empty())
            reason += ": " + snapshot.message;
        it->last_error = reason;
    }
    // Best-effort: a failed annotation write just loses the card detail, it must
    // not derail the lifecycle.
    (void)store_.save(state_.profiles);
}

void LibraryController::poll()
{
    if (!coordinator_)
        return;
    const Snapshot snapshot = coordinator_->snapshot();
    if (snapshot.state == CoordinatorState::connected)
        saw_connected_ = true;
    state_.sync_from(snapshot); // drives mode via app_mode_for()
    if (!snapshot.finished)
        return;

    record_outcome(snapshot);
    if (state_.mode == AppMode::Failed)
        return; // hold on the failure screen; dismiss_failure() tears down

    // A clean finish (user disconnect / orderly close) returns to the library.
    coordinator_.reset();
    state_.coordinator = nullptr;
    connecting_id_.reset();
    reload();
}

Snapshot LibraryController::snapshot() const
{
    if (coordinator_)
        return coordinator_->snapshot();
    return Snapshot{};
}

const ConnectionProfile* LibraryController::connecting_profile() const
{
    if (!connecting_id_)
        return nullptr;
    for (const auto& profile : state_.profiles)
        if (profile.id == *connecting_id_)
            return &profile;
    return nullptr;
}

} // namespace haiku_remote
