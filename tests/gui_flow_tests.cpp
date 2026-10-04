// Tests for the GUI connection-library flow (issue #1, PR4). The SDL shell is a
// thin renderer over LibraryController, a UI-free state machine in
// haiku_remote_core; this exercises that machine end to end -- browse, CRUD,
// favorite, connect, cancel, fail, dismiss -- with a temp ProfileStore and a
// fake-driven ConnectionCoordinator, so it runs with no SDL, no window, no
// sockets, no ssh. The focusable text field's edit model is covered here too,
// since it is the one widget with nontrivial (UTF-8, cursor) state.
//
// House rules (CLAUDE.md): assert exact values; keep expectations independent of
// the code that produces them; give asymmetric subjects (a transport that
// delivers a frame reaches connected and is written back as a success; one that
// delivers nothing fails and is written back as an actionable reason).

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/library_controller.hpp"
#include "haiku_remote/protocol.hpp"
#include "haiku_remote/text_field_model.hpp"
#include "haiku_remote/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h> // getpid, for a unique temp-store path

using namespace haiku_remote;
using namespace std::chrono_literals;

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

std::vector<std::uint8_t> one_message()
{
    return Writer(Op::update_display_mode).finish();
}

// A unique scratch file so a test run never collides with a real library or
// another run. Removed by RAII.
struct TempStore {
    std::filesystem::path path;
    TempStore()
    {
        const auto base = std::filesystem::temp_directory_path();
        path = base
            / ("haiku-remote-gui-flow-"
               + std::to_string(::getpid()) + "-"
               + std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count())
               + ".json");
    }
    ~TempStore() { std::error_code ec; std::filesystem::remove(path, ec); }
    [[nodiscard]] ProfileStore store() const { return ProfileStore(path); }
};

// ---- the fakes the coordinator is driven by (mirrors coordinator_tests) ----

class FakeTransport final : public Transport {
public:
    enum class End { timeout, clean_fin };
    bool deliver_frame = true;
    End end = End::timeout;

    bool connect(std::string&) override { return true; }
    bool send_all(std::span<const std::uint8_t>, std::string&) override { return true; }
    int receive(std::span<std::uint8_t> destination, int timeout_ms,
                std::string& error) override
    {
        if (deliver_frame && !delivered_) {
            delivered_ = true;
            const auto frame = one_message();
            std::copy(frame.begin(), frame.end(), destination.begin());
            return static_cast<int>(frame.size());
        }
        if (end == End::clean_fin) {
            peer_closed_ = true;
            error = "peer closed";
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
        return 0;
    }
    void close() override {}
    [[nodiscard]] bool peer_closed() const override { return peer_closed_; }
    [[nodiscard]] bool connection_reset() const override { return false; }
    [[nodiscard]] std::string describe() const override { return "fake://endpoint"; }

private:
    bool delivered_ = false;
    bool peer_closed_ = false;
};

class FakeTunnel final : public ISshTunnel {
public:
    explicit FakeTunnel(int* starts, int* stops) : starts_(starts), stops_(stops) {}
    TunnelStatus start() override
    {
        if (starts_) ++*starts_;
        running_ = true;
        return TunnelStatus{TunnelState::running, 15921, {}};
    }
    void stop() override { if (stops_) ++*stops_; running_ = false; }
    [[nodiscard]] TunnelState state() const override
    {
        return running_ ? TunnelState::running : TunnelState::stopped;
    }
    [[nodiscard]] int local_port() const override { return 15921; }
    [[nodiscard]] const std::string& stderr_log() const override { return log_; }
    [[nodiscard]] std::optional<std::string> fetch_session_cookie() override
    {
        return std::string{"boot-cookie"};
    }

private:
    int* starts_;
    int* stops_;
    bool running_ = false;
    std::string log_;
};

// A coordinator factory that reaches `connected`: delivers one frame and then
// idles. handshake_timeout is shortened so a failing variant fails fast.
LibraryController::CoordinatorFactory connecting_factory()
{
    return [](CoordinatorConfig config) {
        auto transport_factory = [](const TransportOptions&, std::string&) {
            auto t = std::make_unique<FakeTransport>();
            t->deliver_frame = true;
            t->end = FakeTransport::End::timeout;
            return std::unique_ptr<Transport>(std::move(t));
        };
        return std::make_unique<ConnectionCoordinator>(std::move(config),
                                                       transport_factory);
    };
}

ConnectionProfile direct_profile(std::string name, std::string host = "127.0.0.1")
{
    ConnectionProfile p;
    p.name = std::move(name);
    p.mode = ConnectionMode::direct;
    p.host = std::move(host);
    p.remote_port = 10900;
    p.width = 320;
    p.height = 240;
    p.auto_reconnect = false;
    return p;
}

template <class Predicate>
bool pump(LibraryController& controller, Predicate predicate, int timeout_ms = 4000)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        controller.poll();
        if (predicate())
            return true;
        std::this_thread::sleep_for(2ms);
    }
    controller.poll();
    return predicate();
}

