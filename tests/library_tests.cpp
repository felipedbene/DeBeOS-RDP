// Tests for the connection library layer added in DeBeOS-RDP issue #1 Part 1:
// the ConnectionLibrary CRUD/order/search model, the plan_launch() transport
// seam, and the LibraryScreen's event-to-action logic (driven through its hit
// regions, so no pixel coordinates are hard-coded).
//
// House rules (CLAUDE.md): assert exact values, keep expectations independent
// of the code that produces them, and give ordering an asymmetric subject.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/library_screen.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/profile_library.hpp"
#include "haiku_remote/profile_store.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace haiku_remote;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool contains(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

struct TempDir {
    std::filesystem::path path;
    TempDir()
    {
        static int counter = 0;
        const auto now =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("haiku-remote-lib-" + std::to_string(now) + "-"
               + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

ConnectionProfile make_profile(std::string name, std::string host)
{
    ConnectionProfile profile;
    profile.name = std::move(name);
    profile.host = std::move(host);
    profile.mode = ConnectionMode::ssh;
    profile.remote_port = 10900;
    profile.ssh_port = 22;
    profile.width = 1280;
    profile.height = 800;
    return profile;
}

ConnectionLibrary library_in(const TempDir& dir)
{
    return ConnectionLibrary(ProfileStore(dir.path / "connections.json"));
}

// Center of the first region matching a kind (and, when given, a profile id).
std::optional<std::pair<int, int>>
region_center(const LibraryScreen& screen, LibraryScreen::Region::Kind kind,
              std::string_view profile_id = {})
{
    for (const auto& region : screen.regions()) {
        if (region.kind != kind)
            continue;
        if (!profile_id.empty() && region.profile_id != profile_id)
            continue;
        return std::make_pair(region.x + region.w / 2, region.y + region.h / 2);
    }
    return std::nullopt;
}

bool click(LibraryScreen& screen, LibraryScreen::Region::Kind kind,
           std::string_view profile_id = {})
{
    const auto center = region_center(screen, kind, profile_id);
    if (!center)
        return false;
    screen.pointer_press(center->first, center->second);
    return true;
}

void test_library_crud()
{
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    check(library.empty(), "a fresh library is empty");

    const ConnectionProfile added = library.add(make_profile("Alpha", "a.example"));
    check(!added.id.empty(), "add() assigns an id when none was given");
    check(library.size() == 1, "add() grows the library");
    check(library.find(added.id) != nullptr, "the added profile is findable");

    // Duplicate: new id, "(copy)" name, cleared favorite/history.
    ConnectionProfile fav = make_profile("Beta", "b.example");
    fav.favorite = true;
    fav.last_connected_at = 1700000000;
    fav.last_error = "boom";
    const ConnectionProfile stored_fav = library.add(fav);
    const auto copy = library.duplicate(stored_fav.id);
    check(copy.has_value(), "duplicate() of a known id succeeds");
    check(copy->id != stored_fav.id, "the duplicate gets a fresh id");
    check(copy->name == "Beta (copy)", "the duplicate's name gains ' (copy)'");
    check(!copy->favorite, "the duplicate is not a favorite");
    check(copy->last_connected_at == 0, "the duplicate has no last-connected time");
    check(copy->last_error.empty(), "the duplicate has no last error");
    check(library.duplicate("nope") == std::nullopt,
          "duplicate() of an unknown id yields nullopt");

    // Update replaces by id.
    ConnectionProfile edited = added;
    edited.name = "Alpha renamed";
    check(library.update(edited), "update() of a known id succeeds");
    check(library.find(added.id)->name == "Alpha renamed",
          "update() replaced the stored profile");
    edited.id = "missing";
    check(!library.update(edited), "update() of an unknown id fails");

    // Remove.
    const std::size_t before = library.size();
    check(library.remove(stored_fav.id), "remove() of a known id succeeds");
    check(library.size() == before - 1, "remove() shrinks the library");
    check(!library.remove("missing"), "remove() of an unknown id fails");
}

void test_order_and_search()
{
    TempDir dir;
    ConnectionLibrary library = library_in(dir);

    ConnectionProfile old_fav = make_profile("Graviton", "ec2.example");
    old_fav.favorite = true;
    old_fav.last_connected_at = 100;
    ConnectionProfile recent = make_profile("Laptop", "lap.local");
    recent.favorite = false;
    recent.last_connected_at = 500;
    ConnectionProfile newest_fav = make_profile("Workstation", "ws.corp");
    newest_fav.favorite = true;
    newest_fav.last_connected_at = 300;

    library.add(old_fav);
    library.add(recent);
    library.add(newest_fav);

    // Favorites first, then most-recent within each group: the asymmetric
    // subject is the non-favorite 'recent', which has the newest timestamp of
    // all yet must still sort below both favorites.
    const auto& profiles = library.profiles();
    check(profiles.size() == 3, "three profiles stored");
    check(profiles[0].name == "Workstation",
          "the newer favorite sorts first");
    check(profiles[1].name == "Graviton", "the older favorite sorts second");
    check(profiles[2].name == "Laptop",
          "the non-favorite sorts last despite the newest timestamp");

    // Case-insensitive search over name/host/user.
    check(library.search("ws.corp").size() == 1, "search matches host");
    check(library.search("WORKSTATION").size() == 1,
          "search is case-insensitive on name");
    check(library.search("").size() == 3, "an empty query returns everything");
    check(library.search("zzz").empty(), "a non-matching query returns nothing");
}

void test_mark_connected_reorders()
{
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    const auto a = library.add(make_profile("A", "a.example"));
    const auto b = library.add(make_profile("B", "b.example"));
    check(library.set_last_error(a.id, "down"), "set_last_error finds the id");

    check(library.mark_connected(b.id, 999), "mark_connected finds the id");
    check(library.profiles()[0].id == b.id,
          "a freshly connected profile sorts to the front");
    check(library.find(b.id)->last_connected_at == 999,
          "mark_connected stamps the time");

    // Connecting clears a stale error.
    check(library.mark_connected(a.id, 1000), "mark_connected the errored one");
    check(library.find(a.id)->last_error.empty(),
          "a successful connect clears the last error");
}

void test_persist_round_trip()
{
    TempDir dir;
    {
        ConnectionLibrary library = library_in(dir);
        ConnectionProfile wss = make_profile("Broker", "broker.example");
        wss.mode = ConnectionMode::wss;
        library.add(wss);
        library.add(make_profile("Direct", "direct.example"));
        const SaveResult saved = library.save();
        check(static_cast<bool>(saved), "save() succeeds");
    }
    {
        ConnectionLibrary reloaded = library_in(dir);
        const LoadResult result = reloaded.load();
        check(result.status == LoadStatus::loaded, "the saved file reloads");
        check(reloaded.size() == 2, "both profiles survive a round trip");
        bool found_wss = false;
        for (const auto& p : reloaded.profiles())
            if (p.name == "Broker")
                found_wss = p.mode == ConnectionMode::wss;
        check(found_wss, "the wss mode round-trips through JSON");
    }
}

void test_new_id_unique()
{
    const std::string a = ConnectionLibrary::new_id();
    const std::string b = ConnectionLibrary::new_id();
    check(a != b, "two generated ids differ");
    check(a.rfind("p-", 0) == 0, "a generated id has the 'p-' prefix");
    check(a.size() == 18, "a generated id is 'p-' plus 16 hex digits");
}

void test_plan_launch()
{
    // Part 2: every mode is fully wired, so no plan is a stub. The planner is
    // pure -- it maps a mode onto an ordered list of candidate routes without
    // spawning ssh or opening a socket -- so these checks run on fake hosts.

    // direct == auto: broker first, SSH tunnel fallback.
    ConnectionProfile direct = make_profile("D", "d.example");
    direct.mode = ConnectionMode::direct;
    direct.remote_port = 10901;
    const LaunchPlan direct_plan = plan_launch(direct);
    check(!direct_plan.is_stub, "no mode is a stub in Part 2");
    check(direct_plan.steps.size() == 2,
          "direct/auto plans two steps: broker then tunnel");
    check(direct_plan.steps[0].kind == RouteKind::broker,
          "direct tries the broker first");
    check(direct_plan.steps[1].kind == RouteKind::tunnel,
          "direct falls back to the tunnel");
    check(contains(direct_plan.steps[0].transport.url, "wss://d.example:10902"),
          "the broker step targets the broker port, not the app_server port");
    check(direct_plan.steps[1].transport.host == "127.0.0.1",
          "the tunnel step connects to the local forward end");
    check(direct_plan.transport.url == direct_plan.steps[0].transport.url,
          "plan.transport aliases the first step for back-compat");

    // ssh forces the tunnel: one step, no broker.
    ConnectionProfile ssh = make_profile("S", "s.example");
    ssh.mode = ConnectionMode::ssh;
    const LaunchPlan ssh_plan = plan_launch(ssh);
    check(!ssh_plan.is_stub, "ssh mode is wired, not a stub");
    check(ssh_plan.steps.size() == 1, "ssh forces a single tunnel route");
    check(ssh_plan.steps[0].kind == RouteKind::tunnel, "ssh is the tunnel route");
    check(ssh_plan.transport.host == "127.0.0.1",
          "the ssh route connects to the local forward end, not the host");
    check(ssh_plan.transport.url.empty(), "the ssh route uses raw TCP, not a url");

    // wss forces the broker: one step, no tunnel fallback.
    ConnectionProfile wss = make_profile("W", "w.example");
    wss.mode = ConnectionMode::wss;
    const LaunchPlan wss_plan = plan_launch(wss);
    check(!wss_plan.is_stub, "wss mode is wired, not a stub");
    check(wss_plan.steps.size() == 1, "wss forces a single broker route");
    check(wss_plan.steps[0].kind == RouteKind::broker, "wss is the broker route");
    check(contains(wss_plan.transport.url, "wss://w.example:10902"),
          "the wss route builds a broker wss:// url");
    check(wss_plan.transport.token.empty(),
          "no token is fabricated by the pure planner");

    // The planner reads a cookie only when cookie_source is a plain file;
    // otherwise it leaves it for open_connection() to fetch over SSH.
    ConnectionProfile tagged = make_profile("T", "t.example");
    tagged.mode = ConnectionMode::ssh;
    tagged.cookie_source = "broker"; // a tag, not a path
    check(plan_launch(tagged).steps[0].transport.cookie.empty(),
          "a cookie source tag is not mistaken for a file");
}

void test_screen_new_connection_flow()
{
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    LibraryScreen screen(library, 1280, 800);
    screen.render();
    check(screen.view() == LibraryScreen::View::list, "opens on the list view");

    check(click(screen, LibraryScreen::Region::Kind::new_connection),
          "the New connection button exists and is clickable");
    screen.render();
    check(screen.view() == LibraryScreen::View::editor,
          "New connection opens the editor");

    // Focus the host field and type a host (name/port/size are pre-filled).
    check(click(screen, LibraryScreen::Region::Kind::editor_field,
                /*profile_id*/ {}),
          "an editor field is clickable");
    // Explicitly focus the host field by its field index.
    for (const auto& region : screen.regions()) {
        if (region.kind == LibraryScreen::Region::Kind::editor_field
            && region.field == LibraryScreen::field_host) {
            screen.pointer_press(region.x + 5, region.y + 5);
            break;
        }
    }
    screen.render();
    screen.text_input("graviton.example");

    const std::size_t before = library.size();
    check(click(screen, LibraryScreen::Region::Kind::save), "Save is clickable");
    screen.render();
    check(screen.view() == LibraryScreen::View::list,
          "a valid Save returns to the list");
    check(library.size() == before + 1, "Save added a profile to the library");

    bool found = false;
    for (const auto& p : library.profiles())
        if (p.host == "graviton.example")
            found = true;
    check(found, "the typed host was persisted into the model");
}

void test_screen_connect_request()
{
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    const auto target = library.add(make_profile("Target", "t.example"));
    LibraryScreen screen(library, 1280, 800);
    screen.render();

    check(!screen.take_connect_request().has_value(),
          "no connect request before any click");
    check(click(screen, LibraryScreen::Region::Kind::connect, target.id),
          "a card's Connect button is clickable");
    const auto request = screen.take_connect_request();
    check(request.has_value(), "Connect records a connect request");
    check(request && request->id == target.id,
          "the connect request names the clicked profile");
    check(!screen.take_connect_request().has_value(),
          "the connect request is one-shot");
}

void test_screen_delete_and_favorite()
{
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    const auto a = library.add(make_profile("A", "a.example"));
    LibraryScreen screen(library, 1280, 800);
    screen.render();

    check(click(screen, LibraryScreen::Region::Kind::favorite_card, a.id),
          "the favorite star is clickable");
    check(library.find(a.id)->favorite,
          "clicking the star marks the profile a favorite");

    screen.render();
    check(click(screen, LibraryScreen::Region::Kind::remove, a.id),
          "the Delete button is clickable");
    check(library.find(a.id) == nullptr, "Delete removed the profile");
    // The delete persisted.
    ConnectionLibrary reloaded = library_in(dir);
    reloaded.load();
    check(reloaded.empty(), "the delete was saved to disk");
}

} // namespace

int main()
{
    test_library_crud();
    test_order_and_search();
    test_mark_connected_reorders();
    test_persist_round_trip();
    test_new_id_unique();
    test_plan_launch();
    test_screen_new_connection_flow();
    test_screen_connect_request();
    test_screen_delete_and_favorite();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " library checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " library checks FAILED\n";
    return 1;
}
