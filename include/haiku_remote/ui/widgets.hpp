#pragma once

// A tiny hand-rolled immediate-mode widget set (NOT Dear ImGui), rendered in
// software into the project's own Surface so it reuses the tested FreeType
// TextEngine for all text and needs no bundled bitmap font. The SDL shell
// uploads the finished Surface as a single streaming texture each frame.
//
// Immediate mode: the view re-issues every widget each frame; a widget both
// draws itself and reports interaction (clicked / text changed). The only
// retained state is the focus ring (which text field has the caret), kept here,
// and each field's edit buffer, kept by the view in a TextFieldModel. The
// edit model itself is UI-free and unit-tested (text_field_model.hpp); this
// layer owns only drawing and the keymap.
//
// This unit is the SDL/GUI side of the architecture split: it is compiled only
// into haiku-remote-gui, never into haiku_remote_core.

#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"
#include "haiku_remote/text_field_model.hpp"
#include "haiku_remote/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote::ui {

struct Theme {
    Color background{24, 26, 31, 255};
    Color panel{34, 37, 44, 255};
    Color panel_alt{41, 45, 54, 255};
    Color selected{46, 60, 82, 255};
    Color accent{64, 132, 214, 255};
    Color accent_text{255, 255, 255, 255};
    Color text{226, 229, 235, 255};
    Color text_dim{150, 156, 168, 255};
    Color danger{206, 76, 76, 255};
    Color ok{104, 180, 120, 255};
    Color field{18, 20, 24, 255};
    Color field_focus{30, 46, 66, 255};
    Color border{64, 70, 82, 255};
    int font_size = 14;
    int pad = 8;
};

// Everything the shell gathered from SDL this frame, in logical (window)
// coordinates. The shell fills this; the widgets only read it.
struct Input {
    int mouse_x = 0;
    int mouse_y = 0;
    bool mouse_down = false; // left button currently held
    bool clicked = false;    // left button released this frame (a click)
    int wheel = 0;           // vertical wheel ticks this frame (down negative)
    std::string text;        // SDL_TEXTINPUT accumulated this frame
    // Editing keys for the focused field, set from SDL_KEYDOWN this frame.
    bool backspace = false;
    bool del = false;
    bool left = false;
    bool right = false;
    bool home = false;
    bool end = false;
    bool tab = false;
    bool back_tab = false;
    bool enter = false;
    bool escape = false;
};

// A rectangle in pixels, right/bottom exclusive (easier to reason about than
// Haiku's inclusive Rect for layout); converted at the draw boundary.
struct Box {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    [[nodiscard]] bool contains(int px, int py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

class Widgets {
public:
    explicit Widgets(Theme theme = {});

    void begin(Surface& surface, const Input& input);
    void end();

    [[nodiscard]] const Theme& theme() const { return theme_; }
    [[nodiscard]] const Input& input() const { return input_; }

    // Drawing primitives.
    void fill(Box box, Color color);
    void outline(Box box, Color color);
    void label(int x, int baseline_y, std::string_view text, Color color);
    void label(int x, int baseline_y, std::string_view text)
    {
        label(x, baseline_y, text, theme_.text);
    }
    // Draw text clipped to `max_width`, appending an ellipsis when it overflows.
    void label_clipped(int x, int baseline_y, std::string_view text, Color color,
                       int max_width);

    [[nodiscard]] int measure(std::string_view text);
    [[nodiscard]] int line_height() const { return theme_.font_size + 6; }
    // The baseline y that vertically centres one line of text in `box`.
    [[nodiscard]] int center_baseline(Box box) const;

    // Widgets. Each returns an interaction result for this frame.
    [[nodiscard]] bool button(Box box, std::string_view label, bool enabled = true);
    [[nodiscard]] bool button_accent(Box box, std::string_view label,
                                     bool enabled = true);
    [[nodiscard]] bool button_danger(Box box, std::string_view label,
                                     bool enabled = true);
    // A clickable row (library card background / list entry). Draws the selected
    // highlight when `selected`. Returns true when clicked.
    [[nodiscard]] bool row(Box box, bool selected);
    // A focusable, editable single-line field bound to `model`. `id` must be a
    // stable, nonzero, per-frame-unique integer. Returns true if the text
    // changed this frame.
    bool field(int id, Box box, TextFieldModel& model,
               std::string_view placeholder = {});
    // A two-state toggle (favorite star, mode switch, ...). Returns true when
    // toggled this frame.
    [[nodiscard]] bool toggle(Box box, std::string_view label, bool on);

    void set_focus(int id) { focus_ = id; }
    [[nodiscard]] int focused() const { return focus_; }

private:
    [[nodiscard]] bool click_in(Box box) const;
    void draw_button(Box box, std::string_view label, Color face, Color text_color,
                     bool enabled);

    Theme theme_;
    TextEngine text_;
    Surface* surface_ = nullptr;
    Input input_{};
    int focus_ = 0;
    // Focus-ring bookkeeping for this frame.
    std::vector<int> field_order_;
};

} // namespace haiku_remote::ui
