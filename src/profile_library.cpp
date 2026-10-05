#include "haiku_remote/profile_library.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <random>

namespace haiku_remote {

namespace {

std::string to_lower(std::string_view text)
{
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return lowered;
}

bool contains_ci(std::string_view haystack, const std::string& needle_lower)
{
    if (needle_lower.empty())
        return true;
    return to_lower(haystack).find(needle_lower) != std::string::npos;
}

} // namespace

std::int64_t ConnectionLibrary::now_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string ConnectionLibrary::new_id()
{
    // 64 bits of randomness is plenty to keep two concurrently-created profiles
    // from colliding; the id is never a security token, only a stable handle.
    static std::mt19937_64 engine(std::random_device {}());
    const std::uint64_t bits = engine();
    static constexpr std::array<char, 16> digits = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string id = "p-";
    for (int shift = 60; shift >= 0; shift -= 4)
        id.push_back(digits[(bits >> shift) & 0xf]);
    return id;
}

LoadResult ConnectionLibrary::load()
{
    LoadResult result = store_.load();
    profiles_ = result.profiles; // already favorite-first from the store
    return result;
}

SaveResult ConnectionLibrary::save() const
{
    return store_.save(profiles_);
}

void ConnectionLibrary::reorder()
{
    profiles_ = ProfileStore::order(std::move(profiles_));
}

const ConnectionProfile* ConnectionLibrary::find(std::string_view id) const
{
    for (const auto& profile : profiles_) {
        if (profile.id == id)
            return &profile;
    }
    return nullptr;
}

ConnectionProfile* ConnectionLibrary::find_mutable(std::string_view id)
{
    for (auto& profile : profiles_) {
        if (profile.id == id)
            return &profile;
    }
    return nullptr;
}

std::vector<ConnectionProfile>
ConnectionLibrary::search(std::string_view query) const
{
    const std::string needle = to_lower(query);
    std::vector<ConnectionProfile> matches;
    for (const auto& profile : profiles_) {
        if (contains_ci(profile.name, needle) || contains_ci(profile.host, needle)
            || contains_ci(profile.ssh_user, needle)) {
            matches.push_back(profile);
        }
    }
    return matches;
}

ConnectionProfile ConnectionLibrary::add(ConnectionProfile profile)
{
    if (profile.id.empty())
        profile.id = new_id();
    profiles_.push_back(profile);
    reorder();
    return profile;
}

bool ConnectionLibrary::update(const ConnectionProfile& profile)
{
    ConnectionProfile* existing = find_mutable(profile.id);
    if (existing == nullptr)
        return false;
    *existing = profile;
    reorder();
    return true;
}

std::optional<ConnectionProfile>
ConnectionLibrary::duplicate(std::string_view id)
{
    const ConnectionProfile* source = find(id);
    if (source == nullptr)
        return std::nullopt;
    ConnectionProfile copy = *source;
    copy.id = new_id();
    copy.name = source->name + " (copy)";
    copy.favorite = false;
    copy.last_connected_at = 0;
    copy.last_error.clear();
    profiles_.push_back(copy);
    reorder();
    return copy;
}

bool ConnectionLibrary::remove(std::string_view id)
{
    const auto before = profiles_.size();
    profiles_.erase(
        std::remove_if(profiles_.begin(), profiles_.end(),
                       [&](const ConnectionProfile& p) { return p.id == id; }),
        profiles_.end());
    return profiles_.size() != before;
}

bool ConnectionLibrary::mark_connected(std::string_view id, std::int64_t when)
{
    ConnectionProfile* profile = find_mutable(id);
    if (profile == nullptr)
        return false;
    profile->last_connected_at = when;
    profile->last_error.clear();
    reorder();
    return true;
}

bool ConnectionLibrary::set_last_error(std::string_view id, std::string message)
{
    ConnectionProfile* profile = find_mutable(id);
    if (profile == nullptr)
        return false;
    profile->last_error = std::move(message);
    return true;
}

} // namespace haiku_remote
