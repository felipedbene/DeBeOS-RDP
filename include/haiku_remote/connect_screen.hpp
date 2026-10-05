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
// It reads a ConnectFlow by reference -- the flow is advanced by
// connect_with_progress() on the same thread -- and renders whatever state the
// flow is in: the stage checklist while connecting, or the error with its
// suggested fix and Back / Retry affordances once the flow has failed. It opens
// no socket and runs no process; the dependency arrow is one way, frontend ->
// screen -> flow.

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/known_brokers.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

class ConnectScreen {
public:
    // trust / replace / reject are only ever produced while a trust prompt is
    // showing (show_trust_prompt); back / retry only on the error screen.
    enum class Action { none, back, retry, trust, replace, reject };

    enum class Key { none, enter, escape };

    struct Region {
        enum class Kind { none, back, retry, trust, dont_trust, replace };
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

private:
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
    int pointer_x_ = -1;
    int pointer_y_ = -1;
    bool dirty_ = true;
};

} // namespace haiku_remote
