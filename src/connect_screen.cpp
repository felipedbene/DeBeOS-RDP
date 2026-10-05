#include "haiku_remote/connect_screen.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace haiku_remote {

namespace {

// Same dark palette as LibraryScreen, so the two screens read as one app.
constexpr Color bg {24, 26, 32, 255};
constexpr Color panel {36, 39, 48, 255};
constexpr Color border {70, 76, 92, 255};
constexpr Color accent {74, 144, 226, 255};
constexpr Color good {120, 196, 128, 255};
constexpr Color text_primary {234, 237, 242, 255};
constexpr Color text_muted {150, 156, 170, 255};
constexpr Color danger {214, 92, 92, 255};
constexpr Color white {255, 255, 255, 255};

constexpr int margin = 32;

void fill(Surface& surface, int x, int y, int w, int h, Color color)
{
    if (w <= 0 || h <= 0)
        return;
    surface.fill_rect_color(Rect {static_cast<float>(x), static_cast<float>(y),
                                  static_cast<float>(x + w - 1),
                                  static_cast<float>(y + h - 1)},
                            color);
}

void frame(Surface& surface, int x, int y, int w, int h, Color color)
{
    if (w <= 0 || h <= 0)
        return;
    surface.stroke_rect(Rect {static_cast<float>(x), static_cast<float>(y),
                              static_cast<float>(x + w - 1),
                              static_cast<float>(y + h - 1)},
                        color);
}

// A status marker drawn with Surface primitives rather than a glyph: the font
// on a given host may not carry a check mark or a filled circle (it did not on
// the test host), and a blank marker is worse than a crude one. State is
// carried by both shape and colour: a filled green disc for done, a filled
// accent disc for the active step, a filled red disc for a failure, a hollow
// muted ring for a step not yet reached, and a short muted bar for a skipped
// one.
void draw_marker(Surface& surface, int x, int baseline, StageState state)
{
    constexpr int d = 12;
    const int top = baseline - d; // sits just above the text baseline.
    const Rect r {static_cast<float>(x), static_cast<float>(top),
                  static_cast<float>(x + d - 1), static_cast<float>(top + d - 1)};
    DrawState st;
    switch (state) {
    case StageState::done:
        st.high = good;
        surface.fill_ellipse(r, st);
        break;
    case StageState::active:
        st.high = accent;
        surface.fill_ellipse(r, st);
        break;
    case StageState::failed:
        st.high = danger;
        surface.fill_ellipse(r, st);
        break;
    case StageState::skipped:
        fill(surface, x, baseline - d / 2 - 1, d, 2, text_muted);
        break;
    case StageState::pending:
        st.high = text_muted;
        surface.stroke_ellipse(r, st);
        break;
    }
}

} // namespace

ConnectScreen::ConnectScreen(const ConnectFlow& flow, std::string title,
                             int width, int height)
    : flow_(flow), title_(std::move(title)),
      surface_(std::max(width, 480), std::max(height, 320))
{
}

void ConnectScreen::resize(int width, int height)
{
    width = std::max(width, 480);
    height = std::max(height, 320);
    if (width == surface_.width() && height == surface_.height())
        return;
    surface_ = Surface(width, height);
    dirty_ = true;
}

void ConnectScreen::add_region(Region region)
{
    regions_.push_back(region);
}

float ConnectScreen::draw_text(std::string_view text, int x, int baseline,
                               Color color, float size)
{
    if (text.empty())
        return 0;
    DrawState state;
    state.high = color;
    state.font.size = size;
    return text_.draw(text, Point {static_cast<float>(x),
                                   static_cast<float>(baseline)},
                      state, surface_);
}

int ConnectScreen::draw_wrapped(std::string_view text, int x, int baseline,
                                int max_width, Color color, float size)
{
    DrawState measure;
    measure.font.size = size;
    const int line_height = static_cast<int>(size * 1.4f) + 2;

    std::string line;
    std::size_t i = 0;
    const auto flush = [&]() {
        if (!line.empty()) {
            draw_text(line, x, baseline, color, size);
            baseline += line_height;
            line.clear();
        }
    };
    while (i < text.size()) {
        // Pull the next whitespace-delimited word.
        while (i < text.size() && text[i] == ' ')
            ++i;
        std::size_t begin = i;
        while (i < text.size() && text[i] != ' ')
            ++i;
        if (begin == i)
            break;
        const std::string word(text.substr(begin, i - begin));
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty()
            && text_.width(candidate, measure.font) > static_cast<float>(max_width)) {
            flush();
            line = word;
        } else {
            line = candidate;
        }
    }
    flush();
    return baseline;
}

