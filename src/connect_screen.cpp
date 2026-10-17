#include "haiku_remote/connect_screen.hpp"

#include "haiku_remote/ui_theme.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace haiku_remote {

namespace {

// The shared chrome palette (ui_theme.hpp), so this screen and the library
// read as one app.
constexpr Color bg = ui_theme::background;
constexpr Color panel = ui_theme::panel;
constexpr Color border = ui_theme::border;
constexpr Color accent = ui_theme::accent;
constexpr Color text_primary = ui_theme::text_primary;
constexpr Color text_muted = ui_theme::text_muted;
constexpr Color danger = ui_theme::danger;
constexpr Color on_accent = ui_theme::on_accent;
// The "done" marker. The theme has no success colour; this green keeps 4.9:1
// against the background (WCAG relative luminance, by hand), and state is
// carried by the marker's shape as well, never by colour alone.
constexpr Color good {46, 125, 50, 255};

constexpr int margin = 32;
// Focus ring, as the library draws it: 2 px wide, 1 px clear of the control.
constexpr int ring_offset = 3;
constexpr int ring_thickness = 2;

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
    draw_text(label, tx, baseline, primary ? on_accent : text_primary, 13);
}

void ConnectScreen::pointer_move(int x, int y)
{
    pointer_x_ = x;
    pointer_y_ = y;
}

ConnectScreen::Action ConnectScreen::action_for(Region::Kind kind)
{
    switch (kind) {
    case Region::Kind::back:
        return Action::back;
    case Region::Kind::retry:
        return Action::retry;
    case Region::Kind::trust:
        return Action::trust;
    case Region::Kind::replace:
        return Action::replace;
    case Region::Kind::dont_trust:
        return Action::reject;
    case Region::Kind::cancel:
        return Action::cancel;
    case Region::Kind::copy_log:
        return Action::copy_log;
    case Region::Kind::none:
        break;
    }
    return Action::none;
}

bool ConnectScreen::focusable(Region::Kind kind)
{
    // The trust prompt's buttons are deliberately not: accepting a
    // certificate takes a click on the button that says so, never keys.
    return kind == Region::Kind::cancel || kind == Region::Kind::back
        || kind == Region::Kind::retry || kind == Region::Kind::copy_log;
}

void ConnectScreen::pointer_press(int x, int y)
{
    pointer_x_ = x;
    pointer_y_ = y;
    for (const Region& region : regions_) {
        if (!region.hit(x, y))
            continue;
        action_ = action_for(region.kind);
        if (focusable(region.kind))
            focus_ = region.kind;
        focus_visible_ = false; // the ring is for keyboard use
        dirty_ = true;
        return;
    }
}

void ConnectScreen::move_focus(int delta)
{
    std::vector<Region::Kind> order;
    for (const Region& region : regions_)
        if (focusable(region.kind))
            order.push_back(region.kind);
    if (order.empty())
        return;
    const auto it = std::find(order.begin(), order.end(), focus_);
    int index = 0;
    if (it == order.end())
        index = delta > 0 ? 0 : static_cast<int>(order.size()) - 1;
    else
        index = (static_cast<int>(it - order.begin()) + delta
                 + static_cast<int>(order.size()))
                % static_cast<int>(order.size());
    focus_ = order[static_cast<std::size_t>(index)];
    focus_visible_ = true;
    dirty_ = true;
}

bool ConnectScreen::focus_present() const
{
    for (const Region& region : regions_)
        if (region.kind == focus_ && focusable(region.kind))
            return true;
    return false;
}

