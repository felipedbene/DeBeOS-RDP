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
#include "haiku_remote/text_engine.hpp"
#include "haiku_remote/ui_theme.hpp"

#include "test_fonts.hpp"
#include "haiku_remote/quic.hpp" // HAIKU_REMOTE_HAVE_QUIC

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
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

    // auto: broker first, SSH tunnel fallback -- in a build with wss.
    ConnectionProfile autop = make_profile("A", "a.example");
    autop.mode = ConnectionMode::automatic;
    autop.remote_port = 10901;
    const LaunchPlan auto_plan
        = plan_launch(autop, /*wss_available=*/true, /*quic_available=*/false);
    check(!auto_plan.is_stub, "no mode is a stub in Part 2");
    check(auto_plan.steps.size() == 2, "auto plans two steps: broker then tunnel");
    check(auto_plan.steps[0].kind == RouteKind::broker, "auto tries the broker first");
    check(auto_plan.steps[1].kind == RouteKind::tunnel, "auto falls back to the tunnel");
    check(auto_plan.steps[0].transport.url == "wss://a.example:10902",
          "the broker step targets the broker port, not the app_server port");
    check(auto_plan.steps[1].transport.host == "127.0.0.1"
              && auto_plan.steps[1].transport.port == 0,
          "the tunnel step connects to the local forward end (port chosen later)");
    check(auto_plan.transport.url == auto_plan.steps[0].transport.url,
          "plan.transport aliases the first step for back-compat");

    // ...and in a build without wss, the broker step is not planned at all.
    const LaunchPlan auto_no_wss = plan_launch(autop, /*wss_available=*/false);
    check(auto_no_wss.steps.size() == 1
              && auto_no_wss.steps[0].kind == RouteKind::tunnel
              && auto_no_wss.steps[0].transport.host == "127.0.0.1",
          "auto without wss plans the tunnel alone");
    check(auto_no_wss.note
              == "auto mode: ssh tunnel to baron@a.example:22 -> 127.0.0.1:10901"
                 " (no broker: this build has no wss)",
          "auto without wss says why there is no broker step");
    check(plan_launch(autop).steps.size()
              == (build_supports_wss ? (build_supports_quic ? 3u : 2u) : 1u),
          "plan_launch() defaults to what this build can do");

    // ...and with QUIC (#82), the broker over QUIC comes first, with the
    // 3 s give-up that lets a blocked UDP path fall back to wss.
    const LaunchPlan auto_quic
        = plan_launch(autop, /*wss_available=*/true, /*quic_available=*/true);
    check(auto_quic.steps.size() == 3 && auto_quic.steps[0].kind == RouteKind::broker
              && auto_quic.steps[0].transport.url == "quic://a.example:10902"
              && auto_quic.steps[0].transport.quic_handshake_timeout_ms == 3000
              && auto_quic.steps[1].transport.url == "wss://a.example:10902"
              && auto_quic.steps[1].transport.quic_handshake_timeout_ms == 0
              && auto_quic.steps[2].kind == RouteKind::tunnel,
          "auto with QUIC plans quic, then wss, then the tunnel");
    check(auto_quic.note
              == "auto mode: broker (quic) at quic://a.example:10902,"
                 " falling back to broker (wss) at wss://a.example:10902,"
                 " falling back to ssh tunnel to baron@a.example:22 ->"
                 " 127.0.0.1:10901",
          "the auto note names all three routes in order");
    check(plan_launch(autop, /*wss_available=*/false, /*quic_available=*/true)
                  .steps.size()
              == 1,
          "QUIC needs TLS: no wss means no QUIC step either");

    // direct: issue #1's plain TCP connection to host:remotePort, no SSH.
    TempDir cookie_dir;
    const auto cookie_file = cookie_dir.path / "session_cookie.10901";
    std::ofstream(cookie_file) << "direct-cookie-value\n";
    ConnectionProfile direct = make_profile("D", "d.example");
    direct.mode = ConnectionMode::direct;
    direct.remote_port = 10901;
    direct.cookie_source = cookie_file.string();
    for (const bool wss : {true, false}) {
        const LaunchPlan direct_plan = plan_launch(direct, wss);
        check(direct_plan.steps.size() == 1
                  && direct_plan.steps[0].kind == RouteKind::direct,
              "direct plans exactly one step, the direct route");
        const TransportOptions& t = direct_plan.steps[0].transport;
        check(t.host == "d.example" && t.port == 10901 && t.url.empty(),
              "the direct step is TCP to host:remotePort, not a url or a forward");
        check(t.cookie == "direct-cookie-value",
              "the direct step's cookie is cookieSource's file, newline trimmed");
        check(direct_plan.note == "direct mode: direct tcp to d.example:10901 (no ssh)",
              "the direct plan's note");
    }
    check(route_kind_name(RouteKind::direct) == "direct", "route_kind_name(direct)");
    ConnectionProfile no_cookie = direct;
    no_cookie.cookie_source.clear();
    check(plan_launch(no_cookie).steps[0].transport.cookie.empty(),
          "a direct profile without a cookie source plans no cookie (the"
          " transport then refuses locally)");

    // quic forces QUIC: one broker step with the transport's own timeout.
    ConnectionProfile quic = make_profile("Q", "q.example");
    quic.mode = ConnectionMode::quic;
    const LaunchPlan quic_plan = plan_launch(quic);
    check(quic_plan.steps.size() == 1 && quic_plan.steps[0].kind == RouteKind::broker
              && quic_plan.transport.url == "quic://q.example:10902",
          "quic forces a single broker route over QUIC");
    check(quic_plan.steps[0].note == "broker (quic) at quic://q.example:10902",
          "the quic route says what it is");
    check(mode_from_name("quic") == ConnectionMode::quic
              && mode_name(ConnectionMode::quic) == "quic",
          "the quic mode round-trips by name");

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

