#pragma once

// HAIKU_REMOTE_TEST_SCRIPT: scripted input for the end-to-end test
// (tests/e2e/profile_e2e.py), and for nothing else. Unset -- every real run --
// there is no step and every hook below is a no-op. Steps are separated by ';'
// and consumed strictly in order, each "<screen>:<action>":
//
//   library:connect=<profile name>   click that card's Connect button, as a
//                                    user would (a real window-system event)
//   library:close                    close the library window
//   session:close-when=[min=N,]X,Y,RRGGBB[,X,Y,RRGGBB...]
//                                    once every listed desktop pixel has that
//                                    exact value (and the session has decoded
//                                    at least N messages), report it and close
//                                    the window (a user disconnecting); if they
//                                    do not within 10 s, report that and close
//   session:hold-when=...            the same, but report and keep the desktop
//                                    open (the harness then signals the app)
//   connect-error:back               leave the connect error screen by Back
//   reconnect:close                  once a desktop's reconnect attempt has
//                                    been in one stage for 500 ms (so its ssh
//                                    is running), close the window -- the
//                                    attempt, on its worker, must not hold it
//   reconnect:resume                 let the reconnect run; a "session:" step
//                                    after it is then checked against the
//                                    reconnected session's frames
//   clipboard:echo                   (X11 front end) at the next paste, read
//                                    the X CLIPBOARD selection as usual, then
//                                    take it with exactly the bytes read, and
//                                    leave the text field alone: a selection
//                                    can cross both ways at a size (1 MiB) no
//                                    single-line field is meant to hold
//
// Every hook reports what it did on stderr as an "e2e: ..." line, which is what
// the harness asserts on. It lives here, not in a front end, so the SDL and X11
// front ends are driven -- and judged -- by the same script and the same lines.

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/library_screen.hpp"
#include "haiku_remote/profile_library.hpp"
#include "haiku_remote/session.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

// How the running front end closes the window that is open now, the way a user
// does (a close request, never an application quit). Set by the front end
// before it opens its first window.
void set_test_script_close_window(std::function<void()> close);

// True for the whole run when a script was given, even once its steps are used.
[[nodiscard]] bool test_script_active();

// The head step's action if it is for `screen`, consuming it.
[[nodiscard]] std::optional<std::string> test_script_take(std::string_view screen);

// Close the current window through the front end's hook.
void test_script_close_window();

// Run as a desktop's reconnect attempt starts.
void test_script_reconnect_started();

// Run on every pass of the desktop loop while a reconnect is running, with the
// stage it is in. True while a reconnect:close is pending, so the loop wakes to
// look again.
bool test_script_reconnect_tick(const std::string& stage);

// Every stage the connect worker entered, in order, as "e2e: stage <label>".
// `reported` counts the stages already printed.
void test_script_report_stages(const DiagnosticLog& log, std::size_t& reported);

// A pending "session:close-when" / "session:hold-when" step, parsed.
struct TestPixelWait {
    struct Probe {
        int x = 0;
        int y = 0;
        std::string rgb;
    };
    std::vector<Probe> probes;
    std::size_t min_messages = 0;
    bool close = true; // close-when; false for hold-when
    std::chrono::steady_clock::time_point give_up;
};

// The next step if it is a "session:" one, consuming it.
[[nodiscard]] std::optional<TestPixelWait> test_script_pixel_wait();

// Called after each presented session frame. True once the step is settled
// (and the window asked to close, for close-when).
bool test_script_check_frame(const TestPixelWait& wait, const Session& session);

// The library half, run after each repaint (the card regions are only known
// once the list has been laid out). `click` delivers a real primary-button
// press at (x, y) in the library's surface coordinates.
void test_script_library_step(const LibraryScreen& ui,
                              const ConnectionLibrary& library,
                              const std::function<void(int x, int y)>& click);

} // namespace haiku_remote