// ---- the library starts empty and on the library screen ----

void test_fresh_library_opens_empty_on_library_screen()
{
    TempStore temp;
    LibraryController controller(temp.store());
    const LoadResult loaded = controller.reload();
    check(loaded.status == LoadStatus::missing, "a fresh library has no file");
    check(controller.mode() == AppMode::Library, "launch opens on the library");
    check(controller.profiles().empty(), "and it is empty");
}

// ---- CRUD + favorite, each persisted and reordered ----

void test_crud_and_favorite_persist_and_reorder()
{
    TempStore temp;
    {
        LibraryController controller(temp.store());
        controller.reload();

        controller.open_new_editor();
        check(controller.mode() == AppMode::Editor, "New connection opens the editor");
        check(controller.editing().id.empty(), "a new profile has no id yet");

        const SaveResult saved = controller.commit_profile(direct_profile("alpha"));
        check(saved.ok, "a valid new profile saves");
        check(controller.mode() == AppMode::Library, "saving returns to the library");
        check(controller.profiles().size() == 1, "one profile now exists");
        check(!controller.profiles().front().id.empty(),
              "an id was assigned on create");

        // A second profile, then favorite it: a favorite sorts to the front.
        controller.commit_profile(direct_profile("beta", "10.0.0.2"));
        check(controller.profiles().size() == 2, "two profiles now exist");

        std::size_t beta_index = controller.profiles().size();
        for (std::size_t i = 0; i < controller.profiles().size(); ++i)
            if (controller.profiles()[i].name == "beta")
                beta_index = i;
        check(beta_index < controller.profiles().size(), "beta is present");
        const SaveResult fav = controller.toggle_favorite(beta_index);
        check(fav.ok, "toggling favorite saves");
        check(controller.profiles().front().name == "beta",
              "a favorite sorts to the front of the library");
        check(controller.profiles().front().favorite, "and is marked favorite");

        // Duplicate the favorite: the copy is not itself a favorite.
        const SaveResult dup = controller.duplicate(0);
        check(dup.ok, "duplicate saves");
        check(controller.profiles().size() == 3, "duplicate adds a profile");
        bool found_copy = false;
        for (const auto& p : controller.profiles())
            if (p.name == "beta (copy)") {
                found_copy = true;
                check(!p.favorite, "a duplicate is not pinned");
            }
        check(found_copy, "the duplicate is named for its origin");

        // Delete alpha.
        std::size_t alpha_index = controller.profiles().size();
        for (std::size_t i = 0; i < controller.profiles().size(); ++i)
            if (controller.profiles()[i].name == "alpha")
                alpha_index = i;
        const SaveResult del = controller.remove(alpha_index);
        check(del.ok, "delete saves");
        check(controller.profiles().size() == 2, "delete removes a profile");
        for (const auto& p : controller.profiles())
            check(p.name != "alpha", "alpha is gone");
    }
    // Reopen from disk: persistence survived the controller.
    {
        LibraryController fresh(temp.store());
        const LoadResult loaded = fresh.reload();
        check(loaded.status == LoadStatus::loaded, "the saved file reloads");
        check(fresh.profiles().size() == 2, "two profiles persisted to disk");
        check(fresh.profiles().front().name == "beta",
              "favorite-first ordering survives a reload");
    }
}

