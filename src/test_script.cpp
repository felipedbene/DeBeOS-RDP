#include "haiku_remote/test_script.hpp"

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/launch_arguments.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace haiku_remote {

namespace {

std::vector<std::string>& test_script_steps()
{
    static std::vector<std::string> steps = [] {
        std::vector<std::string> parsed;
        const char* text = std::getenv("HAIKU_REMOTE_TEST_SCRIPT");
        std::string_view rest = text == nullptr ? "" : text;
        while (!rest.empty()) {
            const auto end = rest.find(';');
            if (end != 0)
                parsed.emplace_back(rest.substr(0, end));
            rest = end == std::string_view::npos ? "" : rest.substr(end + 1);
        }
        return parsed;
    }();
    return steps;
}

std::function<void()>& close_hook()
{
    static std::function<void()> hook;
    return hook;
}

std::string hex_color(Color c)
{
    char text[8];
    std::snprintf(text, sizeof(text), "%02x%02x%02x", c.r, c.g, c.b);
    return text;
}

// A pending reconnect:close: when the stage it is waiting in became active.
struct TestReconnectClose {
    bool pending = false;
    std::string stage;
    std::chrono::steady_clock::time_point since;
};

TestReconnectClose& test_reconnect_close()
{
    static TestReconnectClose state;
    return state;
}

} // namespace

void set_test_script_close_window(std::function<void()> close)
{
    close_hook() = std::move(close);
}

bool test_script_active()
{
    static const bool active = [] {
        const char* text = std::getenv("HAIKU_REMOTE_TEST_SCRIPT");
        return text != nullptr && text[0] != '\0';
    }();
    return active;
}

std::optional<std::string> test_script_take(std::string_view screen)
{
    auto& steps = test_script_steps();
    if (steps.empty())
        return std::nullopt;
    const std::string& head = steps.front();
    if (head.size() <= screen.size() || head.compare(0, screen.size(), screen) != 0
        || head[screen.size()] != ':')
        return std::nullopt;
    std::string action = head.substr(screen.size() + 1);
    steps.erase(steps.begin());
    return action;
}

void test_script_close_window()
{
    if (close_hook())
        close_hook()();
}

void test_script_reconnect_started()
{
    const auto action = test_script_take("reconnect");
    if (!action)
        return;
    if (*action == "close") {
        test_reconnect_close() = TestReconnectClose {true, {}, {}};
        return;
    }
    if (*action == "resume") {
        write_stderr_line("e2e: reconnect attempt running");
        return;
    }
    write_stderr_line("e2e: bad reconnect step \"" + *action + "\"");
}

bool test_script_reconnect_tick(const std::string& stage)
{
    TestReconnectClose& close = test_reconnect_close();
    if (!close.pending)
        return false;
    const auto now = std::chrono::steady_clock::now();
    if (stage.empty() || stage != close.stage) {
        close.stage = stage;
        close.since = now;
        return true;
    }
    if (now - close.since < std::chrono::milliseconds(500))
        return true;
    write_stderr_line("e2e: reconnect attempt running (" + stage
                      + "); closing the window");
    close.pending = false;
    test_script_close_window();
    return false;
}

// Read from the attempt's diagnostic log, which records each stage as
// "+T.TTTs stage <kind> (route N): <label>": the UI only sees snapshots of the
// flow, and a stage the worker passed through between two wakes would be
// missing from them.
void test_script_report_stages(const DiagnosticLog& log, std::size_t& reported)
{
    if (!test_script_active())
        return;
    std::size_t seen = 0;
    for (const std::string& line : log.lines()) {
        const std::size_t at = line.find(" stage ");
        const std::size_t label = line.find("): ", at == std::string::npos ? 0 : at);
        if (at == std::string::npos || label == std::string::npos)
            continue;
        if (++seen > reported) {
            write_stderr_line("e2e: stage " + line.substr(label + 3));
            reported = seen;
        }
    }
}

std::optional<TestPixelWait> test_script_pixel_wait()
{
    const auto action = test_script_take("session");
    if (!action)
        return std::nullopt;
    constexpr std::string_view prefix = "close-when=";
    constexpr std::string_view hold = "hold-when=";
    TestPixelWait wait;
    wait.give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    wait.close = action->rfind(hold, 0) != 0;
    const std::size_t skip = wait.close ? prefix.size() : hold.size();
    if (action->rfind(prefix, 0) == 0 || !wait.close) {
        std::vector<std::string> fields;
        std::string_view rest = std::string_view(*action).substr(skip);
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            fields.emplace_back(rest.substr(0, comma));
            rest = comma == std::string_view::npos ? "" : rest.substr(comma + 1);
        }
        std::size_t first = 0;
        if (!fields.empty() && fields[0].rfind("min=", 0) == 0) {
            wait.min_messages = std::stoul(fields[0].substr(4));
            first = 1;
        }
        for (std::size_t i = first; i + 2 < fields.size(); i += 3)
            wait.probes.push_back(
                {std::stoi(fields[i]), std::stoi(fields[i + 1]), fields[i + 2]});
    }
    if (wait.probes.empty())
        write_stderr_line("e2e: bad session step \"" + *action + "\"");
    return wait;
}

bool test_script_check_frame(const TestPixelWait& wait, const Session& session)
{
    bool all = !wait.probes.empty() && session.message_count() >= wait.min_messages;
    std::string seen;
    for (const auto& probe : wait.probes) {
        const std::string rgb = hex_color(session.surface().pixel(probe.x, probe.y));
        seen += " (" + std::to_string(probe.x) + "," + std::to_string(probe.y)
                + ")=" + rgb;
        all = all && rgb == probe.rgb;
    }
    const bool timed_out = std::chrono::steady_clock::now() >= wait.give_up;
    if (!all && !timed_out)
        return false;
    write_stderr_line(std::string(all ? "e2e: desktop presented after "
                                      : "e2e: desktop NOT presented after ")
                      + std::to_string(session.message_count()) + " messages:" + seen);
    if (wait.close)
        test_script_close_window();
    else
        write_stderr_line("e2e: holding the desktop open");
    return true;
}

void test_script_library_step(const LibraryScreen& ui,
                              const ConnectionLibrary& library,
                              const std::function<void(int x, int y)>& click)
{
    const auto action = test_script_take("library");
    if (!action)
        return;
    if (*action == "close") {
        write_stderr_line("e2e: closing the library");
        test_script_close_window();
        return;
    }
    constexpr std::string_view prefix = "connect=";
    if (action->rfind(prefix, 0) == 0) {
        const std::string name = action->substr(prefix.size());
        const ConnectionProfile* profile = find_profile(library.profiles(), name);
        for (const auto& region : ui.regions()) {
            if (profile == nullptr
                || region.kind != LibraryScreen::Region::Kind::connect
                || region.profile_id != profile->id)
                continue;
            write_stderr_line("e2e: clicking Connect on \"" + name + "\"");
            click(region.x + region.w / 2, region.y + region.h / 2);
            return;
        }
    }
    write_stderr_line("e2e: library step \"" + *action + "\" matched nothing; closing");
    test_script_close_window();
}

} // namespace haiku_remote
