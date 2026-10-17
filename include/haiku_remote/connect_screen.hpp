#pragma once

// DeBeOS-RDP issue #1 Part 3: the connection-progress / error screen.
//
// The immediate-mode view shown from the moment the user presses Connect until
// the session starts -- and, if the connect fails, the actionable error that
// replaces the old silent black window. It is drawn with exactly the toolkit
// LibraryScreen uses (a software Surface plus TextEngine) and nothing else, so
// it builds in haiku_remote_core and both the SDL and X11 frontends share it
// verbatim; a frontend only translates its events and blits the finished frame.
//
// It reads a ConnectFlow by reference -- a front end running the connect on a
// worker (AsyncConnect) points it at its own snapshot copy and refreshes that
// -- and renders whatever state the flow is in: the stage checklist with the
// elapsed time and a Cancel button while connecting, or the error with its
// suggested fix, the tail of the diagnostic log, and Back / Retry / Copy
// diagnostics once the flow has failed. It opens no socket and runs no
// process; the dependency arrow is one way, frontend -> screen -> flow.

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/known_brokers.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

class ConnectScreen {
public:
    // trust / replace / reject are only ever produced while a trust prompt is
    // showing (show_trust_prompt); back / retry / copy_log only on the error
    // screen; cancel only while connecting.
    enum class Action { none, back, retry, trust, replace, reject, cancel, copy_log };

    // tab / back_tab move a keyboard focus (drawn as the library's 2 px focus
    // ring) among Cancel, Back, Retry and Copy diagnostics; Enter presses the
    // focused button.
    enum class Key { none, enter, escape, tab, back_tab };

    struct Region {
        enum class Kind {
            none, back, retry, trust, dont_trust, replace, cancel, copy_log
        };
        Kind kind = Kind::none;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;

        [[nodiscard]] bool hit(int px, int py) const
        {
            return px >= x && px < x + w && py >= y && py < y + h;
        }
    };

    // `title` is the connection's display name (e.g. the profile name), shown as
    // the heading. The flow must outlive the screen.
    ConnectScreen(const ConnectFlow& flow, std::string title, int width,
                  int height);

    void resize(int width, int height);
    [[nodiscard]] int width() const { return surface_.width(); }
    [[nodiscard]] int height() const { return surface_.height(); }

    void pointer_move(int x, int y);
    void pointer_press(int x, int y);
    void key(Key k);

    const Surface& render();
    [[nodiscard]] const Surface& surface() const { return surface_; }
    [[nodiscard]] bool dirty() const { return dirty_; }
    void mark_dirty() { dirty_ = true; }

    // One-shot: the Back/Retry the user chose on the error screen, or the
    // Trust / Replace / Reject chosen on a trust prompt. Reading it clears it.
    [[nodiscard]] Action take_action();

    // Trust on first use: overlay the broker-certificate question for `check`
    // (state unknown or changed) until clear_trust_prompt(). Unknown offers
    // "Don't trust" and "Trust and connect"; changed shows the loud warning and
    // offers "Cancel" and "Replace the stored fingerprint and connect". In both,
    // Escape and Enter mean the refusing choice: accepting a certificate takes
    // a click on the button that says so, never a keystroke.
    void show_trust_prompt(const BrokerCheck& check);
    void clear_trust_prompt();
    [[nodiscard]] bool trust_prompt_active() const { return prompt_.has_value(); }

    [[nodiscard]] const std::vector<Region>& regions() const { return regions_; }

    // Shown on the progress screen. Marks the screen dirty only when the
    // displayed (whole-second) value changes.
    void set_elapsed(std::chrono::milliseconds elapsed);
    // Cancel was requested: the button gives way to "Cancelling..." and
    // Escape does nothing more.
    void set_cancelling(bool cancelling);
    [[nodiscard]] bool cancelling() const { return cancelling_; }
    // The diagnostic log's lines, for the error screen's tail of it.
    void set_diagnostics(std::vector<std::string> lines);
    // Confirms a Copy diagnostics on the error screen.
    void note_copied();

    // "Elapsed 12 s", as drawn.
    [[nodiscard]] static std::string elapsed_text(std::chrono::milliseconds elapsed);

    // The button the keyboard focus is on, and whether the ring is showing
    // (it is after Tab, not after a click).
    [[nodiscard]] Region::Kind focus() const { return focus_; }
    [[nodiscard]] bool focus_visible() const { return focus_visible_; }

private:
    static Action action_for(Region::Kind kind);
    static bool focusable(Region::Kind kind);
    void move_focus(int delta);
    [[nodiscard]] bool focus_present() const;
    void add_region(Region region);
    float draw_text(std::string_view text, int x, int baseline, Color color,
                    float size);
    void draw_button(const Region& region, std::string_view label, bool primary);
    // Wrap `text` to `max_width` px at `size`, drawing each line; returns the
    // baseline after the last line.
    int draw_wrapped(std::string_view text, int x, int baseline, int max_width,
                     Color color, float size);
    void render_progress();
    void render_error();
    void render_trust_prompt();
    // Draws a fingerprint, splitting the hex across two lines when one does
    // not fit; returns the baseline after it.
    int draw_fingerprint(std::string_view label, const Fingerprint& fingerprint,
                         int x, int baseline, int max_width, Color color);

    const ConnectFlow& flow_;
    std::string title_;
    TextEngine text_;
    Surface surface_;
    std::vector<Region> regions_;
    std::optional<BrokerCheck> prompt_;
    Action action_ = Action::none;
    std::chrono::milliseconds elapsed_ {0};
    bool cancelling_ = false;
    Region::Kind focus_ = Region::Kind::none;
    bool focus_visible_ = false;
    bool copied_ = false;
    std::vector<std::string> diagnostics_;
    int pointer_x_ = -1;
    int pointer_y_ = -1;
    bool dirty_ = true;
};

} // namespace haiku_remote