// ---- validation names the field and does not mutate ----

void test_invalid_profile_is_rejected_and_names_the_field()
{
    TempStore temp;
    LibraryController controller(temp.store());
    controller.reload();
    controller.open_new_editor();

    ConnectionProfile bad = direct_profile("bad");
    bad.remote_port = 70000; // out of range
    const SaveResult result = controller.commit_profile(bad);
    check(!result.ok, "an out-of-range port is rejected");
    check(result.invalid_field.field == "remotePort",
          "the offending field is named");
    check(controller.profiles().empty(), "the library is left unchanged");
    check(controller.mode() == AppMode::Editor,
          "a rejected save stays in the editor so the user can fix it");
}

// ---- a direct connect reaches the desktop, then returns to the library ----

void test_direct_connect_then_disconnect_returns_to_library()
{
    TempStore temp;
    LibraryController controller(temp.store(), connecting_factory());
    controller.reload();
    controller.commit_profile(direct_profile("desktop"));

    controller.connect(0);
    check(controller.mode() == AppMode::Connecting, "connect enters the connecting view");

    const bool connected = pump(controller,
                                [&] { return controller.mode() == AppMode::Connected; });
    check(connected, "a delivered frame reaches the connected desktop view");

    // Closing the session returns to the library, not process exit.
    controller.cancel();
    const bool home = pump(controller,
                           [&] { return controller.mode() == AppMode::Library; });
    check(home, "a user disconnect returns to the library");
    check(controller.coordinator() == nullptr, "the coordinator is torn down");

    // The successful connection was written back onto the saved profile.
    bool recorded = false;
    for (const auto& p : controller.profiles())
        if (p.name == "desktop" && p.last_connected_at > 0)
            recorded = true;
    check(recorded, "a successful connection stamps last-connected on the profile");
}

// ---- an ssh connect drives the tunnel with no separate terminal ----

void test_ssh_connect_brings_up_the_owned_tunnel()
{
    TempStore temp;
    int starts = 0;
    int stops = 0;
    auto factory = [&](CoordinatorConfig config) {
        auto transport_factory = [](const TransportOptions&, std::string&) {
            auto t = std::make_unique<FakeTransport>();
            return std::unique_ptr<Transport>(std::move(t));
        };
        auto tunnel_factory = [&](const TunnelConfig&, const TunnelOptions&) {
            return std::unique_ptr<ISshTunnel>(
                std::make_unique<FakeTunnel>(&starts, &stops));
        };
        return std::make_unique<ConnectionCoordinator>(std::move(config),
                                                       transport_factory,
                                                       tunnel_factory);
    };
    LibraryController controller(temp.store(), factory);
    controller.reload();

    ConnectionProfile ssh;
    ssh.name = "ec2";
    ssh.mode = ConnectionMode::ssh;
    ssh.host = "ec2.example";
    ssh.ssh_user = "baron";
    ssh.remote_port = 10900;
    ssh.width = 320;
    ssh.height = 240;
    ssh.auto_reconnect = false;
    controller.commit_profile(ssh);

    controller.connect(0);
    const bool connected = pump(controller,
                                [&] { return controller.mode() == AppMode::Connected; });
    check(connected, "an ssh profile connects through its owned tunnel");
    check(starts == 1, "the tunnel was started by the controller, not a terminal");

    controller.cancel();
    pump(controller, [&] { return controller.mode() == AppMode::Library; });
    check(stops >= 1, "the tunnel is stopped on disconnect (no orphan left behind)");
}

// ---- a failed connect holds on the failure screen with a copyable diag ----

