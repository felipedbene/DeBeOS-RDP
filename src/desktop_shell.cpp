#include "haiku_remote/desktop_shell.hpp"

#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/protocol.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace haiku_remote {

namespace {

// Same dark palette as LibraryScreen and ConnectScreen, so the bar reads as part
// of the same app.
constexpr Color panel {36, 39, 48, 255};
constexpr Color button_face {52, 56, 68, 255};
constexpr Color button_hot {74, 144, 226, 255};
constexpr Color border {70, 76, 92, 255};
constexpr Color good {120, 196, 128, 255};
constexpr Color accent {74, 144, 226, 255};
constexpr Color danger {214, 92, 92, 255};
constexpr Color text_primary {234, 237, 242, 255};
constexpr Color text_muted {150, 156, 170, 255};
constexpr Color stats_bg {24, 26, 32, 255};

constexpr int max_bar_width = 760;
constexpr int button_width = 80;
constexpr int button_height = 26;
constexpr int button_gap = 6;
constexpr int bar_margin = 8;
constexpr int stats_width = 300;
constexpr int stats_line = 15;

// Haiku's B_FUNCTION_KEY: what an F-key composes to (InterfaceDefs.h).
constexpr std::string_view function_key_bytes {"\x10", 1};

constexpr std::uint32_t command_held
    = modifiers::command | modifiers::left_command;

// Desktop.cpp:200-217 switches workspace on Command+F1..F12 with neither
// Control nor Option held; Desktop.cpp:219-224 goes to the previous workspace on
// Command+` (key 0x11). Key codes are Haiku's (B_F1_KEY = 0x02).
constexpr std::array<RemoteShortcut, 5> shortcuts {{
    {"Cmd+F1", command_held, 0x02, function_key_bytes, 0x10},
    {"Cmd+F2", command_held, 0x03, function_key_bytes, 0x10},
    {"Cmd+F3", command_held, 0x04, function_key_bytes, 0x10},
    {"Cmd+F4", command_held, 0x05, function_key_bytes, 0x10},
    {"Cmd+`", command_held, 0x11, "`", '`'},
}};

void fill(Surface& surface, int x, int y, int w, int h, Color color)
{
    if (w <= 0 || h <= 0)
        return;
    surface.fill_rect_color(Rect {static_cast<float>(x), static_cast<float>(y),
                                  static_cast<float>(x + w - 1),
                                  static_cast<float>(y + h - 1)},
                            color);
}

std::string_view button_label(DesktopShell::Region::Kind kind)
{
    using Kind = DesktopShell::Region::Kind;
    switch (kind) {
    case Kind::disconnect: return "Disconnect";
    case Kind::reconnect: return "Reconnect";
    case Kind::repaint: return "Repaint";
    case Kind::fullscreen: return "Full screen";
    case Kind::keys: return "Keys";
    case Kind::stats: return "Stats";
    case Kind::back: return "Back";
    case Kind::shortcut: return "";
    }
    return "";
}

} // namespace

std::span<const RemoteShortcut> remote_shortcuts()
{
    return shortcuts;
}

std::vector<std::vector<std::uint8_t>>
encode_shortcut(const RemoteShortcut& shortcut, std::uint32_t held_modifiers)
{
    return {
        InputEncoder::modifiers_changed(shortcut.modifiers),
        InputEncoder::key(true, shortcut.bytes, shortcut.raw_char, shortcut.key),
        InputEncoder::key(false, shortcut.bytes, shortcut.raw_char, shortcut.key),
        InputEncoder::modifiers_changed(held_modifiers),
    };
}

RepaintLever request_repaint(Session& session)
{
    if ((session.negotiated_capabilities() & cap_resync) != 0)
        return session.request_resync() ? RepaintLever::resync
                                        : RepaintLever::failed;
    return session.request_full_repaint() ? RepaintLever::display_mode
                                          : RepaintLever::failed;
}

ReconnectConfig reconnect_config_for(const ConnectionProfile& profile)
{
    ReconnectConfig config;
    config.enabled = profile.auto_reconnect;
    return config;
}

