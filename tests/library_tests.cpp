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
#include "haiku_remote/png_writer.hpp"
#include "haiku_remote/profile_store.hpp"
#include "haiku_remote/ui_theme.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
    {
        // A newer client's top-level member survives this library's load+save.
        std::ofstream out(dir.path / "connections.json", std::ios::trunc);
        out << R"({"version":1,"syncToken":"abc","profiles":[)"
               R"({"id":"p-x","name":"X","host":"x.example","future":7}]})";
    }
    {
        ConnectionLibrary library = library_in(dir);
        library.load();
        library.mark_connected("p-x", 1234);
        check(static_cast<bool>(library.save()), "the library re-saves");
        std::ifstream in(dir.path / "connections.json");
        std::stringstream text;
        text << in.rdbuf();
        check(contains(text.str(), "\"syncToken\": \"abc\""),
              "an unknown top-level member survives ConnectionLibrary load/save");
        check(contains(text.str(), "\"future\": 7"),
              "an unknown profile member survives ConnectionLibrary load/save");
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

    // Delete asks first (issue #1 section 1, "Delete, with confirmation").
    screen.render();
    check(click(screen, LibraryScreen::Region::Kind::remove, a.id),
          "the Delete button is clickable");
    check(library.find(a.id) != nullptr,
          "Delete alone does not remove the profile; it asks to confirm");
    check(screen.pending_delete() == a.id, "the card is awaiting confirmation");
    check(screen.focus().kind == LibraryScreen::Region::Kind::cancel_delete,
          "the safe choice (Keep) takes focus");
    screen.render();
    check(!region_center(screen, LibraryScreen::Region::Kind::remove, a.id),
          "the confirming card replaces Delete with the confirmation buttons");

    // Keep backs out, and nothing was written.
    check(click(screen, LibraryScreen::Region::Kind::cancel_delete, a.id),
          "Keep is clickable");
    check(library.find(a.id) != nullptr, "Keep leaves the profile in place");
    check(screen.pending_delete().empty(), "Keep clears the confirmation");

    // Escape backs out too.
    screen.render();
    click(screen, LibraryScreen::Region::Kind::remove, a.id);
    screen.key(LibraryScreen::Key::escape);
    check(screen.pending_delete().empty() && library.find(a.id) != nullptr,
          "Escape cancels a pending delete");

    // Delete, then confirm: gone, and persisted.
    screen.render();
    click(screen, LibraryScreen::Region::Kind::remove, a.id);
    screen.render();
    check(click(screen, LibraryScreen::Region::Kind::confirm_delete, a.id),
          "the confirming Delete is clickable");
    check(library.find(a.id) == nullptr, "a confirmed Delete removes the profile");
    ConnectionLibrary reloaded = library_in(dir);
    reloaded.load();
    check(reloaded.empty(), "the delete was saved to disk");
}

// ---------------------------------------------------------------------------
// Issue #1 sections 1-2, Persistence, Visual direction
// ---------------------------------------------------------------------------

const LibraryScreen::Region* find_region(const LibraryScreen& screen,
                                         LibraryScreen::Region::Kind kind,
                                         std::string_view profile_id = {},
                                         int field = -1)
{
    for (const auto& region : screen.regions()) {
        if (region.kind != kind)
            continue;
        if (!profile_id.empty() && region.profile_id != profile_id)
            continue;
        if (field >= 0 && region.field != field)
            continue;
        return &region;
    }
    return nullptr;
}

// Focus an editor field by clicking it, clear it, and type `text`.
void set_editor_field(LibraryScreen& screen, int field, std::string_view text)
{
    screen.render();
    const auto* region =
        find_region(screen, LibraryScreen::Region::Kind::editor_field, {}, field);
    if (region == nullptr) {
        check(false, "editor field region exists");
        return;
    }
    screen.pointer_press(region->x + 4, region->y + 4);
    for (int i = 0; i < 64; ++i)
        screen.key(LibraryScreen::Key::backspace);
    if (!text.empty())
        screen.text_input(text);
}