void test_failed_connect_shows_reason_and_diag_then_dismisses()
{
    TempStore temp;
    auto factory = [](CoordinatorConfig config) {
        config.handshake_timeout = 150ms; // fail fast: never deliver a frame
        auto transport_factory = [](const TransportOptions&, std::string&) {
            auto t = std::make_unique<FakeTransport>();
            t->deliver_frame = false;
            t->end = FakeTransport::End::timeout;
            return std::unique_ptr<Transport>(std::move(t));
        };
        return std::make_unique<ConnectionCoordinator>(std::move(config),
                                                       transport_factory);
    };
    LibraryController controller(temp.store(), factory);
    controller.reload();
    controller.commit_profile(direct_profile("doomed"));

    controller.connect(0);
    const bool failed = pump(controller,
                             [&] { return controller.mode() == AppMode::Failed; });
    check(failed, "a handshake that never completes lands on the failure view");

    const Snapshot snap = controller.snapshot();
    check(snap.reason == ConnectionReason::no_rp_init_ack,
          "the actionable reason is no-RP_INIT-ack");
    check(!snap.diag.empty(), "a bounded diagnostic log is available to copy");
    check(controller.coordinator() != nullptr,
          "the coordinator is kept so the failure view can read its snapshot");

    const ConnectionProfile* connecting = controller.connecting_profile();
    check(connecting != nullptr && !connecting->last_error.empty(),
          "the failure is written back as the profile's last error");

    controller.dismiss_failure();
    check(controller.mode() == AppMode::Library, "dismissing returns to the library");
    check(controller.coordinator() == nullptr, "and tears the coordinator down");
}

// ---- an ephemeral (CLI bypass) connection does not touch the library ----

void test_ephemeral_connect_does_not_write_back_to_the_library()
{
    TempStore temp;
    LibraryController controller(temp.store(), connecting_factory());
    controller.reload();
    check(controller.profiles().empty(), "no saved profiles");

    CoordinatorConfig config = coordinator_config_for(direct_profile("cli"), "cookie");
    controller.connect_config(config);
    check(controller.mode() == AppMode::Connecting, "an ephemeral connect skips the library");
    check(controller.connecting_profile() == nullptr,
          "an ephemeral connection is not a saved profile");

    pump(controller, [&] { return controller.mode() == AppMode::Connected; });
    controller.cancel();
    pump(controller, [&] { return controller.mode() == AppMode::Library; });
    check(controller.profiles().empty(),
          "an ephemeral connection leaves the library empty");
}

// ---- the focusable text field's edit model (UTF-8, cursor) ----

void test_text_field_model_edits_on_code_point_boundaries()
{
    TextFieldModel field;
    field.insert("ab");
    check(field.text() == "ab" && field.cursor() == 2, "insert appends at the cursor");
    field.move_left();
    field.insert("X");
    check(field.text() == "aXb", "insert lands at the cursor, not the end");
    check(field.cursor() == 2, "and the cursor advances past the insert");
    field.backspace();
    check(field.text() == "ab", "backspace deletes the code point before the cursor");

    field.set_text("he\xC3\xA9llo"); // "heéllo" -- é is two UTF-8 bytes
    field.move_home();
    field.move_right();
    field.move_right();              // cursor sits just before 'é'
    check(field.cursor() == 2, "two single-byte steps leave the cursor before é");
    field.del();                     // delete the whole 2-byte 'é'
    check(field.text() == "hello", "del removes a whole multi-byte code point");
    check(field.cursor() == 2, "and the cursor does not move on delete");

    field.move_end();
    field.backspace();
    check(field.text() == "hell", "backspace at the end trims the last byte");

    TextFieldModel empty;
    empty.backspace();
    empty.del();
    empty.move_left();
    empty.move_right();
    check(empty.text().empty() && empty.cursor() == 0,
          "edits on an empty field are safe no-ops");
}

} // namespace

int main()
{
    test_fresh_library_opens_empty_on_library_screen();
    test_crud_and_favorite_persist_and_reorder();
    test_invalid_profile_is_rejected_and_names_the_field();
    test_direct_connect_then_disconnect_returns_to_library();
    test_ssh_connect_brings_up_the_owned_tunnel();
    test_failed_connect_shows_reason_and_diag_then_dismisses();
    test_ephemeral_connect_does_not_write_back_to_the_library();
    test_text_field_model_edits_on_code_point_boundaries();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " gui-flow checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " gui-flow checks failed\n";
    return 1;
}
