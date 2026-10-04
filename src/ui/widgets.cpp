#include "haiku_remote/ui/widgets.hpp"

#include <algorithm>

namespace haiku_remote::ui {

namespace {

Rect to_rect(Box box)
{
    // Box is right/bottom exclusive; Haiku's Rect is inclusive.
    return Rect{static_cast<float>(box.x), static_cast<float>(box.y),
                static_cast<float>(box.x + box.w - 1),
                static_cast<float>(box.y + box.h - 1)};
}

// Longest prefix of `text` (on a UTF-8 boundary) whose width plus an ellipsis
// fits in `max_width`. Returns the whole string when it already fits.
std::string clip_to_width(TextEngine& engine, const Font& font,
                          std::string_view text, int max_width)
{
    if (engine.width(text, font) <= static_cast<float>(max_width))
        return std::string(text);
    std::string ellipsis = "...";
    std::size_t len = text.size();
    while (len > 0) {
        // Step back to a code-point boundary.
        --len;
        while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xc0) == 0x80)
            --len;
        std::string candidate(text.substr(0, len));
        candidate += ellipsis;
        if (engine.width(candidate, font) <= static_cast<float>(max_width))
            return candidate;
    }
    return ellipsis;
}

} // namespace

Widgets::Widgets(Theme theme) : theme_(theme) {}

void Widgets::begin(Surface& surface, const Input& input)
{
    surface_ = &surface;
    input_ = input;
    field_order_.clear();
    surface_->clear(theme_.background);
}

void Widgets::end()
{
    if (field_order_.empty())
        return;
    const auto it = std::find(field_order_.begin(), field_order_.end(), focus_);
    const auto count = field_order_.size();
    if (input_.tab) {
        if (it == field_order_.end())
            focus_ = field_order_.front();
        else
            focus_ = field_order_[(static_cast<std::size_t>(it - field_order_.begin())
                                   + 1)
                                  % count];
    } else if (input_.back_tab) {
        if (it == field_order_.end())
            focus_ = field_order_.back();
        else
            focus_ = field_order_[(static_cast<std::size_t>(it - field_order_.begin())
                                   + count - 1)
                                  % count];
    }
}

void Widgets::fill(Box box, Color color)
{
    if (surface_ == nullptr || box.w <= 0 || box.h <= 0)
        return;
    surface_->fill_rect_color(to_rect(box), color);
}

void Widgets::outline(Box box, Color color)
{
    if (surface_ == nullptr || box.w <= 0 || box.h <= 0)
        return;
    surface_->stroke_rect(to_rect(box), color);
}

void Widgets::label(int x, int baseline_y, std::string_view text, Color color)
{
    if (surface_ == nullptr || text.empty())
        return;
    DrawState state;
    state.high = color;
    state.drawing_mode = DrawingMode::over;
    state.font.size = static_cast<float>(theme_.font_size);
    text_.draw(text, Point{static_cast<float>(x), static_cast<float>(baseline_y)},
               state, *surface_);
}

void Widgets::label_clipped(int x, int baseline_y, std::string_view text,
                            Color color, int max_width)
{
    if (surface_ == nullptr || text.empty() || max_width <= 0)
        return;
    Font font;
    font.size = static_cast<float>(theme_.font_size);
    label(x, baseline_y, clip_to_width(text_, font, text, max_width), color);
}

int Widgets::measure(std::string_view text)
{
    Font font;
    font.size = static_cast<float>(theme_.font_size);
    return static_cast<int>(text_.width(text, font) + 0.5f);
}

int Widgets::center_baseline(Box box) const
{
    return box.y + (box.h + theme_.font_size) / 2 - 1;
}

bool Widgets::click_in(Box box) const
{
    return input_.clicked && box.contains(input_.mouse_x, input_.mouse_y);
}

void Widgets::draw_button(Box box, std::string_view label_text, Color face,
                          Color text_color, bool enabled)
{
    const bool hover =
        enabled && box.contains(input_.mouse_x, input_.mouse_y);
    Color drawn = face;
    if (!enabled) {
        drawn = theme_.panel_alt;
        text_color = theme_.text_dim;
    } else if (hover && input_.mouse_down) {
        drawn = {static_cast<std::uint8_t>(face.r * 3 / 4),
                 static_cast<std::uint8_t>(face.g * 3 / 4),
                 static_cast<std::uint8_t>(face.b * 3 / 4), face.a};
    }
    fill(box, drawn);
    outline(box, theme_.border);
    const int tw = measure(label_text);
    label(box.x + (box.w - tw) / 2, center_baseline(box), label_text, text_color);
}

bool Widgets::button(Box box, std::string_view label_text, bool enabled)
{
    draw_button(box, label_text, theme_.panel_alt, theme_.text, enabled);
    return enabled && click_in(box);
}

bool Widgets::button_accent(Box box, std::string_view label_text, bool enabled)
{
    draw_button(box, label_text, theme_.accent, theme_.accent_text, enabled);
    return enabled && click_in(box);
}

bool Widgets::button_danger(Box box, std::string_view label_text, bool enabled)
{
    draw_button(box, label_text, theme_.danger, theme_.accent_text, enabled);
    return enabled && click_in(box);
}

bool Widgets::row(Box box, bool selected)
{
    const bool hover = box.contains(input_.mouse_x, input_.mouse_y);
    Color face = theme_.panel;
    if (selected)
        face = theme_.selected;
    else if (hover)
        face = theme_.panel_alt;
    fill(box, face);
    outline(box, theme_.border);
    return click_in(box);
}

bool Widgets::field(int id, Box box, TextFieldModel& model,
                    std::string_view placeholder)
{
    field_order_.push_back(id);
    if (click_in(box))
        focus_ = id;
    const bool active = (focus_ == id);

    fill(box, active ? theme_.field_focus : theme_.field);
    outline(box, active ? theme_.accent : theme_.border);

    bool changed = false;
    if (active) {
        if (!input_.text.empty()) {
            model.insert(input_.text);
            changed = true;
        }
        if (input_.backspace) {
            model.backspace();
            changed = true;
        }
        if (input_.del) {
            model.del();
            changed = true;
        }
        if (input_.left)
            model.move_left();
        if (input_.right)
            model.move_right();
        if (input_.home)
            model.move_home();
        if (input_.end)
            model.move_end();
    }

    const int bx = box.x + theme_.pad;
    const int baseline = center_baseline(box);
    if (model.empty() && !placeholder.empty())
        label(bx, baseline, placeholder, theme_.text_dim);
    else
        label_clipped(bx, baseline, model.text(), theme_.text,
                      box.w - 2 * theme_.pad);

    if (active) {
        const std::string_view before(model.text().data(), model.cursor());
        const int caret_x = std::min(bx + measure(before), box.x + box.w - theme_.pad);
        fill(Box{caret_x, box.y + 4, 1, box.h - 8}, theme_.text);
    }
    return changed;
}

bool Widgets::toggle(Box box, std::string_view label_text, bool on)
{
    draw_button(box, label_text, on ? theme_.accent : theme_.panel_alt,
                on ? theme_.accent_text : theme_.text_dim, true);
    return click_in(box);
}

} // namespace haiku_remote::ui