void ConnectScreen::draw_button(const Region& region, std::string_view label,
                                bool primary)
{
    fill(surface_, region.x, region.y, region.w, region.h,
         primary ? accent : panel);
    frame(surface_, region.x, region.y, region.w, region.h,
          primary ? accent : border);
    DrawState measure;
    measure.font.size = 13;
    const float advance = text_.width(label, measure.font);
    const int tx =
        region.x + std::max(8, (region.w - static_cast<int>(advance)) / 2);
    const int baseline = region.y + region.h * 2 / 3 + 2;
    draw_text(label, tx, baseline, primary ? white : text_primary, 13);
}

void ConnectScreen::pointer_move(int x, int y)
{
    pointer_x_ = x;
    pointer_y_ = y;
}

void ConnectScreen::pointer_press(int x, int y)
{
    pointer_x_ = x;
    pointer_y_ = y;
    for (const Region& region : regions_) {
        if (!region.hit(x, y))
            continue;
        if (region.kind == Region::Kind::back)
            action_ = Action::back;
        else if (region.kind == Region::Kind::retry)
            action_ = Action::retry;
        dirty_ = true;
        return;
    }
}

void ConnectScreen::key(Key k)
{
    if (flow_.state() != FlowState::failed)
        return; // no actions while still connecting.
    switch (k) {
    case Key::escape:
        action_ = Action::back;
        dirty_ = true;
        break;
    case Key::enter:
        action_ = flow_.error().can_retry ? Action::retry : Action::back;
        dirty_ = true;
        break;
    case Key::none:
        break;
    }
}

ConnectScreen::Action ConnectScreen::take_action()
{
    const Action action = action_;
    action_ = Action::none;
    return action;
}

const Surface& ConnectScreen::render()
{
    regions_.clear();
    surface_.clear(bg);
    if (flow_.state() == FlowState::failed)
        render_error();
    else
        render_progress();
    dirty_ = false;
    return surface_;
}

void ConnectScreen::render_progress()
{
    const int content_w = surface_.width() - 2 * margin;

    draw_text(title_.empty() ? "Connecting\xE2\x80\xA6"
                             : "Connecting to " + title_,
              margin, 56, text_primary, 24);
    if (!flow_.plan_note().empty())
        draw_wrapped(flow_.plan_note(), margin, 84, content_w, text_muted, 12);

    int y = 140;
    const int row_h = 34;
    const auto active = flow_.active_stage();
    for (std::size_t i = 0; i < flow_.stages().size(); ++i) {
        const ProgressStage& stage = flow_.stages()[i];
        draw_marker(surface_, margin, y, stage.state);
        const bool is_active = active && *active == i;
        const Color label_color = stage.state == StageState::pending
                                          || stage.state == StageState::skipped
                                      ? text_muted
                                      : (is_active ? white : text_primary);
        draw_text(stage.label, margin + 28, y, label_color, 15);
        y += row_h;
    }

    if (flow_.fell_back()) {
        draw_wrapped("The broker was not available \xE2\x80\x94 falling back to"
                     " the SSH tunnel.",
                     margin, y + 12, content_w, text_muted, 12);
    }

    // A plain, honest footer: the connect runs on this thread, so a long step
    // (an SSH connect) holds here with its label shown rather than animating.
    draw_text("Working\xE2\x80\xA6 this can take a few seconds.", margin,
              surface_.height() - margin, text_muted, 12);
}

void ConnectScreen::render_error()
{
    const int content_w = surface_.width() - 2 * margin;
    const ConnectError& e = flow_.error();

    draw_text("Could not connect", margin, 56, danger, 24);
    if (!title_.empty())
        draw_text("to " + title_, margin, 82, text_muted, 14);

    int y = 128;
    draw_text(e.title, margin, y, text_primary, 18);
    y += 30;
    y = draw_wrapped(e.detail, margin, y, content_w, text_primary, 14) + 10;
    if (!e.remedy.empty()) {
        draw_text("What to try:", margin, y, text_muted, 12);
        y += 22;
        y = draw_wrapped(e.remedy, margin, y, content_w, accent, 14) + 12;
    }
    if (!e.raw.empty()) {
        draw_text("Details:", margin, y, text_muted, 12);
        y += 20;
        draw_wrapped(e.raw, margin, y, content_w, text_muted, 11);
    }

    // Affordances, bottom-left: Back always, Retry when it could help.
    const int by = surface_.height() - margin - 36;
    Region back {Region::Kind::back, margin, by, 160, 36};
    add_region(back);
    draw_button(back, "\xE2\x86\x90 Back to library", !e.can_retry);
    if (e.can_retry) {
        Region retry {Region::Kind::retry, margin + 172, by, 120, 36};
        add_region(retry);
        draw_button(retry, "Retry", true);
    }
}

} // namespace haiku_remote