ConnectionOutcome outcome_for_connect_error(const ConnectError& error)
{
    switch (error.cause) {
    case ConnectErrorCause::missing_credential:
        return ConnectionOutcome::missing_credential;
    case ConnectErrorCause::broker_token_denied:
    case ConnectErrorCause::broker_cert_pin:
    case ConnectErrorCause::broker_cert_untrusted:
    case ConnectErrorCause::broker_cert_unknown:
    case ConnectErrorCause::broker_cert_changed:
    case ConnectErrorCause::session_refused:
        return ConnectionOutcome::refused;
    case ConnectErrorCause::none:
    case ConnectErrorCause::ssh_auth_or_host:
    case ConnectErrorCause::host_unreachable:
    case ConnectErrorCause::broker_unavailable:
    case ConnectErrorCause::cookie_unavailable:
    case ConnectErrorCause::server_listener:
    case ConnectErrorCause::other:
        return ConnectionOutcome::connect_failed;
    }
    return ConnectionOutcome::connect_failed;
}

DesktopShell::DesktopShell(std::string name, int desktop_width,
                           int desktop_height)
    : name_(std::move(name)),
      desktop_width_(std::max(desktop_width, 1)),
      desktop_height_(std::max(desktop_height, 1)),
      bar_(std::min(desktop_width_, max_bar_width), bar_height),
      stats_(std::min(desktop_width_, stats_width), stats_line)
{
    layout();
}

void DesktopShell::layout()
{
    regions_.clear();
    using Kind = Region::Kind;
    std::vector<Region> row;
    if (keys_mode_) {
        for (std::size_t i = 0; i < shortcuts.size(); ++i)
            row.push_back({Kind::shortcut, static_cast<int>(i)});
        row.push_back({Kind::back});
    } else {
        for (Kind kind : {Kind::disconnect, Kind::reconnect, Kind::repaint,
                          Kind::fullscreen, Kind::keys, Kind::stats})
            row.push_back({kind});
    }
    // Right-aligned, so the connection name keeps the left of the bar.
    int x = bar_.width() - bar_margin
        - static_cast<int>(row.size()) * (button_width + button_gap) + button_gap;
    for (Region region : row) {
        region.x = x;
        region.y = (bar_height - button_height) / 2;
        region.w = button_width;
        region.h = button_height;
        regions_.push_back(region);
        x += button_width + button_gap;
    }
}

void DesktopShell::set_link(Link link, std::string detail)
{
    if (link == link_ && detail == detail_)
        return;
    link_ = link;
    detail_ = std::move(detail);
    dirty_ = true;
}

IntRect DesktopShell::bar_rect() const
{
    const int left = (desktop_width_ - bar_.width()) / 2;
    return {left, 0, left + bar_.width() - 1, bar_height - 1};
}

IntRect DesktopShell::stats_rect() const
{
    const int height = std::min(desktop_height_, stats_.height());
    return {0, desktop_height_ - height, stats_.width() - 1, desktop_height_ - 1};
}

bool DesktopShell::visible() const
{
    return pinned_ || hovered_ || link_ != Link::connected;
}