// The editor's mode chip: each click moves to the next mode this build can
// use, in connection_profile.hpp's order, and Save stores the mode shown. The
// expected cycles are written out here, not derived from next_mode().
void test_screen_mode_chip_cycles_the_modes()
{
    const std::vector<ConnectionMode> cycle = build_supports_quic
        ? std::vector<ConnectionMode> {ConnectionMode::ssh, ConnectionMode::automatic,
                                       ConnectionMode::direct, ConnectionMode::wss,
                                       ConnectionMode::quic, ConnectionMode::ssh}
        : build_supports_wss
        ? std::vector<ConnectionMode> {ConnectionMode::ssh, ConnectionMode::automatic,
                                       ConnectionMode::direct, ConnectionMode::wss,
                                       ConnectionMode::ssh}
        : std::vector<ConnectionMode> {ConnectionMode::ssh, ConnectionMode::automatic,
                                       ConnectionMode::direct, ConnectionMode::ssh};
    for (std::size_t clicks = 0; clicks < cycle.size(); ++clicks) {
        TempDir dir;
        ConnectionLibrary library = library_in(dir);
        LibraryScreen screen(library, 1280, 800);
        screen.render();
        click(screen, LibraryScreen::Region::Kind::new_connection);
        screen.render();
        for (const auto& region : screen.regions()) {
            if (region.kind == LibraryScreen::Region::Kind::editor_field
                && region.field == LibraryScreen::field_host) {
                screen.pointer_press(region.x + 5, region.y + 5);
                break;
            }
        }
        screen.render();
        screen.text_input("modes.example");
        bool clicked = true;
        for (std::size_t i = 0; i < clicks; ++i) {
            clicked = clicked && click(screen, LibraryScreen::Region::Kind::mode_cycle);
            screen.render();
        }
        click(screen, LibraryScreen::Region::Kind::save);
        screen.render();
        const bool saved = clicked && library.size() == 1
            && library.profiles().front().mode == cycle[clicks];
        check(saved, "the mode chip after " + std::to_string(clicks)
                         + " click(s) saves mode "
                         + std::string(mode_name(cycle[clicks])));
    }
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
    // A click puts the caret where it lands (here: before the text), so
    // clear the field with Select All + Backspace.
    screen.pointer_press(region->x + 4, region->y + 4);
    screen.key(LibraryScreen::Key::select_all);
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

    // user@host, through the field; the mode chip cycles ssh -> auto.
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
        // connection_modes (#86): ssh, auto, direct, wss.
        check(request->mode == ConnectionMode::automatic,
              "one click on the mode chip moves ssh -> auto");
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
    // As a JSON string: a Windows path's backslashes are escaped on disk
    // (C:\\Users\\...), so the raw path is not a substring there.
    std::string escaped_identity;
    for (const char c : a.identity_file) {
        if (c == '\\' || c == '"')
            escaped_identity.push_back('\\');
        escaped_identity.push_back(c);
    }
    check(contains(text, "\"" + escaped_identity + "\""),
          "the export keeps the identity-file path");
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

bool is_text_focus(const LibraryScreen& screen)
{
    using Kind = LibraryScreen::Region::Kind;
    const Kind k = screen.focus().kind;
    return k == Kind::search || k == Kind::quick_host || k == Kind::editor_field
        || k == Kind::transfer_path;
}

// ---------------------------------------------------------------------------
// Text editing: caret, selection, clipboard, IME, blink (issue #1, editor
// polish). Expected positions come from the field geometry (box x + its text
// padding) and from the font engine's own advance widths -- never from the
// screen's caret code.
// ---------------------------------------------------------------------------

// A pinned millisecond clock for the caret blink.
struct FakeMillis {
    std::shared_ptr<std::int64_t> now = std::make_shared<std::int64_t>(1000);
    std::function<std::int64_t()> clock() const
    {
        auto shared = now;
        return [shared] { return *shared; };
    }
};

int field_text_width(std::string_view text, float scale = 1.0f)
{
    static TextEngine engine;
    Font font;
    font.size = 13 * scale;
    return static_cast<int>(std::lround(engine.width(text, font)));
}

void test_text_editing_model()
{
    using Key = LibraryScreen::Key;
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    LibraryScreen screen(library, 1024, 700);
    screen.render();
    check(screen.focus().kind == LibraryScreen::Region::Kind::search,
          "the list opens with the search field focused");

    // Code-point stepping: "héllo" is h(1) é(2) l l o = 6 bytes, so four Lefts
    // from the end land on byte 1 -- byte stepping would stop at 2, inside é.
    screen.text_input("h\xC3\xA9llo");
    check(screen.search_text() == "h\xC3\xA9llo" && screen.caret() == 6,
          "typed UTF-8 is kept whole and the caret ends after it");
    for (int i = 0; i < 4; ++i)
        screen.key(Key::left);
    check(screen.caret() == 1, "Left steps by code point, over the 2-byte e-acute ("
                                   + std::to_string(screen.caret()) + ")");
    screen.text_input("X");
    check(screen.search_text() == "hX\xC3\xA9llo" && screen.caret() == 2,
          "text is inserted at the caret, not appended");
    screen.key(Key::right);
    check(screen.caret() == 4, "Right steps over e-acute in one press");
    screen.key(Key::home);
    screen.key(Key::del);
    check(screen.search_text() == "X\xC3\xA9llo" && screen.caret() == 0,
          "Delete removes the code point after the caret");
    screen.key(Key::right);
    screen.key(Key::del);
    check(screen.search_text() == "Xllo", "Delete removes all of a multi-byte code point");
    screen.key(Key::end);
    screen.key(Key::backspace);
    check(screen.search_text() == "Xll" && screen.caret() == 3,
          "End then Backspace removes the last code point");

    // Words: the separators of a host are not word characters.
    screen.key(Key::select_all);
    screen.text_input("ops@host.example");
    check(screen.search_text() == "ops@host.example",
          "typing over Select All replaces the whole value");
    screen.key(Key::left, LibraryScreen::mod_word);
    check(screen.caret() == 9, "word-Left stops at the start of 'example'");
    screen.key(Key::left, LibraryScreen::mod_word);
    check(screen.caret() == 4, "word-Left stops at the start of 'host'");
    screen.key(Key::left, LibraryScreen::mod_word);
    check(screen.caret() == 0, "word-Left stops at the start of 'ops'");
    screen.key(Key::right, LibraryScreen::mod_word);
    check(screen.caret() == 3, "word-Right stops at the end of 'ops'");
    screen.key(Key::end);
    screen.key(Key::backspace, LibraryScreen::mod_word);
    check(screen.search_text() == "ops@host.", "word-Backspace deletes 'example'");

    // Selection with Shift: the anchor stays put.
    screen.key(Key::home);
    screen.key(Key::right, LibraryScreen::mod_shift);
    screen.key(Key::right, LibraryScreen::mod_shift);
    check(screen.selection_anchor() == 0 && screen.caret() == 2
              && screen.selected_text() == "op",
          "Shift-Right twice selects 'op'");
    screen.key(Key::right, LibraryScreen::mod_shift | LibraryScreen::mod_word);
    check(screen.selected_text() == "ops", "Shift-word-Right extends to the word end");
    screen.key(Key::left);
    check(screen.caret() == 0 && screen.selected_text().empty(),
          "Left with a selection collapses it to its start");
    screen.key(Key::end, LibraryScreen::mod_shift);
    screen.key(Key::left, LibraryScreen::mod_shift);
    check(screen.selected_text() == "ops@host", "Shift-End, Shift-Left selects all but '.'");
    screen.key(Key::backspace);
    check(screen.search_text() == "." && screen.caret() == 0,
          "Backspace with a selection deletes exactly the selection");

    // Clipboard, through the frontend's hooks.
    std::string clip;
    int writes = 0;
    screen.set_clipboard([&] { return clip; },
                         [&](const std::string& value) {
                             clip = value;
                             ++writes;
                         });
    screen.key(Key::select_all);
    screen.text_input("graviton.example");
    screen.key(Key::copy);
    check(writes == 0, "Copy with nothing selected leaves the clipboard alone");
    screen.key(Key::left, LibraryScreen::mod_word | LibraryScreen::mod_shift);
    screen.key(Key::copy);
    check(clip == "example" && writes == 1 && screen.search_text() == "graviton.example",
          "Copy writes the selection and keeps the text");
    screen.key(Key::cut);
    check(clip == "example" && writes == 2 && screen.search_text() == "graviton.",
          "Cut writes the selection and removes it");
    clip = "a\nb\tc\xC3\xA9";
    screen.key(Key::paste);
    check(screen.search_text() == "graviton.abc\xC3\xA9" && screen.caret() == 14,
          "Paste inserts at the caret with newlines and tabs dropped");
    screen.key(Key::home);
    screen.key(Key::paste);
    check(screen.search_text() == "abc\xC3\xA9graviton.abc\xC3\xA9" && screen.caret() == 5,
          "Paste at Home inserts at the start");

    // Without hooks the screen's own clipboard round-trips.
    LibraryScreen local(library, 1024, 700);
    local.render();
    local.text_input("alpha");
    local.key(Key::select_all);
    local.key(Key::cut);
    check(local.search_text().empty(), "Cut empties the field (own clipboard)");
    local.key(Key::paste);
    local.key(Key::paste);
    check(local.search_text() == "alphaalpha", "the own clipboard pastes what was cut");

    // IME composition is shown, never stored, and owns the keys meanwhile.
    local.key(Key::select_all);
    local.key(Key::backspace);
    local.text_input("x");
    local.text_editing("\xE3\x81\x8B\xE3\x81\xAA", 1); // "kana" in hiragana
    check(local.search_text() == "x"
              && local.composition() == "\xE3\x81\x8B\xE3\x81\xAA",
          "a composition is held aside, not inserted");
    local.key(Key::backspace);
    local.key(Key::left);
    check(local.search_text() == "x" && local.caret() == 1,
          "keys during a composition do not edit the field");
    local.text_input("\xE4\xBB\xAE\xE5\x90\x8D"); // the committed kanji
    check(local.search_text() == "x\xE4\xBB\xAE\xE5\x90\x8D" && local.composition().empty()
              && local.caret() == 7,
          "the commit is inserted at the caret and ends the composition");

    // Escape clears the search under the caret; the caret follows.
    local.key(Key::escape);
    local.text_input("z");
    check(local.search_text() == "z" && local.caret() == 1,
          "after Escape clears the field the caret is back inside it");
}

void test_text_pointer_editing()
{
    using Kind = LibraryScreen::Region::Kind;
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    LibraryScreen screen(library, 1024, 700);
    screen.render();
    click(screen, Kind::new_connection);
    screen.render();
    set_editor_field(screen, LibraryScreen::field_name, "MMMM");
    screen.render();
    const auto* name = find_region(screen, Kind::editor_field, {}, LibraryScreen::field_name);
    if (name == nullptr) {
        check(false, "the Name field has a region");
        return;
    }
    const int origin = name->x + 10; // the box's text padding
    const int mid = name->y + name->h / 2;
    screen.pointer_press(origin + field_text_width("MM") + 1, mid);
    screen.pointer_release(origin + field_text_width("MM") + 1, mid);
    check(screen.caret() == 2 && screen.selection_anchor() == 2,
          "a click between the second and third M puts the caret at byte 2");
    screen.pointer_press(name->x + 2, mid);
    check(screen.caret() == 0, "a click in the left padding puts the caret at the start");
    screen.pointer_move(origin + field_text_width("MMM") - 1, mid);
    check(screen.caret() == 3 && screen.selection_anchor() == 0,
          "dragging selects from the press to the pointer");
    screen.pointer_move(name->x + name->w + 200, mid);
    check(screen.caret() == 4, "a drag past the box selects to the end");
    screen.pointer_release(name->x + name->w + 200, mid);
    screen.pointer_move(origin, mid);
    check(screen.caret() == 4 && screen.selected_text() == "MMMM",
          "after release the pointer no longer moves the caret");
    screen.pointer_press(origin + field_text_width("M"), mid);
    screen.pointer_release(origin + field_text_width("M"), mid);
    screen.pointer_press(origin + field_text_width("MMM"), mid, 1, LibraryScreen::mod_shift);
    check(screen.selection_anchor() == 1 && screen.caret() == 3,
          "Shift-click extends from the caret to the click");

    set_editor_field(screen, LibraryScreen::field_name, "ops@host.example");
    screen.render();
    const int in_host = origin + field_text_width("ops@ho");
    screen.pointer_press(in_host, mid, 2);
    check(screen.selected_text() == "host", "a double-click selects the word under it");
    screen.pointer_press(in_host, mid, 3);
    check(screen.selected_text() == "ops@host.example", "a triple-click selects all");
    // A click in another field moves focus there with the caret at the click.
    const auto* host = find_region(screen, Kind::editor_field, {}, LibraryScreen::field_host);
    if (host != nullptr) {
        screen.pointer_press(host->x + 2, host->y + host->h / 2);
        check(screen.focus().field == LibraryScreen::field_host && screen.caret() == 0
                  && screen.selected_text().empty(),
              "clicking another field focuses it with no stale selection");
    }
}

void test_caret_selection_and_blink_pixels()
{
    using Kind = LibraryScreen::Region::Kind;
    using Key = LibraryScreen::Key;
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    LibraryScreen screen(library, 1024, 700);
    FakeMillis millis;
    screen.set_millis_clock(millis.clock());
    screen.render();
    click(screen, Kind::new_connection);
    set_editor_field(screen, LibraryScreen::field_name, "MMMM");
    screen.key(Key::home);
    const Surface& s0 = screen.render();
    const auto* name = find_region(screen, Kind::editor_field, {}, LibraryScreen::field_name);
    if (name == nullptr) {
        check(false, "the Name field has a region");
        return;
    }
    const int origin = name->x + 10;
    // Row 8 of the box: inside the caret/selection band (inset 7), above the
    // glyphs, so every pixel there is caret, selection or field background.
    const int row = name->y + 8;
    const Color white = ui_theme::field_background;
    check_pixel(s0, origin, row, ui_theme::accent, "the caret at Home is at the text origin");
    check_pixel(s0, origin + 1, row, white, "the caret is one pixel wide at 1x");
    check_pixel(s0, origin - 1, row, white, "nothing left of the caret but padding");
    check_pixel(s0, origin, name->y + 7, ui_theme::accent, "the caret starts 7 rows down");
    check_pixel(s0, origin, name->y + 6, white, "...and not above that");
    check_pixel(s0, origin, name->y + name->h - 8, ui_theme::accent, "the caret ends 7 rows up");
    check_pixel(s0, origin, name->y + name->h - 7, white, "...and not below that");
    check(screen.caret_rect().x == origin && screen.caret_rect().w == 1
              && screen.caret_rect().y == name->y && screen.caret_rect().h == name->h,
          "caret_rect reports the caret column over the field's height");

    // Blink: on for 530 ms, off for 530 ms; the screen says when to wake.
    *millis.now += 100;
    check(screen.next_wake_ms() == 430, "the next blink is 430 ms away ("
                                            + std::to_string(screen.next_wake_ms()) + ")");
    *millis.now += 429;
    screen.tick();
    check(!screen.dirty(), "no redraw is asked for before the phase changes");
    *millis.now += 1;
    screen.tick();
    check(screen.dirty(), "the phase change at 530 ms asks for a redraw");
    check(screen.next_wake_ms() == 530, "a full phase until the next change");
    const Surface& s1 = screen.render();
    check_pixel(s1, origin, row, white, "the caret is off in the second phase");
    *millis.now += 530;
    screen.tick();
    check(screen.dirty(), "the caret comes back at 1060 ms");
    check_pixel(screen.render(), origin, row, ui_theme::accent, "the caret is on again");
    *millis.now += 530; // off phase...
    screen.key(Key::end);
    screen.key(Key::home); // ...but any edit or move restarts the blink
    check_pixel(screen.render(), origin, row, ui_theme::accent,
                "moving the caret shows it at once, whatever the phase");
    screen.set_window_active(false);
    check(screen.next_wake_ms() == -1, "an inactive window needs no wake-ups");
    check_pixel(screen.render(), origin, row, white, "an inactive window shows no caret");
    screen.set_window_active(true);

    // Selection: an accent band exactly under the selected glyphs.
    screen.key(Key::select_all);
    const Surface& s2 = screen.render();
    const int all = field_text_width("MMMM");
    check_pixel(s2, origin, row, ui_theme::accent, "the selection starts at the text origin");
    check_pixel(s2, origin - 1, row, white, "...and not before it");
    check_pixel(s2, origin + all - 1, row, ui_theme::accent,
                "the selection covers the last M");
    check_pixel(s2, origin + all, row, white,
                "the selection ends at the text's advance, and no caret is drawn beyond it");
    {
        // The selected glyphs must be exactly what the font engine paints in
        // on_accent over accent at the same pen position: a reference drawn
        // here with TextEngine alone, not through the screen.
        Surface reference(s2.width(), s2.height());
        reference.fill_rect_color(Rect {0, 0, static_cast<float>(s2.width() - 1),
                                        static_cast<float>(s2.height() - 1)},
                                  ui_theme::accent);
        TextEngine engine;
        DrawState pen;
        pen.high = ui_theme::on_accent;
        pen.font.size = 13;
        engine.draw("MMMM",
                    Point {static_cast<float>(origin),
                           static_cast<float>(name->y + name->h / 2 + 5)},
                    pen, reference);
        int mismatched = 0;
        int inked = 0;
        for (int y = name->y + 7; y < name->y + name->h - 7; ++y) {
            for (int x = origin; x < origin + all; ++x) {
                mismatched += same_color(s2.pixel(x, y), reference.pixel(x, y)) ? 0 : 1;
                inked += same_color(reference.pixel(x, y), ui_theme::accent) ? 0 : 1;
            }
        }
        check(inked > 0 && mismatched == 0,
              "selected text is exactly on_accent glyphs over the accent band ("
                  + std::to_string(mismatched) + " of the band's pixels differ, "
                  + std::to_string(inked) + " inked)");
    }
    screen.key(Key::left, LibraryScreen::mod_shift);
    const Surface& s3 = screen.render();
    const int three = field_text_width("MMM");
    check_pixel(s3, origin + three - 1, row, ui_theme::accent, "Shift-Left keeps three Ms");
    check_pixel(s3, origin + three, row, white, "...and drops the fourth");
    screen.set_window_active(false);
    check_pixel(screen.render(), origin, row, ui_theme::accent_soft,
                "an inactive window mutes the selection to accent_soft");
    screen.set_window_active(true);

    // Horizontal scroll: a value wider than the box keeps the caret inside it.
    set_editor_field(screen, LibraryScreen::field_name, std::string(90, 'M'));
    screen.render();
    const int inner_right = name->x + name->w - 10; // exclusive
    check(screen.caret_rect().x == inner_right - 1,
          "at the end of a long value the caret sits at the box's right padding ("
              + std::to_string(screen.caret_rect().x) + ")");
    check_pixel(screen.render(), inner_right - 1, row, ui_theme::accent,
                "...and is drawn there");
    // The value really scrolled (the caret is not merely clamped to the edge):
    // a click on the caret, at the box's right padding, maps to the end.
    screen.pointer_press(inner_right - 1, name->y + name->h / 2);
    screen.pointer_release(inner_right - 1, name->y + name->h / 2);
    check(screen.caret() == 90,
          "a click on the scrolled caret keeps it at the end of the value ("
              + std::to_string(screen.caret()) + ")");
    screen.key(Key::home);
    screen.render();
    check(screen.caret_rect().x == origin, "Home scrolls the value back to its start");

    // Focus on a button: nothing blinks, so nothing wakes.
    screen.key(Key::escape);
    screen.render();
    click(screen, Kind::new_connection);
    for (int i = 0; i < LibraryScreen::editor_field_count; ++i)
        screen.key(Key::tab);
    check(!is_text_focus(screen), "Tab past the fields reaches a button");
    check(screen.next_wake_ms() == -1, "with a button focused there is no blink timer");
    screen.render();
    check(screen.caret_rect().w == 0, "and no caret");
}

// ---------------------------------------------------------------------------
// Scrolling: the editor's form (wheel, Page keys, focus-follows) and the list
// ---------------------------------------------------------------------------

void test_editor_scrolls()
{
    using Kind = LibraryScreen::Region::Kind;
    using Key = LibraryScreen::Key;
    TempDir dir;
    ConnectionLibrary library = library_in(dir);

    // At 1024x700 the whole form fits: nothing scrolls (PR #80's layout holds).
    {
        LibraryScreen big(library, 1024, 700);
        big.render();
        click(big, Kind::new_connection);
        big.render();
        check(big.editor_max_scroll() == 0, "the editor does not scroll at 1024x700 ("
                                                + std::to_string(big.editor_max_scroll()) + ")");
        big.wheel(-3);
        big.render();
        check(big.editor_scroll() == 0, "the wheel cannot scroll a form that fits");
    }

    // At the minimum window, 800x480. The form runs to 620 (the route chips'
    // bottom, 586 + 26, plus 8); the band ends 10 above the action bar, which
    // sits 32 + 38 above the bottom: 480 - 70 - 10 = 400. So 220 to scroll.
    LibraryScreen screen(library, LibraryScreen::min_window_width,
                         LibraryScreen::min_window_height);
    screen.render();
    click(screen, Kind::new_connection);
    const Surface& s0 = screen.render();
    check(screen.editor_max_scroll() == 220,
          "at 800x480 the form scrolls by 220 (" + std::to_string(screen.editor_max_scroll())
              + ")");
    const int left = 32;
    // Host is row 1: 112 + 70 + 18 = 200 (focused: a 2px accent edge).
    check_pixel(s0, left, 200 + 16, ui_theme::accent, "unscrolled, Host's edge is at y 200");
    check_pixel(s0, left, 168, ui_theme::background, "unscrolled, y 168 is between fields");

    screen.wheel(-1);
    const Surface& s1 = screen.render();
    check(screen.editor_scroll() == 48, "one wheel notch scrolls 48");
    const auto* host = find_region(screen, Kind::editor_field, {}, LibraryScreen::field_host);
    check(host != nullptr && host->y == 152, "Host moved up by exactly 48");
    check_pixel(s1, left, 152 + 16, ui_theme::accent, "Host's edge is drawn at y 152");
    check_pixel(s1, left, 238, ui_theme::border_strong,
                "SSH user (row 2, 270 - 48 = 222) now covers y 238");
    // Name (row 0) is at 130 - 48 = 82..114, mostly under the header: painted
    // over there, and clickable only in its visible part, 104..114.
    check_pixel(s1, left + 5, 100, ui_theme::background,
                "a field scrolled under the header is painted over");
    check_pixel(s1, left + 5, 106, ui_theme::field_background,
                "...and shows below the header's edge");
    const auto* name = find_region(screen, Kind::editor_field, {}, LibraryScreen::field_name);
    check(name != nullptr && name->y == 104 && name->h == 10,
          "a field partly under the header is clickable only where it shows");
    check_pixel(s1, left + 300, 104, ui_theme::border,
                "a hairline marks the band's top while the form is scrolled");
    bool clipped = true;
    for (const auto& r : screen.regions()) {
        if (r.kind == Kind::editor_field || r.kind == Kind::mode_cycle
            || r.kind == Kind::favorite_toggle || r.kind == Kind::autoreconnect_toggle)
            clipped = clipped && r.y >= 104 && r.y + r.h <= 400;
    }
    check(clipped, "every form hit region is clipped to the band 104..400");
    const auto* save = find_region(screen, Kind::save);
    check(save != nullptr && save->y == 410 && save->h == 38,
          "the action bar stays put at y 410 while the form scrolls");

    screen.wheel(5);
    screen.render();
    check(screen.editor_scroll() == 0, "scrolling up stops at the top");
    screen.wheel(-100);
    screen.render();
    check(screen.editor_scroll() == 220, "scrolling down stops at the bottom");
    screen.key(Key::page_up);
    screen.render();
    check(screen.editor_scroll() == 0,
          "Page Up scrolls a band less a row: 296 - 70 = 226, clamped to 0");
    screen.key(Key::page_down);
    screen.render();
    check(screen.editor_scroll() == 220, "Page Down scrolls to the bottom here");
    screen.wheel(100);
    screen.render();

    // Tab brings the focused control into view, by just enough. Focus starts
    // on Host (row 1, fully visible). Row r's field is at 130 + 70r and must
    // show 18 above (label) and 20 below (error line).
    screen.key(Key::tab); // SSH user, row 2: bottom 270 + 52 = 322, fits
    screen.render();
    check(screen.editor_scroll() == 0, "Tab to a visible field does not scroll");
    screen.key(Key::tab); // SSH port, row 3: 340 + 52 = 392, fits
    screen.render();
    check(screen.editor_scroll() == 0, "SSH port still fits");
    screen.key(Key::tab); // Remote port, row 4: 410 + 52 = 462 -> 62
    screen.render();
    check(screen.editor_scroll() == 62,
          "Tab to Remote port scrolls exactly 62 (" + std::to_string(screen.editor_scroll())
              + ")");
    screen.key(Key::tab); // Local port, row 5: 480 + 52 = 532 -> 132
    screen.render();
    check(screen.editor_scroll() == 132, "Tab to Local port scrolls to 132");
    screen.key(Key::tab); // Identity file, row 0 right: top 112 -> 112 - 104 = 8
    screen.render();
    check(screen.editor_scroll() == 8,
          "Tab back up to Identity file scrolls up to 8 (" + std::to_string(screen.editor_scroll())
              + ")");
    screen.key(Key::back_tab);
    screen.render();
    check(screen.editor_scroll() == 132, "Shift-Tab follows focus back down");

    // Every control in the Tab order is fully visible once focused.
    bool all_visible = true;
    std::string hidden;
    for (int i = 0; i < 20; ++i) {
        screen.key(Key::tab);
        screen.render();
        const LibraryScreen::Focus& f = screen.focus();
        const auto* r = find_region(screen, f.kind, {}, f.field);
        const int full = f.kind == Kind::editor_field ? 32 : f.kind == Kind::cancel
                                                              || f.kind == Kind::save
                                                              || f.kind == Kind::save_and_connect
                                                         ? 38
                                                         : 34;
        if (r == nullptr || r->h != full || r->y < 104 || r->y + r->h > 448) {
            all_visible = false;
            hidden += " " + std::to_string(static_cast<int>(f.kind)) + "/"
                + std::to_string(f.field);
        }
    }
    check(all_visible, "every focused control is wholly on screen at 800x480:" + hidden);

    // Growing the window clamps the scroll; at 2x every offset doubles.
    screen.resize(1024, 700);
    screen.render();
    check(screen.editor_scroll() == 0 && screen.editor_max_scroll() == 0,
          "back at 1024x700 the scroll clamps to 0");
    LibraryScreen two(library, 2 * 800, 2 * 480);
    two.set_scale(2.0f);
    two.render();
    click(two, Kind::new_connection);
    two.render();
    two.wheel(-1);
    two.render();
    const auto* host2 = find_region(two, Kind::editor_field, {}, LibraryScreen::field_host);
    check(two.editor_max_scroll() == 220 && two.editor_scroll() == 48 && host2 != nullptr
              && host2->y == 2 * 152,
          "at 2x the scroll is in logical units and Host lands at exactly 2 x 152");
}

void test_list_wheel_scrolls_cards()
{
    using Kind = LibraryScreen::Region::Kind;
    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    for (int i = 0; i < 6; ++i)
        library.add(make_profile("W" + std::to_string(i), "w" + std::to_string(i)));
    const auto order = library.search("");
    LibraryScreen screen(library, 1024, 700); // four cards per page
    screen.render();
    const auto first_card = [&]() -> std::string {
        const auto* card = find_region(screen, Kind::card);
        return card == nullptr ? std::string() : card->profile_id;
    };
    check(first_card() == order[0].id, "the list starts at the first card");
    screen.wheel(-1);
    screen.render();
    check(first_card() == order[1].id, "a notch down shows from the second card");
    screen.wheel(-10);
    screen.render();
    check(first_card() == order[2].id, "the list stops with its last card at the bottom");
    screen.wheel(0.5f);
    screen.render();
    check(first_card() == order[2].id, "half a notch does not move the list yet");
    screen.wheel(0.5f);
    screen.render();
    check(first_card() == order[1].id, "the second half-notch completes one card");
    screen.key(LibraryScreen::Key::page_down);
    screen.render();
    check(first_card() == order[2].id, "Page Down scrolls the list to its end");

    // The minimum window still lays the list out: two whole cards.
    LibraryScreen small(library, LibraryScreen::min_window_width,
                        LibraryScreen::min_window_height);
    small.render();
    std::size_t cards = 0;
    bool inside = true;
    for (const auto& r : small.regions()) {
        cards += r.kind == Kind::card ? 1 : 0;
        inside = inside && r.x >= 0 && r.y >= 0 && r.x + r.w <= 800 && r.y + r.h <= 480;
    }
    check(cards == 2, "800x480 shows two cards (" + std::to_string(cards) + ")");
    check(inside, "every list control fits inside 800x480");
}

// TextEngine::stops() and fit(), the two calls that keep the text fields linear,
// against the oracle they replace: width() of each prefix, measured one by one.
void test_text_engine_stops_and_fit()
{
    TextEngine text;
    Font font;
    font.size = 13;
    // Mixed widths and a two-byte e-acute, so a byte/code-point mix-up or an
    // off-by-one stop cannot pass on a uniform string.
    const std::string s = "Wa\xC3\xA9 i/W";
    const TextEngine::Stops stops = text.stops(s, font);
    check(stops.offsets == std::vector<std::size_t> {0, 1, 2, 4, 5, 6, 7, 8},
          "stops: one offset per code point boundary, e-acute is one stop");
    check(stops.caret.size() == 8 && stops.pen.size() == 7,
          "stops: a caret per boundary and a pen per glyph");
    if (stops.caret.size() == 8 && stops.pen.size() == 7) {
        bool exact = true;
        for (std::size_t k = 0; k < 8; ++k)
            exact = exact && stops.caret[k] == text.width(s.substr(0, stops.offsets[k]), font);
        check(exact, "stops: caret[k] is exactly width() of the k-code-point prefix");
        bool pens = true;
        for (std::size_t k = 0; k < 7; ++k)
            pens = pens && stops.pen[k] == stops.caret[k];
        check(pens, "stops: with no kerning (B_BITMAP_SPACING) each pen is its caret");
    }
    // fit(): the longest prefix no wider than the limit, by brute force here.
    for (float limit : {0.0f, 5.0f, 17.5f, 30.0f, 1000.0f}) {
        std::size_t want = 0;
        for (std::size_t at : {1, 2, 4, 5, 6, 7, 8})
            if (text.width(s.substr(0, at), font) <= limit)
                want = at;
        check(text.fit(s, font, limit) == want,
              "fit() at " + std::to_string(limit) + " px is the longest prefix that fits ("
                  + std::to_string(want) + " bytes)");
    }
    // The operation count. width() of three code points walks three; fit()
    // stops at the first stop past the limit: "WW" fits in "WWWWWWWW", so it
    // walks W0, W1, W2 (all fit) and W3 (does not), four, never all eight.
    const std::uint64_t before = text.stats().code_points;
    const std::uint64_t layouts = text.stats().layouts;
    const float ww = text.width("WW", font);
    check(text.stats().code_points - before == 2 && text.stats().layouts - layouts == 1,
          "stats: width() of two code points is one layout of two");
    const std::uint64_t at_fit = text.stats().code_points;
    check(text.fit("WWWWWWWW", font, ww) == 2, "fit(): two W fit in the width of two");
    check(text.stats().code_points - at_fit == 4,
          "fit() lays out up to the first glyph past the limit, not the whole string ("
              + std::to_string(text.stats().code_points - at_fit) + " code points)");
}

// What `act` costs in code points laid out by the screen's text engine.
template <typename Act>
std::uint64_t layout_cost(LibraryScreen& screen, Act&& act)
{
    const std::uint64_t before = screen.text_stats().code_points;
    act();
    return screen.text_stats().code_points - before;
}

// A 1 MiB paste hung the library for minutes (found by PR #97 on X11): eliding
// "No connections match ..." re-measured the whole search once per code point it
// dropped, and every frame measured each field prefix it needed and drew every
// glyph. Now each field has a byte limit, refusal is visible, and what a frame
// costs does not grow with the text. Pinned as exact code-point counts, which
// do not depend on the font or the host, never as wall-clock time.
void test_text_field_scale()
{
    using Kind = LibraryScreen::Region::Kind;
    using Key = LibraryScreen::Key;

    // The limits, written out: each field's natural maximum.
    check(LibraryScreen::max_field_bytes(Kind::editor_field, LibraryScreen::field_host) == 253,
          "host holds a DNS name: 253 bytes");
    for (int field : {LibraryScreen::field_remote_port, LibraryScreen::field_ssh_port,
                      LibraryScreen::field_local_port, LibraryScreen::field_width,
                      LibraryScreen::field_height})
        check(LibraryScreen::max_field_bytes(Kind::editor_field, field) == 5,
              std::string("a numeric field holds 5 digits: ") + std::to_string(field));
    for (int field : {LibraryScreen::field_identity_file,
                      LibraryScreen::field_known_hosts_file,
                      LibraryScreen::field_cookie_source})
        check(LibraryScreen::max_field_bytes(Kind::editor_field, field) == 4095,
              "a path field holds PATH_MAX less its NUL: 4095 bytes");
    check(LibraryScreen::max_field_bytes(Kind::editor_field, LibraryScreen::field_name) == 255
              && LibraryScreen::max_field_bytes(Kind::editor_field,
                                                LibraryScreen::field_ssh_user) == 255,
          "name and SSH user hold 255 bytes");
    check(LibraryScreen::max_field_bytes(Kind::search) == 255
              && LibraryScreen::max_field_bytes(Kind::quick_host) == 509
              && LibraryScreen::max_field_bytes(Kind::transfer_path) == 4095,
          "search 255, quick connect user@host 509, import/export path 4095");
    check(LibraryScreen::max_field_bytes(Kind::card) == 0, "a card is not a text field");

    TempDir dir;
    ConnectionLibrary library = library_in(dir);
    library.add(make_profile("alpha", "a.example")); // so a miss says "No connections match"
    std::string clip;
    const auto run = [&](std::size_t length, std::uint64_t& paste, std::uint64_t& first,
                         std::uint64_t& second, std::uint64_t& click_cost) {
        LibraryScreen screen(library, 1024, 700);
        screen.set_clipboard([&] { return clip; }, [](const std::string&) {});
        screen.render();
        clip.assign(length, 'W');
        paste = layout_cost(screen, [&] { screen.key(Key::paste); });
        check(screen.search_text().size() == length && screen.input_error().empty(),
              "a " + std::to_string(length) + "-byte paste within the limit is taken whole");
        first = layout_cost(screen, [&] { screen.render(); });
        second = layout_cost(screen, [&] { screen.render(); });
        const auto* box = find_region(screen, Kind::search);
        click_cost = box == nullptr ? 1 : layout_cost(screen, [&] {
            screen.pointer_press(box->x + box->w / 2, box->y + box->h / 2);
            screen.pointer_move(box->x + 20, box->y + box->h / 2);
            screen.pointer_release(box->x + 20, box->y + box->h / 2);
        });
        check(screen.caret() < length, "the click moved the caret into the text");
    };
    // At the limit, and a shorter text that still overflows the box (200 W is
    // wider than the 960 px search at any font this could pick, and than
    // the elided "No connections match" line).
    std::uint64_t paste_n = 0, first_n = 0, second_n = 0, click_n = 0;
    std::uint64_t paste_k = 0, first_k = 0, second_k = 0, click_k = 0;
    run(255, paste_n, first_n, second_n, click_n);
    run(200, paste_k, first_k, second_k, click_k);
    check(paste_n == 0 && paste_k == 0, "a paste lays out nothing until the next render");
    check(first_n - second_n == 255,
          "the first render after a 255-byte paste lays the field out exactly once: "
              + std::to_string(first_n - second_n) + " code points more than the next");
    check(first_k - second_k == 200, "...and a 200-byte one exactly once ("
                                         + std::to_string(first_k - second_k) + ")");
    check(second_n == second_k,
          "a frame costs the same at 255 bytes as at 200 (" + std::to_string(second_n)
              + " vs " + std::to_string(second_k) + " code points): only what is in view is"
              " drawn and only what fits is elided");
    check(click_n == 0 && click_k == 0,
          "placing the caret by click and drag lays out nothing ("
              + std::to_string(click_n) + ", " + std::to_string(click_k) + ")");

    // Over the limit: refused, visibly, whatever the size, for the same cost.
    const auto refuse = [&](std::size_t length, std::uint64_t& paste, std::uint64_t& frame) {
        LibraryScreen screen(library, 1024, 700);
        screen.set_clipboard([&] { return clip; }, [](const std::string&) {});
        screen.render();
        screen.text_input("al");
        screen.render();
        clip.assign(length, 'W');
        paste = layout_cost(screen, [&] { screen.key(Key::paste); });
        check(screen.search_text() == "al",
              "a " + std::to_string(length) + "-byte paste into search inserts nothing");
        check(screen.input_error() == "Search takes at most 255 bytes; the text was not inserted."
                  && screen.input_error_field().kind == Kind::search,
              "the refusal is said against the search field: '" + screen.input_error() + "'");
        frame = layout_cost(screen, [&] { screen.render(); });
        const auto* box = find_region(screen, Kind::search);
        if (box != nullptr)
            check_pixel(screen.surface(), box->x, box->y + box->h / 2,
                        Color {178, 50, 40, 255}, "the refusing search box has a danger border");
        screen.text_input("p");
        check(screen.search_text() == "alp" && screen.input_error().empty(),
              "the next edit that fits is taken and clears the refusal");
        screen.render();
        if (box != nullptr)
            check_pixel(screen.surface(), box->x, box->y + box->h / 2,
                        Color {37, 89, 189, 255}, "...and the border is the focus accent again");
    };
    std::uint64_t paste_mib = 0, frame_mib = 0, paste_over = 0, frame_over = 0;
    refuse(std::size_t {1} << 20, paste_mib, frame_mib);
    refuse(254, paste_over, frame_over); // one past the limit with "al" in it
    check(paste_mib == 0 && paste_over == 0,
          "refusing a paste lays out nothing (1 MiB: " + std::to_string(paste_mib) + ")");
    check(frame_mib == frame_over,
          "the frame after refusing 1 MiB costs what it costs after refusing 254 bytes ("
              + std::to_string(frame_mib) + " vs " + std::to_string(frame_over) + ")");

    // The editor: Host at exactly 253 is taken; one byte more is not, and says
    // so in the field's own error line; replacing a selection counts only what
    // it leaves.
    LibraryScreen editor(library, 1024, 700);
    editor.set_clipboard([&] { return clip; }, [](const std::string&) {});
    editor.render();
    click(editor, Kind::new_connection);
    editor.render();
    clip.assign(253, 'h');
    editor.key(Key::paste);
    check(editor.editor_text(LibraryScreen::field_host).size() == 253
              && editor.input_error().empty(),
          "253 bytes fill Host exactly");
    editor.text_input("x");
    check(editor.editor_text(LibraryScreen::field_host).size() == 253
              && editor.input_error() == "Host takes at most 253 bytes; the text was not inserted."
              && editor.input_error_field().field == LibraryScreen::field_host,
          "a 254th byte is refused against Host: '" + editor.input_error() + "'");
    editor.render();
    const auto* host = find_region(editor, Kind::editor_field, {}, LibraryScreen::field_host);
    if (host != nullptr)
        check_pixel(editor.surface(), host->x, host->y + host->h / 2,
                    Color {178, 50, 40, 255}, "the refusing Host field has a danger border");
    editor.key(Key::select_all);
    editor.key(Key::paste);
    check(editor.editor_text(LibraryScreen::field_host) == clip && editor.input_error().empty(),
          "253 bytes over a 253-byte selection replace it");
    // Typing digits into a port stops at five.
    set_editor_field(editor, LibraryScreen::field_ssh_port, "65535");
    editor.text_input("1");
    check(editor.editor_text(LibraryScreen::field_ssh_port) == "65535"
              && editor.input_error() == "SSH port takes at most 5 bytes; the text was not inserted.",
          "a sixth digit in SSH port is refused: '" + editor.input_error() + "'");
    // Another field's focus drops the refusal; it belonged to SSH port.
    set_editor_field(editor, LibraryScreen::field_identity_file, "");
    check(editor.input_error().empty(), "focusing another field drops the refusal");

    // An unfocused value costs what it shows: a 4095-byte path and a 1000-byte
    // one render for the same number of code points.
    const auto unfocused_frame = [&](std::size_t length) {
        clip.assign(length, 'p');
        set_editor_field(editor, LibraryScreen::field_identity_file, "");
        editor.key(Key::paste);
        check(editor.editor_text(LibraryScreen::field_identity_file).size() == length,
              "the path field takes " + std::to_string(length) + " bytes");
        set_editor_field(editor, LibraryScreen::field_known_hosts_file, "k");
        editor.render();
        return layout_cost(editor, [&] { editor.render(); });
    };
    const std::uint64_t path_long = unfocused_frame(4095);
    const std::uint64_t path_short = unfocused_frame(1000);
    check(path_long == path_short,
          "an unfocused 4095-byte path costs a frame what a 1000-byte one does ("
              + std::to_string(path_long) + " vs " + std::to_string(path_short) + ")");
}

} // namespace

int main()
{
    // The screens' text draws with the committed faces alone, so the pixel
    // checks below hold on every host (tests/test_fonts.hpp).
    test_fonts::pin();
    {
        const std::string problem = test_fonts::pin_problem();
        check(problem.empty(),
              "text draws with the committed faces, not the host's (" + problem + ")");
    }
    test_library_crud();
    test_order_and_search();
    test_mark_connected_reorders();
    test_persist_round_trip();
    test_new_id_unique();
    test_plan_launch();
    test_screen_new_connection_flow();
    test_screen_mode_chip_cycles_the_modes();
    test_screen_connect_request();
    test_screen_delete_and_favorite();
    test_quick_connect();
    test_card_text();
    test_editor_validation_lists_every_error();
    test_import_export();
    test_keyboard_reachability();
    test_theme_pixels_and_focus_ring();
    test_layout_at_1024x700_and_high_dpi();
    test_text_editing_model();
    test_text_pointer_editing();
    test_caret_selection_and_blink_pixels();
    test_editor_scrolls();
    test_list_wheel_scrolls_cards();
    test_text_engine_stops_and_fit();
    test_text_field_scale();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " library checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " library checks FAILED\n";
    return 1;
}