bool same_color(Color a, Color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

std::string color_text(Color c)
{
    return "{" + std::to_string(c.r) + "," + std::to_string(c.g) + ","
        + std::to_string(c.b) + "," + std::to_string(c.a) + "}";
}

void check_pixel(const Surface& surface, int x, int y, Color expected,
                 const std::string& what)
{
    const Color got = surface.pixel(x, y);
    check(same_color(got, expected),
          what + " (pixel " + std::to_string(x) + "," + std::to_string(y) + " is "
              + color_text(got) + ", want " + color_text(expected) + ")");
}

void test_quick_connect()
{
    using Kind = LibraryScreen::Region::Kind;
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    LibraryScreen screen(library, 1024, 700);
    screen.render();

    // No host: a field error in the status line, and no request.
    check(click(screen, Kind::quick_connect), "quick Connect is clickable");
    check(!screen.take_connect_request().has_value(),
          "quick connect with no host makes no request");
    check(screen.status() == "Quick connect: host is required in SSH tunnel mode.",
          "quick connect names the missing host");
    check(screen.status_is_error(), "the quick-connect failure is shown as an error");

    // user@host, through the field; the mode chip cycles ssh -> wss.
    screen.render();
    check(click(screen, Kind::quick_host), "the quick-connect field is clickable");
    screen.text_input("ops@quick.example");
    check(screen.quick_host_text() == "ops@quick.example",
          "typing goes to the quick-connect field once it has focus");
    check(screen.search_text().empty(), "...and not to the search box");
    screen.render();
    check(click(screen, Kind::quick_mode), "the quick mode chip is clickable");
    screen.render();
    check(click(screen, Kind::quick_connect), "quick Connect is clickable again");
    const auto request = screen.take_connect_request();
    check(request.has_value(), "quick connect makes a connect request");
    if (request) {
        check(request->id.empty(), "a quick-connect request has no id (never saved)");
        check(request->host == "quick.example", "the host is split from user@");
        check(request->ssh_user == "ops", "the user is split from user@host");
        check(request->mode == ConnectionMode::wss,
              "one click on the mode chip moves ssh -> wss");
        check(request->name == "quick.example", "the name defaults to the host");
        check(request->remote_port == 10900 && request->width == 1280
                  && request->height == 800,
              "the other fields take the profile defaults");
    }
    check(library.empty(), "quick connect adds nothing to the library");
    check(!std::filesystem::exists(dir.path / "connections.json"),
          "quick connect writes nothing to disk");

    // Enter in the field connects too.
    screen.render();
    click(screen, Kind::quick_host);
    screen.key(LibraryScreen::Key::enter);
    check(screen.take_connect_request().has_value(),
          "Enter in the quick-connect field connects");

    // More options: the editor in quick mode; Connect there does not save.
    screen.render();
    check(click(screen, Kind::quick_options), "More options is clickable");
    check(screen.view() == LibraryScreen::View::editor && screen.editor_is_quick(),
          "More options opens the editor in quick-connect mode");
    check(screen.editor_text(LibraryScreen::field_host) == "quick.example"
              && screen.editor_text(LibraryScreen::field_ssh_user) == "ops",
          "the quick editor starts from the quick-connect text");
    set_editor_field(screen, LibraryScreen::field_width, "1920");
    screen.render();
    check(find_region(screen, Kind::save) == nullptr,
          "the quick editor offers Connect, not Save");
    check(click(screen, Kind::connect_now), "the quick editor's Connect is clickable");
    const auto from_editor = screen.take_connect_request();
    check(from_editor && from_editor->id.empty() && from_editor->width == 1920,
          "the quick editor connects with its edited values and no id");
    check(library.empty() && !std::filesystem::exists(dir.path / "connections.json"),
          "the quick editor's Connect saves nothing");

    // ...whereas Save & connect saves, then connects with the stored id.
    screen.render();
    click(screen, Kind::quick_options);
    screen.render();
    check(click(screen, Kind::save_and_connect), "Save & connect is clickable");
    const auto saved_request = screen.take_connect_request();
    check(library.size() == 1, "Save & connect adds the profile");
    check(saved_request && !saved_request->id.empty()
              && library.find(saved_request->id) != nullptr,
          "Save & connect connects the stored profile, by its new id");
}

void test_card_text()
{
    // describe_last_connected: exact strings at each boundary.
    const std::int64_t t = 1'700'000'000;
    check(LibraryScreen::describe_last_connected(0, t) == "Never connected",
          "0 reads as never connected");
    check(LibraryScreen::describe_last_connected(t, t + 59) == "Last connected just now",
          "under a minute is 'just now'");
    check(LibraryScreen::describe_last_connected(t, t + 60) == "Last connected 1 min ago",
          "60 s is 1 min");
    check(LibraryScreen::describe_last_connected(t, t + 3599)
              == "Last connected 59 min ago",
          "3599 s is 59 min");
    check(LibraryScreen::describe_last_connected(t, t + 3 * 3600 + 5)
              == "Last connected 3 h ago",
          "hours");
    check(LibraryScreen::describe_last_connected(t, t + 86400)
              == "Last connected 1 day ago",
          "one day, singular");
    check(LibraryScreen::describe_last_connected(t, t + 6 * 86400)
              == "Last connected 6 days ago",
          "six days, plural");

    // A week or more: the local calendar date. Pin the zone so the expected
    // date is a fact, not a property of the machine running the test.
#if defined(_WIN32)
    _putenv_s("TZ", "UTC");
    _tzset();
#else
    setenv("TZ", "UTC", 1);
    tzset();
#endif
    const std::int64_t march_first_noon_utc = 1772366400; // 2026-03-01 12:00:00Z
    check(LibraryScreen::describe_last_connected(march_first_noon_utc,
                                                 march_first_noon_utc + 30 * 86400)
              == "Last connected on 2026-03-01",
          "older than a week shows the date");

    // The card's meta line: mode, resolution, real last-success time.
    ConnectionProfile profile = make_profile("P", "p.example");
    profile.width = 1920;
    profile.height = 1080;
    profile.last_connected_at = t;
    check(LibraryScreen::card_meta(profile, t + 300)
              == "SSH tunnel  \xC2\xB7  1920 \xC3\x97 1080  \xC2\xB7  Last connected 5 min ago",
          "the card shows mode, resolution and the last-success time");
    profile.mode = ConnectionMode::direct;
    profile.last_connected_at = 0;
    check(LibraryScreen::card_meta(profile, t)
              == "Direct TCP  \xC2\xB7  1920 \xC3\x97 1080  \xC2\xB7  Never connected",
          "a never-connected direct profile says so");

    // The screen uses the injected clock (so the card is not stuck at "never").
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    const auto stored = library.add(make_profile("Q", "q.example"));
    library.mark_connected(stored.id, t);
    LibraryScreen screen(library, 1024, 700);
    std::int64_t calls = 0;
    screen.set_clock([&] {
        ++calls;
        return t + 120;
    });
    screen.render();
    check(calls == 1, "rendering one card reads the clock once");
}

void test_editor_validation_lists_every_error()
{
    using Kind = LibraryScreen::Region::Kind;
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    LibraryScreen screen(library, 1024, 700);
    screen.render();
    click(screen, Kind::new_connection);

    // Non-digits are kept as typed (no silent dropping) ...
    set_editor_field(screen, LibraryScreen::field_remote_port, "10a9");
    check(screen.editor_text(LibraryScreen::field_remote_port) == "10a9",
          "a numeric field keeps exactly what was typed");
    // ... and an empty number is an error, not 0.
    set_editor_field(screen, LibraryScreen::field_ssh_port, "");
    set_editor_field(screen, LibraryScreen::field_width, "0");
    set_editor_field(screen, LibraryScreen::field_local_port, "99999");
    // host left empty.

    screen.render();
    check(click(screen, Kind::save), "Save is clickable");
    check(screen.view() == LibraryScreen::View::editor,
          "an invalid profile is not saved; the editor stays open");
    check(library.empty(), "nothing was added");

    const std::vector<FieldError> expected = {
        {"host", "host is required in SSH tunnel mode"},
        {"sshPort", "SSH port is required"},
        {"remotePort", "Remote port must be a whole number"},
        {"localPort", "port must be between 1 and 65535"},
        {"width", "width must be between 1 and 16384"},
    };
    const auto& errors = screen.editor_errors();
    bool same = errors.size() == expected.size();
    for (std::size_t i = 0; same && i < errors.size(); ++i)
        same = errors[i].field == expected[i].field
            && errors[i].message == expected[i].message;
    check(same, "every validation error is listed, per field, in tab order");
    if (!same) {
        for (const auto& e : errors)
            std::cerr << "  got: " << e.field << ": " << e.message << '\n';
    }
    check(screen.status()
              == "5 problems to fix before saving: Host, SSH port, Remote port, Local port, Width.",
          "the status counts every problem");
    check(screen.focus().kind == Kind::editor_field
              && screen.focus().field == LibraryScreen::field_host,
          "focus jumps to the first bad field");

    // localPort maps to its own field: its border is drawn in the danger colour,
    // while a valid field keeps the normal border.
    const Surface& surface = screen.render();
    const auto* local =
        find_region(screen, Kind::editor_field, {}, LibraryScreen::field_local_port);
    const auto* user =
        find_region(screen, Kind::editor_field, {}, LibraryScreen::field_ssh_user);
    check(local != nullptr && user != nullptr, "the local-port and user fields exist");
    if (local != nullptr && user != nullptr) {
        check_pixel(surface, local->x, local->y + local->h / 2, ui_theme::danger,
                    "the local-port field with an error has a danger border");
        check_pixel(surface, user->x, user->y + user->h / 2, ui_theme::border_strong,
                    "a valid field keeps the normal border");
    }

    // Errors update live as each is fixed.
    set_editor_field(screen, LibraryScreen::field_host, "fixed.example");
    check(screen.editor_errors().size() == 4
              && screen.editor_errors().front().field == "sshPort",
          "fixing the host removes exactly its error");
    set_editor_field(screen, LibraryScreen::field_ssh_port, "22");
    set_editor_field(screen, LibraryScreen::field_remote_port, "10901");
    set_editor_field(screen, LibraryScreen::field_width, "1024");
    set_editor_field(screen, LibraryScreen::field_local_port, "15901");
    check(screen.editor_errors().empty(), "all errors gone once all are fixed");

    screen.render();
    click(screen, Kind::save);
    check(screen.view() == LibraryScreen::View::list, "a valid profile saves");
    check(library.size() == 1, "the profile was added");
    const ConnectionProfile saved = library.profiles().front();
    check(saved.local_port == std::optional<int>(15901),
          "the local port field is saved");
    check(saved.remote_port == 10901 && saved.width == 1024,
          "the typed numbers are saved exactly");
    check(saved.name == "fixed.example", "an empty name defaults to the host");

    // Clearing the local port means automatic.
    screen.render();
    click(screen, Kind::edit, saved.id);
    check(screen.editor_text(LibraryScreen::field_local_port) == "15901",
          "the editor shows the stored local port");
    set_editor_field(screen, LibraryScreen::field_local_port, "");
    screen.render();
    click(screen, Kind::save);
    check(library.find(saved.id)->local_port == std::nullopt,
          "an empty local port is saved as automatic (null)");
}

void test_import_export()
{
    using Kind = LibraryScreen::Region::Kind;
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    ConnectionProfile a = make_profile("Alpha", "a.example");
    a.last_connected_at = 1700000000;
    a.last_error = "boom";
    a.identity_file = (dir.path / "id_test").string();
    a.extra_fields = {{"future", "42"}};
    {
        std::ofstream key(dir.path / "id_test");
        key << "-----BEGIN OPENSSH PRIVATE KEY----- SECRETBYTES";
    }
    const auto stored_a = library.add(a);
    library.add(make_profile("Beta", "b.example"));
    check(static_cast<bool>(library.save()), "the source library saves");

    // Export through the UI.
    LibraryScreen screen(library, 1024, 700);
    screen.render();
    check(click(screen, Kind::export_profiles), "Export is clickable");
    check(screen.view() == LibraryScreen::View::transfer, "Export opens the path prompt");
    for (int i = 0; i < 80; ++i)
        screen.key(LibraryScreen::Key::backspace);
    const std::filesystem::path out = dir.path / "export" / "connections-export.json";
    screen.text_input(out.string());
    screen.key(LibraryScreen::Key::enter);
    check(screen.view() == LibraryScreen::View::list, "a successful export returns");
    check(screen.status()
              == "Exported 2 connections to " + out.string()
                     + " (no secrets, no history).",
          "the export reports what it wrote");

    std::ifstream in(out);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    check(!contains(text, "SECRETBYTES") && !contains(text, "PRIVATE KEY"),
          "the export never contains key material");
    check(contains(text, a.identity_file), "the export keeps the identity-file path");
    check(!contains(text, "boom"), "the export drops the last error");
    check(!contains(text, "1700000000"), "the export drops the last-connected time");
    check(!contains(text, "future"), "the export drops unknown fields");

    // Import into a fresh library through the UI.
    TempDir dir2;
    ConnectionLibrary other = library_in(dir2);
    LibraryScreen screen2(other, 1024, 700);
    screen2.render();
    check(click(screen2, Kind::import_profiles), "Import is clickable");
    for (int i = 0; i < 80; ++i)
        screen2.key(LibraryScreen::Key::backspace);
    screen2.text_input(out.string());
    screen2.render();
    check(click(screen2, Kind::transfer_confirm), "the Import button is clickable");
    check(other.size() == 2, "both profiles were imported");
    check(screen2.status() == "Imported 2 connections.", "the import reports its count");
    ConnectionLibrary reloaded = library_in(dir2);
    reloaded.load();
    check(reloaded.size() == 2, "the import was saved");
    const ConnectionProfile* imported = other.find(stored_a.id);
    check(imported != nullptr && imported->identity_file == a.identity_file
              && imported->last_connected_at == 0 && imported->extra_fields.empty(),
          "an imported profile keeps its path, not its history or unknown fields");

    // Importing again never replaces: colliding ids are re-issued.
    const ImportResult again = ProfileStore::import_file(out);
    check(other.import_profiles(again.profiles) == 2, "the second import adds two");
    check(other.size() == 4, "a repeated import adds copies rather than replacing");
    check(other.find(stored_a.id)->name == "Alpha",
          "the original keeps its id after a colliding import");
    std::vector<std::string> ids;
    for (const auto& p : other.profiles())
        ids.push_back(p.id);
    std::sort(ids.begin(), ids.end());
    check(std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
          "every id is unique after a colliding import");

    // A missing file is an error in the prompt, not a crash or an empty import.
    screen2.render();
    click(screen2, Kind::import_profiles);
    for (int i = 0; i < 80; ++i)
        screen2.key(LibraryScreen::Key::backspace);
    screen2.text_input((dir2.path / "nope.json").string());
    screen2.key(LibraryScreen::Key::enter);
    check(screen2.view() == LibraryScreen::View::transfer,
          "a failed import stays on the prompt");
}

// Tab through the whole view once and return what received focus.
std::vector<LibraryScreen::Focus> tab_cycle(LibraryScreen& screen, std::size_t steps)
{
    std::vector<LibraryScreen::Focus> seen;
    for (std::size_t i = 0; i < steps; ++i) {
        screen.key(LibraryScreen::Key::tab);
        seen.push_back(screen.focus());
    }
    return seen;
}

// Every focusable region on screen must be reached by Tab.
void check_all_regions_reachable(LibraryScreen& screen, std::string_view view)
{
    screen.render();
    std::vector<LibraryScreen::Focus> wanted;
    for (const auto& region : screen.regions()) {
        if (region.kind == LibraryScreen::Region::Kind::card)
            continue;
        wanted.push_back({region.kind, region.profile_id, region.field});
    }
    const auto seen = tab_cycle(screen, wanted.size() + 1);
    std::size_t missing = 0;
    for (const auto& want : wanted) {
        if (std::find(seen.begin(), seen.end(), want) == seen.end())
            ++missing;
    }
    check(!wanted.empty() && missing == 0,
          std::string("every control in the ") + std::string(view)
              + " view is reachable with Tab (" + std::to_string(missing)
              + " missing)");
}

void test_keyboard_reachability()
{
    using Kind = LibraryScreen::Region::Kind;
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    const auto p = library.add(make_profile("Only", "only.example"));
    LibraryScreen screen(library, 1024, 700);
    screen.render();
    check(screen.focus().kind == Kind::search, "the list opens with the search focused");
    check(!screen.focus_visible(), "no focus ring before the keyboard is used");

    // The exact list order from the search box.
    const auto seen = tab_cycle(screen, 13);
    const std::vector<Kind> expected = {
        Kind::favorite_card, Kind::connect,         Kind::edit,
        Kind::duplicate,     Kind::remove,          Kind::import_profiles,
        Kind::export_profiles, Kind::new_connection, Kind::quick_host,
        Kind::quick_mode,    Kind::quick_connect,   Kind::quick_options,
        Kind::search,
    };
    bool order_ok = seen.size() == expected.size();
    for (std::size_t i = 0; order_ok && i < seen.size(); ++i)
        order_ok = seen[i].kind == expected[i];
    check(order_ok, "Tab visits cards, header actions and quick connect in order");
    check(screen.focus_visible(), "Tab turns on the focus ring");

    // Enter and Space activate the focused control.
    for (int i = 0; i < 4; ++i)
        screen.key(LibraryScreen::Key::tab); // star, connect, edit, duplicate
    check(screen.focus() == LibraryScreen::Focus {Kind::duplicate, p.id, -1},
          "four Tabs from the search box reach the card's Duplicate");
    screen.key(LibraryScreen::Key::enter);
    check(library.size() == 2, "Enter on Duplicate duplicates");
    screen.render();
    // Back to the original card's star and toggle it with Space.
    screen.key(LibraryScreen::Key::back_tab);
    screen.key(LibraryScreen::Key::back_tab);
    screen.key(LibraryScreen::Key::back_tab);
    const LibraryScreen::Focus star = screen.focus();
    check(star.kind == Kind::favorite_card, "Shift-Tab walks back to a star");
    const bool before = library.find(star.profile_id)->favorite;
    screen.text_input(" ");
    check(library.find(star.profile_id)->favorite == !before,
          "Space on the star toggles the favorite");
    check(screen.search_text().empty(), "Space on a button does not type into search");

    check_all_regions_reachable(screen, "list");

    // Editor: every field, toggle and button.
    screen.render();
    click(screen, Kind::new_connection);
    check_all_regions_reachable(screen, "editor");
    // Space toggles the favorite checkbox from the keyboard.
    screen.render();
    while (screen.focus().kind != Kind::favorite_toggle)
        screen.key(LibraryScreen::Key::tab);
    screen.text_input(" ");
    set_editor_field(screen, LibraryScreen::field_host, "kb.example");
    screen.render();
    while (screen.focus().kind != Kind::save)
        screen.key(LibraryScreen::Key::tab);
    screen.key(LibraryScreen::Key::enter);
    bool saved_favorite = false;
    for (const auto& profile : library.profiles())
        if (profile.host == "kb.example")
            saved_favorite = profile.favorite;
    check(saved_favorite, "a keyboard-only edit can toggle Favorite and Save");

    // Transfer prompt.
    screen.render();
    click(screen, Kind::export_profiles);
    check_all_regions_reachable(screen, "export");
}

void test_theme_pixels_and_focus_ring()
{
    using Kind = LibraryScreen::Region::Kind;
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    library.add(make_profile("First", "first.example"));
    library.add(make_profile("Second", "second.example"));
    LibraryScreen screen(library, 1024, 700);
    const Surface& surface = screen.render();

    // The palette, pinned to exact values (issue #1 Visual direction: warm
    // neutral background, cobalt/Haiku-blue accent).
    check_pixel(surface, 2, 2, Color {246, 243, 238, 255},
                "the background is the warm neutral");
    const auto* new_button = find_region(screen, Kind::new_connection);
    check(new_button != nullptr, "the New connection button exists");
    if (new_button == nullptr)
        return;
    const int mid = new_button->y + new_button->h / 2;
    check_pixel(surface, new_button->x + 3, mid, Color {37, 89, 189, 255},
                "the primary button is cobalt");

    // Cards: the selected (first) card is cobalt-tinted, the second is plain.
    std::vector<const LibraryScreen::Region*> cards;
    for (const auto& region : screen.regions())
        if (region.kind == Kind::card)
            cards.push_back(&region);
    check(cards.size() == 2, "two cards are drawn");
    if (cards.size() == 2) {
        check_pixel(surface, cards[0]->x + cards[0]->w / 2, cards[0]->y + 6,
                    Color {232, 239, 252, 255}, "the selected card is tinted");
        check_pixel(surface, cards[1]->x + cards[1]->w / 2, cards[1]->y + 6,
                    Color {255, 253, 250, 255}, "an unselected card is plain");
        check_pixel(surface, cards[0]->x + cards[0]->w / 2, cards[0]->y,
                    Color {37, 89, 189, 255}, "the selected card has a cobalt edge");
    }

    // No ring before keyboard focus; a 2px cobalt ring with a 1px gap after.
    check_pixel(surface, new_button->x - 3, mid, ui_theme::background,
                "no focus ring while the pointer is in charge");
    for (int i = 0; i < 5; ++i)
        screen.key(LibraryScreen::Key::back_tab); // search -> ... -> New connection
    check(screen.focus().kind == Kind::new_connection, "Shift-Tab reached New connection");
    const Surface& ringed = screen.render();
    check_pixel(ringed, new_button->x - 3, mid, Color {37, 89, 189, 255},
                "focus ring, outer pixel");
    check_pixel(ringed, new_button->x - 2, mid, Color {37, 89, 189, 255},
                "focus ring, inner pixel");
    check_pixel(ringed, new_button->x - 1, mid, ui_theme::background,
                "a 1px gap separates the ring from the button");
    check_pixel(ringed, new_button->x - 4, mid, ui_theme::background,
                "the ring is exactly 2px wide");
    check_pixel(ringed, new_button->x + new_button->w + 2, mid, Color {37, 89, 189, 255},
                "the ring surrounds the right edge too");

    // Delete confirmation is drawn in the danger colour.
    screen.render();
    click(screen, Kind::remove, cards.empty() ? "" : cards[0]->profile_id);
    screen.pointer_move(-100, -100); // no hover tint
    const Surface& confirming = screen.render();
    const auto* confirm = find_region(screen, Kind::confirm_delete);
    check(confirm != nullptr, "the confirm button is drawn");
    if (confirm != nullptr)
        check_pixel(confirming, confirm->x + 3, confirm->y + confirm->h / 2,
                    Color {178, 50, 40, 255}, "the confirming Delete is the danger red");

    std::string png_error;
    const auto shots = std::filesystem::temp_directory_path() / "debeos-rdp-library";
    std::filesystem::create_directories(shots);
    // Screenshots for review: a library with a favourite, a recent success and
    // a failure, at the size issue #1 requires.
    {
        ConnectionProfile fav = make_profile("Graviton test instance",
                                             "ec2-203-0-113-7.compute.example");
        fav.favorite = true;
        const auto stored = library.add(fav);
        library.mark_connected(stored.id, 1700000000);
        ConnectionProfile broken = make_profile("Lab box", "lab.example");
        broken.mode = ConnectionMode::direct;
        broken.width = 1920;
        broken.height = 1080;
        const auto b = library.add(broken);
        library.set_last_error(b.id, "TCP connection refused by lab.example:10900");
    }
    LibraryScreen shot(library, 1024, 700);
    shot.set_clock([] { return std::int64_t {1700000000 + 2 * 3600}; });
    shot.render();
    (void)write_png(shot.render(), (shots / "library-1024x700.png").string(), png_error);
    click(shot, Kind::new_connection);
    shot.render();
    click(shot, Kind::save); // provoke the field errors
    (void)write_png(shot.render(), (shots / "editor-1024x700.png").string(), png_error);
    shot.key(LibraryScreen::Key::escape);
    shot.render();
    const auto* first_card = find_region(shot, Kind::card);
    if (first_card != nullptr) {
        const std::string id = first_card->profile_id;
        click(shot, Kind::remove, id);
        shot.pointer_move(-100, -100);
        shot.key(LibraryScreen::Key::down); // keyboard: ring on the next card's Delete
        shot.key(LibraryScreen::Key::up);
        click(shot, Kind::remove, id);
        shot.key(LibraryScreen::Key::tab);
        shot.key(LibraryScreen::Key::back_tab);
        (void)write_png(shot.render(), (shots / "library-confirm-delete.png").string(),
                        png_error);
    }
    std::cout << "library screenshots: " << shots.string() << '\n';
}

void test_layout_at_1024x700_and_high_dpi()
{
    using Kind = LibraryScreen::Region::Kind;
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    for (int i = 0; i < 6; ++i)
        library.add(make_profile("P" + std::to_string(i), "h" + std::to_string(i)));

    LibraryScreen one(library, 1024, 700);
    one.render();
    std::size_t cards = 0;
    bool inside = true;
    for (const auto& r : one.regions()) {
        if (r.kind == Kind::card)
            ++cards;
        inside = inside && r.x >= 0 && r.y >= 0 && r.x + r.w <= 1024 && r.y + r.h <= 700;
    }
    check(cards == 4, "1024x700 shows four cards (" + std::to_string(cards) + ")");
    check(inside, "every list control fits inside 1024x700");

    click(one, Kind::new_connection);
    one.render();
    bool editor_inside = true;
    int lowest_field = 0;
    for (const auto& r : one.regions()) {
        editor_inside = editor_inside && r.x >= 0 && r.y >= 0 && r.x + r.w <= 1024
            && r.y + r.h <= 700;
        if (r.kind == Kind::editor_field || r.kind == Kind::mode_cycle)
            lowest_field = std::max(lowest_field, r.y + r.h);
    }
    const auto* save = find_region(one, Kind::save);
    check(editor_inside, "every editor control fits inside 1024x700");
    check(save != nullptr && save->y > lowest_field,
          "the editor's buttons sit below its fields at 1024x700");

    // High-DPI: the same window at 2x renders every control at exactly twice
    // the size and position, and the ring at twice the width.
    LibraryScreen two(library, 2048, 1400);
    two.set_scale(2.0f);
    one.key(LibraryScreen::Key::escape);
    one.render();
    two.render();
    const auto* a = find_region(one, Kind::new_connection);
    const auto* b = find_region(two, Kind::new_connection);
    check(a != nullptr && b != nullptr && b->x == 2 * a->x && b->y == 2 * a->y
              && b->w == 2 * a->w && b->h == 2 * a->h,
          "at scale 2 the New connection button is exactly twice the size");
    const auto* qa = find_region(one, Kind::quick_host);
    const auto* qb = find_region(two, Kind::quick_host);
    check(qa != nullptr && qb != nullptr && qb->x == 2 * qa->x && qb->w == 2 * qa->w,
          "at scale 2 the quick-connect field is exactly twice the size");
    std::size_t cards2 = 0;
    for (const auto& r : two.regions())
        cards2 += r.kind == Kind::card ? 1 : 0;
    check(cards2 == 4, "the 2x window shows the same four cards");
    if (b != nullptr) {
        for (int i = 0; i < 5; ++i)
            two.key(LibraryScreen::Key::back_tab);
        const Surface& s = two.render();
        const int mid = b->y + b->h / 2;
        check_pixel(s, b->x - 6, mid, ui_theme::focus_ring, "2x ring, outer pixel");
        check_pixel(s, b->x - 3, mid, ui_theme::focus_ring, "2x ring, inner pixel");
        check_pixel(s, b->x - 2, mid, ui_theme::background, "2x ring gap");
        check_pixel(s, b->x - 7, mid, ui_theme::background, "2x ring is 4px wide");
    }

    // Resizing re-lays out: a taller window shows more cards.
    one.resize(1024, 900);
    one.render();
    std::size_t tall = 0;
    for (const auto& r : one.regions())
        tall += r.kind == Kind::card ? 1 : 0;
    check(tall == 6, "a 1024x900 window shows all six cards (" + std::to_string(tall) + ")");
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
    test_quick_connect();
    test_card_text();
    test_editor_validation_lists_every_error();
    test_import_export();
    test_keyboard_reachability();
    test_theme_pixels_and_focus_ring();
    test_layout_at_1024x700_and_high_dpi();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " library checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " library checks FAILED\n";
    return 1;
}