bool DesktopShell::inside_bar(int x, int y) const
{
    const IntRect r = bar_rect();
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

bool DesktopShell::pointer_move(int x, int y, Clock::time_point now)
{
    const bool was_hot = pointer_x_ >= 0;
    if (visible()) {
        if (inside_bar(x, y)) {
            hovered_ = true;
            hide_at_.reset();
            pointer_x_ = x - bar_rect().left;
            pointer_y_ = y;
            dirty_ = true;
            return true;
        }
        if (hovered_ && !hide_at_)
            hide_at_ = now + hide_delay;
        pointer_x_ = pointer_y_ = -1;
        if (was_hot)
            dirty_ = true;
        return false;
    }
    const IntRect r = bar_rect();
    if (y >= 0 && y < reveal_band && x >= r.left && x <= r.right) {
        hovered_ = true;
        hide_at_.reset();
        pointer_x_ = x - r.left;
        pointer_y_ = y;
        dirty_ = true;
        return true;
    }
    return false;
}

bool DesktopShell::pointer_press(int x, int y, Clock::time_point now)
{
    (void)now;
    press_owned_ = false;
    if (!visible() || !inside_bar(x, y))
        return false;
    press_owned_ = true;
    const int bx = x - bar_rect().left;
    for (const Region& region : regions_) {
        if (!region.hit(bx, y))
            continue;
        using Kind = Region::Kind;
        switch (region.kind) {
        case Kind::disconnect: action_ = Action::disconnect; break;
        case Kind::reconnect: action_ = Action::reconnect; break;
        case Kind::repaint: action_ = Action::repaint; break;
        case Kind::fullscreen: action_ = Action::fullscreen; break;
        case Kind::stats:
            stats_visible_ = !stats_visible_;
            action_ = Action::stats;
            break;
        case Kind::keys:
            keys_mode_ = true;
            layout();
            break;
        case Kind::back:
            keys_mode_ = false;
            layout();
            break;
        case Kind::shortcut:
            shortcut_ = &shortcuts[static_cast<std::size_t>(region.index)];
            action_ = Action::shortcut;
            break;
        }
        dirty_ = true;
        break;
    }
    return true;
}

bool DesktopShell::pointer_release()
{
    const bool owned = press_owned_;
    press_owned_ = false;
    return owned;
}

void DesktopShell::toggle()
{
    pinned_ = !pinned_;
    if (!pinned_) {
        hovered_ = false;
        hide_at_.reset();
        keys_mode_ = false;
        layout();
    }
    dirty_ = true;
}

void DesktopShell::tick(Clock::time_point now)
{
    if (hide_at_ && now >= *hide_at_) {
        hovered_ = false;
        hide_at_.reset();
        if (!pinned_ && keys_mode_) {
            keys_mode_ = false;
            layout();
        }
        dirty_ = true;
    }
}

void DesktopShell::set_stats_visible(bool shown)
{
    if (stats_visible_ != shown)
        dirty_ = true;
    stats_visible_ = shown;
}

void DesktopShell::set_stats_lines(std::vector<std::string> lines)
{
    if (lines == stats_lines_)
        return;
    stats_lines_ = std::move(lines);
    const int height = std::min(
        desktop_height_,
        10 + static_cast<int>(std::max<std::size_t>(stats_lines_.size(), 1))
                * stats_line);
    if (height != stats_.height())
        stats_ = Surface(stats_.width(), height);
    dirty_ = true;
}

DesktopShell::Action DesktopShell::take_action()
{
    return std::exchange(action_, Action::none);
}

const RemoteShortcut* DesktopShell::take_shortcut()
{
    return std::exchange(shortcut_, nullptr);
}

void DesktopShell::draw_text(Surface& target, std::string_view text, int x,
                             int baseline, Color color, float size)
{
    if (text.empty())
        return;
    DrawState state;
    state.high = color;
    state.font.size = size;
    text_.draw(text, Point {static_cast<float>(x), static_cast<float>(baseline)},
               state, target);
}

void DesktopShell::draw_button(const Region& region, std::string_view label)
{
    const bool hot = region.hit(pointer_x_, pointer_y_);
    fill(bar_, region.x, region.y, region.w, region.h,
         hot ? button_hot : button_face);
    DrawState measure;
    measure.font.size = 12;
    const float advance = text_.width(label, measure.font);
    const int tx =
        region.x + std::max(4, (region.w - static_cast<int>(advance)) / 2);
    draw_text(bar_, label, tx, region.y + 17, text_primary, 12);
}

const Surface& DesktopShell::render_bar()
{
    bar_.clear(panel);
    fill(bar_, 0, bar_height - 1, bar_.width(), 1, border);

    // The name and status, clipped to the space left of the buttons.
    const int text_right = regions_.empty() ? bar_.width() : regions_.front().x - 8;
    Color status_color = good;
    std::string status;
    switch (link_) {
    case Link::connected: status = "Connected"; status_color = good; break;
    case Link::connecting: status = "Connecting"; status_color = accent; break;
    case Link::waiting: status = "Reconnecting"; status_color = accent; break;
    case Link::lost: status = "Disconnected"; status_color = danger; break;
    }
    if (!detail_.empty())
        status += " \xE2\x80\x94 " + detail_;
    if (keys_mode_)
        status = "Send to the remote:";
    Surface label(std::max(text_right - bar_margin, 1), bar_height - 1);
    label.clear(panel);
    draw_text(label, name_, 0, 16, text_primary, 13);
    draw_text(label, status, 0, 32, keys_mode_ ? text_muted : status_color, 11);
    for (int y = 0; y < label.height(); ++y)
        for (int x = 0; x < label.width(); ++x)
            bar_.set_pixel(bar_margin + x, y, label.pixel(x, y));

    for (const Region& region : regions_) {
        const std::string_view text = region.kind == Region::Kind::shortcut
            ? shortcuts[static_cast<std::size_t>(region.index)].label
            : button_label(region.kind);
        draw_button(region, text);
    }
    return bar_;
}

const Surface& DesktopShell::render_stats()
{
    stats_.clear(stats_bg);
    int baseline = 5 + 11;
    for (const std::string& line : stats_lines_) {
        draw_text(stats_, line, 8, baseline, text_muted, 11);
        baseline += stats_line;
    }
    return stats_;
}

} // namespace haiku_remote