void ConnectScreen::key(Key k)
{
    if (prompt_) {
        // Both keys refuse. Trust and Replace are reachable only by clicking
        // the button that names them -- never by habitually pressing Enter.
        if (k == Key::escape || k == Key::enter) {
            action_ = Action::reject;
            dirty_ = true;
        }
        return;
    }
    if (k == Key::tab || k == Key::back_tab) {
        move_focus(k == Key::tab ? 1 : -1);
        return;
    }
    // Enter on a focused button presses it.
    if (k == Key::enter && focus_visible_ && focus_present()) {
        action_ = action_for(focus_);
        dirty_ = true;
        return;
    }
    if (flow_.state() != FlowState::failed) {
        // While connecting, Escape is Cancel (once); Enter does nothing.
        if (k == Key::escape && !cancelling_) {
            action_ = Action::cancel;
            dirty_ = true;
        }
        return;
    }
    switch (k) {
    case Key::escape:
        action_ = Action::back;
        dirty_ = true;
        break;
    case Key::enter:
        action_ = flow_.error().can_retry ? Action::retry : Action::back;
        dirty_ = true;
        break;
    case Key::tab:
    case Key::back_tab:
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

std::string ConnectScreen::elapsed_text(std::chrono::milliseconds elapsed)
{
    const long long seconds = elapsed.count() / 1000;
    if (seconds < 60)
        return "Elapsed " + std::to_string(seconds) + " s";
    const long long rest = seconds % 60;
    return "Elapsed " + std::to_string(seconds / 60) + " min "
           + (rest < 10 ? "0" : "") + std::to_string(rest) + " s";
}

void ConnectScreen::set_elapsed(std::chrono::milliseconds elapsed)
{
    if (elapsed.count() / 1000 != elapsed_.count() / 1000)
        dirty_ = true;
    elapsed_ = elapsed;
}

void ConnectScreen::set_cancelling(bool cancelling)
{
    if (cancelling != cancelling_)
        dirty_ = true;
    cancelling_ = cancelling;
}

void ConnectScreen::set_diagnostics(std::vector<std::string> lines)
{
    if (lines != diagnostics_)
        dirty_ = true;
    diagnostics_ = std::move(lines);
}

void ConnectScreen::note_copied()
{
    copied_ = true;
    dirty_ = true;
}

void ConnectScreen::show_trust_prompt(const BrokerCheck& check)
{
    prompt_ = check;
    action_ = Action::none;
    dirty_ = true;
}

void ConnectScreen::clear_trust_prompt()
{
    prompt_.reset();
    action_ = Action::none;
    dirty_ = true;
}

const Surface& ConnectScreen::render()
{
    regions_.clear();
    surface_.clear(bg);
    if (prompt_)
        render_trust_prompt();
    else if (flow_.state() == FlowState::failed)
        render_error();
    else
        render_progress();
    if (focus_visible_ && !prompt_) {
        for (const Region& region : regions_) {
            if (region.kind != focus_ || !focusable(region.kind))
                continue;
            for (int i = 0; i < ring_thickness; ++i) {
                const int off = ring_offset - i;
                frame(surface_, region.x - off, region.y - off,
                      region.w + 2 * off, region.h + 2 * off, ui_theme::focus_ring);
            }
        }
    }
    dirty_ = false;
    return surface_;
}

void ConnectScreen::render_progress()
{
    const int content_w = surface_.width() - 2 * margin;

    draw_text(title_.empty() ? "Connecting\xE2\x80\xA6"
                             : "Connecting to " + title_,
              margin, 52, text_primary, 24);
    if (!flow_.plan_note().empty())
        draw_wrapped(flow_.plan_note(), margin, 78, content_w, text_muted, 12);

    // Ten rows for an auto-mode plan (four broker, six tunnel): kept compact
    // so the whole checklist, the fallback note and the Cancel row fit the
    // 560 px window.
    int y = 128;
    const int row_h = 26;
    const auto active = flow_.active_stage();
    std::size_t last_step = flow_.stages().empty()
                                ? 0
                                : flow_.stages().front().step_index;
    for (std::size_t i = 0; i < flow_.stages().size(); ++i) {
        const ProgressStage& stage = flow_.stages()[i];
        if (stage.step_index != last_step) {
            y += 8; // a gap between one route's run and the next
            last_step = stage.step_index;
        }
        draw_marker(surface_, margin, y, stage.state);
        const bool is_active = active && *active == i;
        const Color label_color = stage.state == StageState::pending
                                          || stage.state == StageState::skipped
                                      ? text_muted
                                      : (is_active ? accent : text_primary);
        draw_text(stage.label, margin + 28, y, label_color, 14);
        y += row_h;
    }

    if (flow_.fell_back()) {
        draw_wrapped("The broker was not available \xE2\x80\x94 falling back to"
                     " the SSH tunnel.",
                     margin, y + 6, content_w, text_muted, 12);
    }

    // The bottom row: Cancel (or the teardown in progress) and the clock.
    const int by = surface_.height() - margin - 36;
    if (cancelling_) {
        draw_text("Cancelling\xE2\x80\xA6 stopping ssh and closing the"
                  " connection.",
                  margin, by + 23, text_muted, 13);
    } else {
        Region cancel {Region::Kind::cancel, margin, by, 120, 36};
        add_region(cancel);
        draw_button(cancel, "Cancel", false);
    }
    const std::string clock = elapsed_text(elapsed_);
    DrawState measure;
    measure.font.size = 13;
    const int clock_w = static_cast<int>(text_.width(clock, measure.font));
    draw_text(clock, surface_.width() - margin - clock_w, by + 23, text_muted, 13);
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
        y = draw_wrapped(e.raw, margin, y, content_w, text_muted, 11) + 6;
    }

    const int by = surface_.height() - margin - 36;

    // The newest diagnostic lines that fit between the details and the
    // buttons; the whole log is one click away (Copy diagnostics).
    if (!diagnostics_.empty()) {
        constexpr int line_h = 14;
        const int top = y + 4;
        const int bottom = by - 10;
        const int fit = (bottom - top - 18) / line_h;
        if (fit > 0) {
            fill(surface_, margin - 8, top, content_w + 16, bottom - top, panel);
            draw_text("Diagnostic log (newest last):", margin, top + 14,
                      text_muted, 11);
            const std::size_t count = std::min<std::size_t>(
                diagnostics_.size(), static_cast<std::size_t>(fit));
            int ly = top + 14 + line_h;
            for (std::size_t i = diagnostics_.size() - count;
                 i < diagnostics_.size(); ++i) {
                draw_text(diagnostics_[i], margin, ly, text_muted, 10);
                ly += line_h;
            }
        }
    }

    // Affordances, bottom-left: Back always, Retry when it could help, and
    // Copy diagnostics whenever there is a log to copy.
    Region back {Region::Kind::back, margin, by, 160, 36};
    add_region(back);
    draw_button(back, "\xE2\x86\x90 Back to library", !e.can_retry);
    int bx = margin + 172;
    if (e.can_retry) {
        Region retry {Region::Kind::retry, bx, by, 120, 36};
        add_region(retry);
        draw_button(retry, "Retry", true);
        bx += 132;
    }
    if (!diagnostics_.empty()) {
        Region copy {Region::Kind::copy_log, bx, by, 170, 36};
        add_region(copy);
        draw_button(copy, copied_ ? "Copied" : "Copy diagnostics", false);
    }
}

int ConnectScreen::draw_fingerprint(std::string_view label,
                                    const Fingerprint& fingerprint, int x,
                                    int baseline, int max_width, Color color)
{
    constexpr float size = 13;
    const int line_height = static_cast<int>(size * 1.4f) + 2;
    draw_text(label, x, baseline, text_muted, 12);
    baseline += line_height;
    const std::string full = display_fingerprint(fingerprint);
    DrawState measure;
    measure.font.size = size;
    if (text_.width(full, measure.font) <= static_cast<float>(max_width)) {
        draw_text(full, x, baseline, color, size);
        return baseline + line_height;
    }
    // "SHA256:" + 64 hex: break the hex in half so it stays comparable.
    const std::size_t split = 7 + 32;
    draw_text(full.substr(0, split), x, baseline, color, size);
    baseline += line_height;
    draw_text(full.substr(split), x, baseline, color, size);
    return baseline + line_height;
}

void ConnectScreen::render_trust_prompt()
{
    const BrokerCheck& check = *prompt_;
    const int content_w = surface_.width() - 2 * margin;
    const int by = surface_.height() - margin - 36;

    if (check.state == BrokerTrust::changed) {
        int y = draw_wrapped("WARNING: BROKER IDENTIFICATION HAS CHANGED!", margin,
                             52, content_w, danger, 22)
                + 2;
        y = draw_wrapped("Someone could be eavesdropping on you right now"
                         " (man-in-the-middle attack)! It is also possible that"
                         " the broker's certificate was just regenerated.",
                         margin, y, content_w, text_primary, 14)
            + 6;
        draw_text("Broker: " + check.key(), margin, y, text_primary, 14);
        y += 26;
        if (check.stored)
            y = draw_fingerprint("Stored fingerprint:", *check.stored, margin, y,
                                 content_w, text_primary);
        y = draw_fingerprint("Presented fingerprint:", check.presented, margin, y,
                             content_w, danger);
        if (check.vouched)
            y = draw_wrapped("The presented certificate matches the one fetched"
                             " over SSH from the host, which is consistent with a"
                             " regenerated certificate.",
                             margin, y + 4, content_w, text_muted, 12);
        draw_wrapped("Check broker.fingerprint on the host before replacing.",
                     margin, y + 4, content_w, text_muted, 12);

        Region cancel {Region::Kind::dont_trust, margin, by, 120, 36};
        add_region(cancel);
        draw_button(cancel, "Cancel", true);
        // Deliberately not primary-styled, and never bound to a key.
        // Beside Cancel when it fits, else on its own row above it.
        Region replace {Region::Kind::replace, margin + 132, by, 360, 36};
        if (replace.x + replace.w > surface_.width() - margin)
            replace = Region {Region::Kind::replace, margin, by - 46, content_w, 36};
        add_region(replace);
        fill(surface_, replace.x, replace.y, replace.w, replace.h, panel);
        frame(surface_, replace.x, replace.y, replace.w, replace.h, danger);
        draw_text("Replace the stored fingerprint and connect", replace.x + 14,
                  replace.y + replace.h * 2 / 3 + 2, danger, 13);
        return;
    }

    draw_text("Unknown broker certificate", margin, 56, text_primary, 24);
    int y = 96;
    y = draw_wrapped("The authenticity of this broker can't be established.",
                     margin, y, content_w, text_primary, 15)
        + 6;
    draw_text("Broker: " + check.key(), margin, y, text_primary, 14);
    y += 28;
    y = draw_fingerprint("Certificate fingerprint:", check.presented, margin, y,
                         content_w, accent);
    if (check.vouched)
        y = draw_wrapped("This matches the certificate fetched over SSH from the"
                         " host.",
                         margin, y, content_w, good, 12)
            + 4;
    y = draw_wrapped("Compare it with broker.fingerprint on the host. Trusting"
                     " records it in " + check.store_file.string()
                     + "; a different certificate later will be refused with a"
                       " warning.",
                     margin, y + 4, content_w, text_muted, 12);
    draw_text("Trust this certificate?", margin, y + 14, text_primary, 16);

    Region no {Region::Kind::dont_trust, margin, by, 140, 36};
    add_region(no);
    draw_button(no, "Don't trust", false);
    Region yes {Region::Kind::trust, margin + 152, by, 180, 36};
    add_region(yes);
    draw_button(yes, "Trust and connect", true);
}

} // namespace haiku_remote
