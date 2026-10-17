#include "haiku_remote/library_screen.hpp"

#include "haiku_remote/profile_library.hpp"
#include "haiku_remote/profile_store.hpp"
#include "haiku_remote/ui_theme.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>

namespace haiku_remote {

namespace {

namespace theme = ui_theme;

// Layout, in logical units (multiplied by the screen's scale at draw time).
constexpr int margin = 32;
constexpr int max_content_width = 1120; // wider windows centre the content
constexpr int header_button_y = 30;
constexpr int header_button_h = 36;
constexpr int quick_top = 108;
constexpr int quick_h = 64;
constexpr int search_top = 188;
constexpr int search_h = 36;
constexpr int cards_top = 240;
constexpr int card_height = 88;
constexpr int card_gap = 10;
constexpr int footer_h = 28;

constexpr int button_h = 32;
constexpr int button_gap = 8;
constexpr int radius_control = 6;
constexpr int radius_card = 10;
// Focus ring: a 2-unit ring with a 1-unit gap outside the control, so it reads
// against any fill, including an accent-coloured primary button.
constexpr int ring_offset = 3;
constexpr int ring_thickness = 2;

constexpr int editor_form_top = 112;
constexpr int editor_row_h = 70;
constexpr int editor_rows = 6;
// The editor's scrolling band runs from just under the status line to just
// above the action bar; the header and the action bar never scroll.
constexpr int editor_view_top = 104;
constexpr int editor_actions_h = 38;
constexpr int editor_view_gap = 10;  // between the band and the action bar
constexpr int field_label_rise = 18; // a field's label sits this far above it
constexpr int field_h = 32;
constexpr int chip_h = 26;           // a route chip
constexpr int scrollbar_w = 4;
constexpr int scrollbar_min = 24;

// A text field's caret and selection band are inset this far from the box's
// top and bottom edges.
constexpr int caret_inset = 7;
constexpr float field_font = 13;

const char* const default_export_path = "~/haiku-remote-connections.json";

bool is_text_kind(LibraryScreen::Region::Kind kind)
{
    using Kind = LibraryScreen::Region::Kind;
    return kind == Kind::search || kind == Kind::quick_host
        || kind == Kind::editor_field || kind == Kind::transfer_path;
}

bool is_focusable_kind(LibraryScreen::Region::Kind kind)
{
    using Kind = LibraryScreen::Region::Kind;
    return kind != Kind::none && kind != Kind::card;
}

// The modes, their cycle order and their labels are connection_profile.hpp's
// (connection_modes, next_mode(), mode_label()): nothing here enumerates them.

int field_for_validate_name(std::string_view name)
{
    for (int field = 0; field < LibraryScreen::editor_field_count; ++field) {
        if (LibraryScreen::validate_name(field) == name)
            return field;
    }
    return -1;
}

int tab_position(int field)
{
    const auto& order = LibraryScreen::editor_tab_order;
    const auto it = std::find(order.begin(), order.end(), field);
    return it == order.end() ? static_cast<int>(order.size())
                             : static_cast<int>(it - order.begin());
}

const char* field_label(int field)
{
    switch (field) {
    case LibraryScreen::field_name: return "Name";
    case LibraryScreen::field_host: return "Host";
    case LibraryScreen::field_ssh_user: return "SSH user";
    case LibraryScreen::field_remote_port: return "Remote port";
    case LibraryScreen::field_ssh_port: return "SSH port";
    case LibraryScreen::field_local_port: return "Local port";
    case LibraryScreen::field_identity_file: return "Identity file";
    case LibraryScreen::field_known_hosts_file: return "Known hosts file";
    case LibraryScreen::field_cookie_source: return "Cookie source";
    case LibraryScreen::field_width: return "Width";
    case LibraryScreen::field_height: return "Height";
    default: return "";
    }
}

const char* field_placeholder(int field)
{
    switch (field) {
    case LibraryScreen::field_name: return "Defaults to the host";
    case LibraryScreen::field_host: return "hostname or IP address";
    case LibraryScreen::field_local_port: return "Automatic (a free port)";
    case LibraryScreen::field_identity_file: return "Optional \xE2\x80\x94 ssh-agent when empty";
    case LibraryScreen::field_known_hosts_file: return "Optional";
    case LibraryScreen::field_cookie_source: return "Optional \xE2\x80\x94 file path or \"broker\"";
    default: return "";
    }
}

// Remove the last UTF-8 code point (not merely the last byte).
void pop_utf8(std::string& text)
{
    if (text.empty())
        return;
    std::size_t cut = text.size() - 1;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    text.erase(cut);
}

// Typed or pasted text with control characters (newlines and tabs included:
// every field is a single line) removed. Anything else -- any UTF-8 -- is kept.
std::string printable(std::string_view utf8)
{
    std::string out;
    out.reserve(utf8.size());
    for (char c : utf8) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f)
            continue;
        out.push_back(c);
    }
    return out;
}

bool is_continuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// The code-point boundary before / after byte offset `at`.
std::size_t prev_cp(std::string_view text, std::size_t at)
{
    if (at == 0)
        return 0;
    --at;
    while (at > 0 && is_continuation(text[at]))
        --at;
    return at;
}

std::size_t next_cp(std::string_view text, std::size_t at)
{
    if (at >= text.size())
        return text.size();
    ++at;
    while (at < text.size() && is_continuation(text[at]))
        ++at;
    return at;
}

// Word characters: letters, digits, underscore, and every non-ASCII byte (so
// a word in any script holds together). Punctuation that separates the parts
// of a host or a path -- . / @ : - -- is not, so Ctrl-Left steps through them.
bool is_word_byte(char c)
{
    const auto byte = static_cast<unsigned char>(c);
    return byte >= 0x80 || std::isalnum(byte) != 0 || c == '_';
}

std::size_t prev_word(std::string_view text, std::size_t at)
{
    while (at > 0 && !is_word_byte(text[prev_cp(text, at)]))
        at = prev_cp(text, at);
    while (at > 0 && is_word_byte(text[prev_cp(text, at)]))
        at = prev_cp(text, at);
    return at;
}

std::size_t next_word(std::string_view text, std::size_t at)
{
    while (at < text.size() && !is_word_byte(text[at]))
        at = next_cp(text, at);
    while (at < text.size() && is_word_byte(text[at]))
        at = next_cp(text, at);
    return at;
}

// Byte length of the first `count` code points of `text` (all of it if fewer).
std::size_t cp_bytes(std::string_view text, int count)
{
    std::size_t at = 0;
    for (int i = 0; i < count && at < text.size(); ++i)
        at = next_cp(text, at);
    return at;
}

std::int64_t steady_millis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Clip a hit region to the rows [top, bottom) of the window; false when
// nothing of it is left.
bool clip_rows(LibraryScreen::Region& region, int top, int bottom)
{
    const int y0 = std::max(region.y, top);
    const int y1 = std::min(region.y + region.h, bottom);
    if (y1 <= y0)
        return false;
    region.y = y0;
    region.h = y1 - y0;
    return true;
}

// --- pixel-space painting --------------------------------------------------

void fill_px(Surface& surface, int x, int y, int w, int h, Color color)
{
    if (w <= 0 || h <= 0)
        return;
    surface.fill_rect_color(Rect {static_cast<float>(x), static_cast<float>(y),
                                  static_cast<float>(x + w - 1),
                                  static_cast<float>(y + h - 1)},
                            color);
}

// Blend `color` over the existing pixel with coverage cov/16.
void blend_px(Surface& surface, int x, int y, Color color, int cov)
{
    if (cov <= 0 || x < 0 || y < 0 || x >= surface.width() || y >= surface.height())
        return;
    if (cov >= 16) {
        surface.set_pixel(x, y, color);
        return;
    }
    const Color old = surface.pixel(x, y);
    const auto mix = [cov](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint8_t>((a * cov + b * (16 - cov) + 8) / 16);
    };
    surface.set_pixel(x, y, Color {mix(color.r, old.r), mix(color.g, old.g),
                                   mix(color.b, old.b), 255});
}

// A rounded rectangle, filled (thickness <= 0) or as a ring of `thickness`
// pixels. Straight edges are pixel-aligned and exact; only the four corner
// boxes are antialiased, by 4x4 supersampling, so every non-corner pixel is the
// exact theme colour (which is what the pixel tests pin).
void round_rect(Surface& surface, int x, int y, int w, int h, int r, Color color,
                int thickness = 0)
{
    if (w <= 0 || h <= 0)
        return;
    r = std::max(0, std::min({r, w / 2, h / 2}));
    const bool ring = thickness > 0 && 2 * thickness < std::min(w, h);
    if (!ring) {
        fill_px(surface, x + r, y, w - 2 * r, h, color);
        fill_px(surface, x, y + r, r, h - 2 * r, color);
        fill_px(surface, x + w - r, y + r, r, h - 2 * r, color);
    } else {
        const int t = thickness;
        fill_px(surface, x + r, y, w - 2 * r, t, color);
        fill_px(surface, x + r, y + h - t, w - 2 * r, t, color);
        fill_px(surface, x, y + r, t, h - 2 * r, color);
        fill_px(surface, x + w - t, y + r, t, h - 2 * r, color);
    }
    if (r == 0)
        return;
    const float outer2 = static_cast<float>(r) * static_cast<float>(r);
    const float inner = ring ? static_cast<float>(r - thickness) : -1.0f;
    const float inner2 = inner > 0 ? inner * inner : -1.0f;
    struct Corner {
        int bx, by;   // box origin
        float cx, cy; // arc centre
    };
    const Corner corners[4] = {
        {x, y, static_cast<float>(x + r), static_cast<float>(y + r)},
        {x + w - r, y, static_cast<float>(x + w - r), static_cast<float>(y + r)},
        {x, y + h - r, static_cast<float>(x + r), static_cast<float>(y + h - r)},
        {x + w - r, y + h - r, static_cast<float>(x + w - r),
         static_cast<float>(y + h - r)},
    };
    for (const Corner& c : corners) {
        for (int py = c.by; py < c.by + r; ++py) {
            for (int pxl = c.bx; pxl < c.bx + r; ++pxl) {
                int cov = 0;
                for (int sy = 0; sy < 4; ++sy) {
                    for (int sx = 0; sx < 4; ++sx) {
                        const float dx = static_cast<float>(pxl) + (sx + 0.5f) / 4 - c.cx;
                        const float dy = static_cast<float>(py) + (sy + 0.5f) / 4 - c.cy;
                        const float d2 = dx * dx + dy * dy;
                        // inner2 < 0 (no inner arc) keeps every sample.
                        if (d2 <= outer2 && (!ring || d2 > inner2))
                            ++cov;
                    }
                }
                blend_px(surface, pxl, py, color, cov);
            }
        }
    }
}

// A five-pointed star centred on (cx, cy): filled, or outlined at 1.5 units.
void draw_star(Surface& surface, float cx, float cy, float radius, bool filled,
               Color color, float scale)
{
    constexpr float pi = 3.14159265358979f;
    std::array<Point, 10> points;
    for (int i = 0; i < 10; ++i) {
        const float r = i % 2 == 0 ? radius : radius * 0.45f;
        const float angle = -pi / 2 + static_cast<float>(i) * pi / 5;
        points[static_cast<std::size_t>(i)] =
            Point {cx + r * std::cos(angle), cy + r * std::sin(angle)};
    }
    DrawState state;
    state.high = color;
    if (filled) {
        surface.fill_polygon(points, state);
    } else {
        state.pen_size = 1.5f * scale;
        surface.stroke_polyline(points, state, true);
    }
}

// A filled rounded rect with a 1px (device) border of `edge`.
void bordered(Surface& surface, int x, int y, int w, int h, int r, Color fill_color,
              Color edge, int border_px)
{
    round_rect(surface, x, y, w, h, r, edge);
    round_rect(surface, x + border_px, y + border_px, w - 2 * border_px,
               h - 2 * border_px, std::max(0, r - border_px), fill_color);
}

} // namespace

// --------------------------------------------------------------------------
// Construction, sizing, small accessors
// --------------------------------------------------------------------------

LibraryScreen::LibraryScreen(ConnectionLibrary& library, int width, int height)
    : library_(library),
      surface_(std::max(width, min_logical_width),
               std::max(height, min_logical_height)),
      clock_(&ConnectionLibrary::now_seconds),
      millis_(&steady_millis)
{
    blink_epoch_ = millis_();
    const std::size_t count = library_.size();
    status_ = count == 0
                  ? "No saved connections yet. Quick-connect to a host, or add one with New connection."
                  : std::to_string(count) + " saved connection"
                        + (count == 1 ? "" : "s") + ".";
}

void LibraryScreen::resize(int width, int height)
{
    width = std::max(width, min_logical_width);
    height = std::max(height, min_logical_height);
    if (width == surface_.width() && height == surface_.height())
        return;
    surface_ = Surface(width, height);
    dirty_ = true;
}

void LibraryScreen::set_scale(float scale)
{
    if (!(scale >= 1.0f))
        scale = 1.0f; // also catches NaN
    scale = std::min(scale, 4.0f);
    if (scale == scale_)
        return;
    scale_ = scale;
    dirty_ = true;
}

void LibraryScreen::set_clock(std::function<std::int64_t()> clock)
{
    clock_ = clock ? std::move(clock)
                   : std::function<std::int64_t()>(&ConnectionLibrary::now_seconds);
    dirty_ = true;
}

std::optional<ConnectionProfile> LibraryScreen::take_connect_request()
{
    auto request = std::move(connect_request_);
    connect_request_.reset();
    return request;
}

std::string_view LibraryScreen::validate_name(int field)
{
    switch (field) {
    case field_name: return "name";
    case field_host: return "host";
    case field_ssh_user: return "sshUser";
    case field_remote_port: return "remotePort";
    case field_ssh_port: return "sshPort";
    case field_local_port: return "localPort";
    case field_identity_file: return "identityFile";
    case field_known_hosts_file: return "knownHostsFile";
    case field_cookie_source: return "cookieSource";
    case field_width: return "width";
    case field_height: return "height";
    default: return "";
    }
}

std::string LibraryScreen::describe_last_connected(std::int64_t when,
                                                   std::int64_t now)
{
    if (when <= 0)
        return "Never connected";
    const std::int64_t age = now - when;
    if (age >= 0 && age < 60)
        return "Last connected just now";
    if (age >= 0 && age < 3600)
        return "Last connected " + std::to_string(age / 60) + " min ago";
    if (age >= 0 && age < 86400)
        return "Last connected " + std::to_string(age / 3600) + " h ago";
    if (age >= 0 && age < 7 * 86400) {
        const std::int64_t days = age / 86400;
        return "Last connected " + std::to_string(days)
            + (days == 1 ? " day ago" : " days ago");
    }
    // Older than a week, or in the future (a skewed clock): a plain date.
    const std::time_t t = static_cast<std::time_t>(when);
    std::tm tm {};
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0)
        return "Last connected long ago";
#else
    if (localtime_r(&t, &tm) == nullptr)
        return "Last connected long ago";
#endif
    char date[32];
    if (std::strftime(date, sizeof date, "%Y-%m-%d", &tm) == 0)
        return "Last connected long ago";
    return std::string("Last connected on ") + date;
}

std::string LibraryScreen::card_meta(const ConnectionProfile& profile,
                                     std::int64_t now)
{
    return mode_label(profile.mode) + "  \xC2\xB7  " + std::to_string(profile.width)
        + " \xC3\x97 " + std::to_string(profile.height) + "  \xC2\xB7  "
        + describe_last_connected(profile.last_connected_at, now);
}

int LibraryScreen::px(int logical) const
{
    return static_cast<int>(std::lround(static_cast<float>(logical) * scale_));
}

LibraryScreen::Region LibraryScreen::make_region(Region::Kind kind, int lx, int ly,
                                                 int lw, int lh,
                                                 std::string profile_id,
                                                 int field) const
{
    Region region;
    region.kind = kind;
    region.profile_id = std::move(profile_id);
    region.field = field;
    region.x = px(lx);
    region.y = px(ly);
    region.w = px(lx + lw) - region.x;
    region.h = px(ly + lh) - region.y;
    return region;
}

void LibraryScreen::add_region(Region region)
{
    regions_.push_back(std::move(region));
}

// --------------------------------------------------------------------------
// Focus
// --------------------------------------------------------------------------

std::vector<LibraryScreen::Focus> LibraryScreen::focus_order() const
{
    using Kind = Region::Kind;
    std::vector<Focus> order;
    if (view_ == View::list) {
        // Visual order: header actions, quick connect, search, then the cards.
        for (Kind kind : {Kind::import_profiles, Kind::export_profiles,
                          Kind::new_connection, Kind::quick_host, Kind::quick_mode,
                          Kind::quick_connect, Kind::quick_options, Kind::search})
            order.push_back(Focus {kind, {}, -1});
        for (const auto& profile : library_.search(search_)) {
            if (profile.id == pending_delete_) {
                order.push_back(Focus {Kind::confirm_delete, profile.id, -1});
                order.push_back(Focus {Kind::cancel_delete, profile.id, -1});
                continue;
            }
            for (Kind kind : {Kind::favorite_card, Kind::connect, Kind::edit,
                              Kind::duplicate, Kind::remove})
                order.push_back(Focus {kind, profile.id, -1});
        }
    } else if (view_ == View::editor) {
        for (int field : editor_tab_order)
            order.push_back(Focus {Kind::editor_field, {}, field});
        for (Kind kind : {Kind::mode_cycle, Kind::favorite_toggle,
                          Kind::autoreconnect_toggle,
                          editor_.quick ? Kind::connect_now : Kind::save,
                          Kind::save_and_connect, Kind::cancel})
            order.push_back(Focus {kind, {}, -1});
    } else {
        for (Kind kind : {Kind::transfer_path, Kind::transfer_confirm,
                          Kind::transfer_cancel})
            order.push_back(Focus {kind, {}, -1});
    }
    return order;
}

void LibraryScreen::set_focus(Focus focus)
{
    const bool changed = !(focus == focus_);
    focus_ = std::move(focus);
    if (changed) {
        // A newly focused field starts with its caret at the end and nothing
        // selected; the editor brings the control into view on next render.
        reset_edit();
        if (view_ == View::editor)
            reveal_pending_ = true;
    }
    sync_selection_to_focus();
}

void LibraryScreen::move_focus(int delta, bool wrap)
{
    const auto order = focus_order();
    if (order.empty())
        return;
    const auto it = std::find(order.begin(), order.end(), focus_);
    const int count = static_cast<int>(order.size());
    int index = 0;
    if (it == order.end()) {
        index = delta > 0 ? 0 : count - 1;
    } else {
        index = static_cast<int>(it - order.begin()) + delta;
        if (wrap)
            index = ((index % count) + count) % count;
        else
            index = std::clamp(index, 0, count - 1);
    }
    set_focus(order[static_cast<std::size_t>(index)]);
    focus_visible_ = true;
}

void LibraryScreen::sync_selection_to_focus()
{
    if (view_ != View::list || focus_.profile_id.empty())
        return;
    const auto visible = library_.search(search_);
    for (std::size_t i = 0; i < visible.size(); ++i) {
        if (visible[i].id == focus_.profile_id) {
            selected_ = i;
            return;
        }
    }
}

bool LibraryScreen::focus_is_text() const
{
    return is_text_kind(focus_.kind);
}

std::string* LibraryScreen::focused_text()
{
    switch (focus_.kind) {
    case Region::Kind::search: return &search_;
    case Region::Kind::quick_host: return &quick_host_;
    case Region::Kind::transfer_path: return &transfer_.path;
    case Region::Kind::editor_field:
        if (focus_.field >= 0 && focus_.field < editor_field_count)
            return &editor_.text[static_cast<std::size_t>(focus_.field)];
        return nullptr;
    default: return nullptr;
    }
}

bool LibraryScreen::is_focused(const Region& region) const
{
    return region.kind == focus_.kind && region.profile_id == focus_.profile_id
        && region.field == focus_.field;
}

void LibraryScreen::activate(const Focus& focus)
{
    Region region;
    region.kind = focus.kind;
    region.profile_id = focus.profile_id;
    region.field = focus.field;
    if (view_ == View::list)
        list_pointer_press(region);
    else if (view_ == View::editor)
        editor_pointer_press(region);
    else
        transfer_pointer_press(region);
}

// --------------------------------------------------------------------------
// Input
// --------------------------------------------------------------------------

void LibraryScreen::pointer_move(int x, int y)
{
    pointer_x_ = x;
    pointer_y_ = y;
    int hovered = -1;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        if (regions_[i].hit(x, y)) {
            hovered = static_cast<int>(i);
            break;
        }
    }
    if (hovered != hovered_) {
        hovered_ = hovered;
        dirty_ = true;
    }
    // A drag that began in a text field moves the caret (the anchor stays),
    // following the pointer even when it leaves the box.
    if (edit_.dragging && focus_is_text()) {
        for (const Region& region : regions_) {
            if (!is_focused(region))
                continue;
            if (std::string* text = focused_text()) {
                const int rel = x - (region.x + px(text_pad(region.kind))) + edit_.hscroll;
                const std::size_t at = offset_at(*text, rel);
                if (at != edit_.caret) {
                    edit_.caret = at;
                    touch_caret();
                }
            }
            break;
        }
    }
}

void LibraryScreen::pointer_press(int x, int y, int clicks, unsigned mods)
{
    pointer_x_ = x;
    pointer_y_ = y;
    edit_.dragging = false;
    for (const Region& hit : regions_) {
        if (!hit.hit(x, y))
            continue;
        const Region region = hit; // the handlers may rebuild regions_
        focus_visible_ = false;
        if (is_focusable_kind(region.kind))
            set_focus(Focus {region.kind, region.profile_id, region.field});
        if (is_text_kind(region.kind)) {
            // Place the caret under the pointer (the box's horizontal scroll
            // included), or select a word / everything on a double / triple
            // click; Shift extends the existing selection instead.
            if (std::string* text = focused_text()) {
                clamp_edit();
                edit_.composition.clear();
                const int rel = x - (region.x + px(text_pad(region.kind))) + edit_.hscroll;
                const std::size_t at = offset_at(*text, rel);
                if (clicks >= 3) {
                    edit_.anchor = 0;
                    edit_.caret = text->size();
                } else if (clicks == 2) {
                    // The word under the pointer; between words, the run of
                    // separators there.
                    std::size_t from = at;
                    std::size_t to = at;
                    const bool in_word = at < text->size() && is_word_byte((*text)[at]);
                    while (from > 0
                           && is_word_byte((*text)[prev_cp(*text, from)]) == in_word)
                        from = prev_cp(*text, from);
                    while (to < text->size() && is_word_byte((*text)[to]) == in_word)
                        to = next_cp(*text, to);
                    edit_.anchor = from;
                    edit_.caret = to;
                } else {
                    edit_.caret = at;
                    if ((mods & mod_shift) == 0)
                        edit_.anchor = at;
                    edit_.dragging = true;
                }
                touch_caret();
            }
        }
        if (view_ == View::list)
            list_pointer_press(region);
        else if (view_ == View::editor)
            editor_pointer_press(region);
        else
            transfer_pointer_press(region);
        dirty_ = true;
        return;
    }
}

void LibraryScreen::pointer_release(int x, int y)
{
    pointer_x_ = x;
    pointer_y_ = y;
    edit_.dragging = false;
}

void LibraryScreen::text_input(std::string_view utf8)
{
    if (utf8.empty())
        return;
    dirty_ = true;
    edit_.composition.clear(); // a commit ends any composition
    // Space on a button or toggle presses it, as everywhere else.
    if (utf8 == " " && !focus_is_text() && focus_.kind != Region::Kind::none) {
        activate(focus_);
        return;
    }
    // Anything typed elsewhere in the list filters it (type-to-search).
    if (view_ == View::list && !focus_is_text())
        set_focus(Focus {Region::Kind::search, {}, -1});
    insert_text(utf8);
}

void LibraryScreen::text_editing(std::string_view composition, int cursor)
{
    if (!focus_is_text())
        return;
    std::string shown = printable(composition);
    if (shown.empty() && edit_.composition.empty())
        return;
    if (!shown.empty() && has_selection()) {
        // Composing over a selection replaces it, as typing would.
        if (std::string* text = focused_text()) {
            delete_selection(*text);
            text_changed();
        }
    }
    edit_.composition = std::move(shown);
    edit_.composition_cursor = cursor;
    touch_caret();
}

void LibraryScreen::key(Key k, unsigned mods)
{
    if (k == Key::none)
        return;
    dirty_ = true;
    // While an input method is composing, its keys are its own.
    if (!edit_.composition.empty() && focus_is_text())
        return;
    if (focus_is_text() && edit_key(k, mods))
        return;
    if (view_ == View::list)
        list_key(k);
    else if (view_ == View::editor)
        editor_key(k);
    else
        transfer_key(k);
}

void LibraryScreen::wheel(float notches)
{
    if (!(notches == notches) || notches == 0.0f) // NaN or nothing
        return;
    if (view_ == View::editor) {
        const EditorLayout layout = editor_layout();
        set_editor_scroll(editor_scroll_
                              - static_cast<int>(std::lround(notches * wheel_step)),
                          layout);
        return;
    }
    if (view_ != View::list)
        return;
    // A notch moves the list one card. Fractional (trackpad) notches add up.
    wheel_rest_ += notches;
    const int whole = static_cast<int>(wheel_rest_);
    wheel_rest_ -= static_cast<float>(whole);
    if (whole != 0)
        scroll_list(-whole);
}

void LibraryScreen::scroll_list(int cards)
{
    const auto visible = library_.search(search_);
    const std::size_t per_page = cards_per_page();
    if (visible.size() <= per_page)
        return;
    const int max_scroll = static_cast<int>(visible.size() - per_page);
    const int scroll = std::clamp(static_cast<int>(scroll_) + cards, 0, max_scroll);
    if (scroll == static_cast<int>(scroll_))
        return;
    scroll_ = static_cast<std::size_t>(scroll);
    // The selection rides along inside the page, so render's
    // ensure_selection_visible does not scroll straight back; keyboard focus
    // on a card's control moves to the same control on the selected card.
    const std::size_t selected = std::clamp(selected_, scroll_, scroll_ + per_page - 1);
    if (selected != selected_) {
        selected_ = selected;
        if (!focus_.profile_id.empty()) {
            Region::Kind kind = focus_.kind;
            if (kind == Region::Kind::confirm_delete || kind == Region::Kind::cancel_delete)
                kind = Region::Kind::remove;
            pending_delete_.clear();
            set_focus(Focus {kind, visible[selected_].id, -1});
        }
    }
    dirty_ = true;
}

void LibraryScreen::set_clipboard(std::function<std::string()> read,
                                  std::function<void(const std::string&)> write)
{
    clipboard_read_ = std::move(read);
    clipboard_write_ = std::move(write);
}

void LibraryScreen::set_millis_clock(std::function<std::int64_t()> clock)
{
    millis_ = clock ? std::move(clock) : std::function<std::int64_t()>(&steady_millis);
    blink_epoch_ = millis_();
    dirty_ = true;
}

bool LibraryScreen::caret_on() const
{
    if (!window_active_)
        return false;
    const std::int64_t elapsed = std::max<std::int64_t>(0, millis_() - blink_epoch_);
    return (elapsed / caret_blink_ms) % 2 == 0;
}

int LibraryScreen::next_wake_ms() const
{
    if (!window_active_ || !focus_is_text())
        return -1;
    const std::int64_t elapsed = std::max<std::int64_t>(0, millis_() - blink_epoch_);
    return static_cast<int>(caret_blink_ms - elapsed % caret_blink_ms);
}

void LibraryScreen::tick()
{
    if (!focus_is_text())
        return;
    if ((caret_on() ? 0 : 1) != rendered_caret_phase_)
        dirty_ = true;
}

void LibraryScreen::set_window_active(bool active)
{
    if (active == window_active_)
        return;
    window_active_ = active;
    edit_.dragging = false;
    touch_caret();
}

// --------------------------------------------------------------------------
// Text editing (one focused field at a time)
// --------------------------------------------------------------------------

void LibraryScreen::reset_edit()
{
    const std::string* text = focused_text();
    edit_ = TextEdit {};
    edit_.caret = edit_.anchor = text != nullptr ? text->size() : 0;
    touch_caret();
}

void LibraryScreen::clamp_edit()
{
    const std::string* text = focused_text();
    if (text == nullptr) {
        edit_.caret = edit_.anchor = 0;
        return;
    }
    // The text can change under the caret (Escape clears a field); keep both
    // ends inside it and on a code-point boundary.
    for (std::size_t* at : {&edit_.caret, &edit_.anchor}) {
        *at = std::min(*at, text->size());
        while (*at > 0 && *at < text->size() && is_continuation((*text)[*at]))
            --*at;
    }
}

void LibraryScreen::touch_caret()
{
    blink_epoch_ = millis_();
    dirty_ = true;
}

void LibraryScreen::text_changed()
{
    touch_caret();
    if (view_ == View::editor) {
        editor_changed();
    } else if (focus_.kind == Region::Kind::search) {
        selected_ = 0;
        scroll_ = 0;
    }
}

void LibraryScreen::delete_selection(std::string& text)
{
    const std::size_t from = std::min(edit_.caret, edit_.anchor);
    const std::size_t to = std::max(edit_.caret, edit_.anchor);
    text.erase(from, to - from);
    edit_.caret = edit_.anchor = from;
}

void LibraryScreen::insert_text(std::string_view utf8)
{
    std::string* text = focused_text();
    if (text == nullptr)
        return;
    const std::string clean = printable(utf8);
    if (clean.empty() && !has_selection())
        return;
    clamp_edit();
    delete_selection(*text);
    text->insert(edit_.caret, clean);
    edit_.caret += clean.size();
    edit_.anchor = edit_.caret;
    text_changed();
}

std::string LibraryScreen::selected_text() const
{
    const std::string* text = const_cast<LibraryScreen*>(this)->focused_text();
    if (text == nullptr)
        return {};
    const std::size_t from = std::min({edit_.caret, edit_.anchor, text->size()});
    const std::size_t to = std::min(std::max(edit_.caret, edit_.anchor), text->size());
    return text->substr(from, to - from);
}

bool LibraryScreen::edit_key(Key k, unsigned mods)
{
    std::string* text = focused_text();
    if (text == nullptr)
        return false;
    clamp_edit();
    const bool shift = (mods & mod_shift) != 0;
    const bool word = (mods & mod_word) != 0;
    const auto move_to = [&](std::size_t at) {
        edit_.caret = at;
        if (!shift)
            edit_.anchor = at;
        touch_caret();
    };
    const auto write = [&](const std::string& value) {
        if (clipboard_write_)
            clipboard_write_(value);
        else
            own_clipboard_ = value;
    };
    switch (k) {
    case Key::left:
        // Left with a selection collapses it to its start.
        if (has_selection() && !shift)
            move_to(std::min(edit_.caret, edit_.anchor));
        else
            move_to(word ? prev_word(*text, edit_.caret) : prev_cp(*text, edit_.caret));
        return true;
    case Key::right:
        if (has_selection() && !shift)
            move_to(std::max(edit_.caret, edit_.anchor));
        else
            move_to(word ? next_word(*text, edit_.caret) : next_cp(*text, edit_.caret));
        return true;
    case Key::home:
        move_to(0);
        return true;
    case Key::end:
        move_to(text->size());
        return true;
    case Key::select_all:
        edit_.anchor = 0;
        edit_.caret = text->size();
        touch_caret();
        return true;
    case Key::backspace:
    case Key::del: {
        if (!has_selection()) {
            const bool back = k == Key::backspace;
            const std::size_t other = back
                ? (word ? prev_word(*text, edit_.caret) : prev_cp(*text, edit_.caret))
                : (word ? next_word(*text, edit_.caret) : next_cp(*text, edit_.caret));
            if (other == edit_.caret)
                return true; // nothing on that side
            edit_.anchor = other;
        }
        delete_selection(*text);
        text_changed();
        return true;
    }
    case Key::copy:
        if (has_selection())
            write(selected_text());
        return true;
    case Key::cut:
        if (has_selection()) {
            write(selected_text());
            delete_selection(*text);
            text_changed();
        }
        return true;
    case Key::paste:
        insert_text(clipboard_read_ ? clipboard_read_() : own_clipboard_);
        return true;
    default:
        return false;
    }
}

int LibraryScreen::text_pad(Region::Kind kind) const
{
    return kind == Region::Kind::search ? 12 : 10;
}

// The code-point boundary nearest `rel_px` device pixels from the text origin.
std::size_t LibraryScreen::offset_at(std::string_view text, int rel_px)
{
    Font font;
    font.size = field_font * scale_;
    std::size_t best = 0;
    float best_distance = std::fabs(static_cast<float>(rel_px));
    for (std::size_t at = next_cp(text, 0); at > 0; at = next_cp(text, at)) {
        const float distance =
            std::fabs(static_cast<float>(rel_px) - text_.width(text.substr(0, at), font));
        if (distance < best_distance) {
            best_distance = distance;
            best = at;
        }
        if (at >= text.size())
            break;
    }
    return best;
}

void LibraryScreen::list_key(Key k)
{
    using Kind = Region::Kind;
    switch (k) {
    case Key::backspace:
    case Key::del:
    case Key::paste:
        // Off any text field, these edit the search, like typing does.
        // (On a text field, key() already handed them to edit_key.)
        set_focus(Focus {Kind::search, {}, -1});
        (void)edit_key(k, mod_none);
        break;
    case Key::page_up:
    case Key::page_down: {
        const int page = static_cast<int>(cards_per_page());
        scroll_list(k == Key::page_down ? page : -page);
        break;
    }
    case Key::tab:
        move_focus(+1, true);
        break;
    case Key::back_tab:
        move_focus(-1, true);
        break;
    case Key::up:
    case Key::down: {
        const auto visible = library_.search(search_);
        if (visible.empty())
            break;
        const bool down = k == Key::down;
        if (!focus_.profile_id.empty()) {
            // On a card control: the same control on the neighbouring card.
            std::size_t index = selected_;
            if (down && index + 1 < visible.size())
                ++index;
            else if (!down && index > 0)
                --index;
            Kind kind = focus_.kind;
            if (kind == Kind::confirm_delete || kind == Kind::cancel_delete)
                kind = Kind::remove;
            pending_delete_.clear();
            set_focus(Focus {kind, visible[index].id, -1});
            focus_visible_ = true;
        } else if (down) {
            if (selected_ + 1 < visible.size())
                ++selected_;
        } else if (selected_ > 0) {
            --selected_;
        }
        break;
    }
    case Key::enter:
        if (focus_.kind == Kind::search || focus_.kind == Kind::none) {
            const auto visible = library_.search(search_);
            if (selected_ < visible.size())
                connect_profile(visible[selected_].id);
        } else if (focus_.kind == Kind::quick_host) {
            quick_connect();
        } else {
            activate(focus_);
        }
        break;
    case Key::escape:
        if (!pending_delete_.empty()) {
            Region keep;
            keep.kind = Kind::cancel_delete;
            keep.profile_id = pending_delete_;
            list_pointer_press(keep);
        } else if (focus_.kind == Kind::quick_host && !quick_host_.empty()) {
            quick_host_.clear();
        } else if (!search_.empty()) {
            search_.clear();
            selected_ = 0;
            scroll_ = 0;
        }
        break;
    default:
        break;
    }
}

// --------------------------------------------------------------------------
// List view
// --------------------------------------------------------------------------

void LibraryScreen::begin_new()
{
    editor_ = EditorState {};
    editor_scroll_ = 0;
    editor_.is_new = true;
    editor_.text[field_ssh_user] = "baron";
    editor_.text[field_remote_port] = "10900";
    editor_.text[field_ssh_port] = "22";
    editor_.text[field_width] = "1280";
    editor_.text[field_height] = "800";
    list_focus_ = focus_;
    view_ = View::editor;
    set_focus(Focus {Region::Kind::editor_field, {}, field_host});
    status_ = "New connection. The name defaults to the host.";
    status_error_ = false;
}

void LibraryScreen::begin_quick()
{
    begin_new();
    editor_.quick = true;
    editor_.mode = quick_mode_;
    std::string host = quick_host_;
    const auto at = host.rfind('@');
    if (at != std::string::npos) {
        editor_.text[field_ssh_user] = host.substr(0, at);
        host.erase(0, at + 1);
    }
    editor_.text[field_host] = host;
    status_ = "Quick connect: nothing is saved unless you choose Save & connect.";
    reset_edit(); // the host was filled in after focus landed on it
}

void LibraryScreen::begin_edit(const ConnectionProfile& profile)
{
    editor_ = EditorState {};
    editor_scroll_ = 0;
    editor_.id = profile.id;
    editor_.is_new = false;
    editor_.mode = profile.mode;
    editor_.favorite = profile.favorite;
    editor_.auto_reconnect = profile.auto_reconnect;
    editor_.text[field_name] = profile.name;
    editor_.text[field_host] = profile.host;
    editor_.text[field_ssh_user] = profile.ssh_user;
    editor_.text[field_remote_port] = std::to_string(profile.remote_port);
    editor_.text[field_ssh_port] = std::to_string(profile.ssh_port);
    editor_.text[field_local_port] =
        profile.local_port ? std::to_string(*profile.local_port) : std::string();
    editor_.text[field_identity_file] = profile.identity_file;
    editor_.text[field_known_hosts_file] = profile.known_hosts_file;
    editor_.text[field_cookie_source] = profile.cookie_source;
    editor_.text[field_width] = std::to_string(profile.width);
    editor_.text[field_height] = std::to_string(profile.height);
    list_focus_ = focus_;
    view_ = View::editor;
    set_focus(Focus {Region::Kind::editor_field, {}, field_name});
    status_ = "Editing '" + profile.name + "'.";
    status_error_ = false;
}

void LibraryScreen::begin_transfer(bool is_export)
{
    transfer_ = TransferState {};
    transfer_.is_export = is_export;
    transfer_.path = default_export_path;
    list_focus_ = focus_;
    view_ = View::transfer;
    set_focus(Focus {Region::Kind::transfer_path, {}, -1});
}

void LibraryScreen::return_to_list(std::string status, bool is_error)
{
    view_ = View::list;
    pending_delete_.clear();
    status_ = std::move(status);
    status_error_ = is_error;
    // Return focus where it was, if that control still exists.
    const auto order = focus_order();
    if (std::find(order.begin(), order.end(), list_focus_) != order.end())
        set_focus(list_focus_);
    else
        set_focus(Focus {Region::Kind::search, {}, -1});
}

void LibraryScreen::connect_profile(const std::string& id)
{
    const ConnectionProfile* profile = library_.find(id);
    if (profile == nullptr)
        return;
    connect_request_ = *profile;
    status_ = "Connecting to '" + profile->name + "'...";
    status_error_ = false;
}

void LibraryScreen::quick_connect()
{
    ConnectionProfile profile;
    profile.mode = quick_mode_;
    std::string host = quick_host_;
    const auto at = host.rfind('@');
    if (at != std::string::npos) {
        profile.ssh_user = host.substr(0, at);
        host.erase(0, at + 1);
    }
    profile.host = host;
    profile.name = host;
    const ValidationResult validation = validate(profile);
    if (!validation.ok()) {
        // The name is the host here, so a bad host is the problem to report.
        const FieldError* host_error = validation.find("host");
        const FieldError& first =
            host_error != nullptr ? *host_error : validation.errors.front();
        status_ = "Quick connect: " + first.message + ".";
        status_error_ = true;
        set_focus(Focus {Region::Kind::quick_host, {}, -1});
        return;
    }
    connect_request_ = profile; // empty id: in no library, never saved
    status_ = "Connecting to " + profile.host + "...";
    status_error_ = false;
}

void LibraryScreen::list_pointer_press(const Region& region)
{
    using Kind = Region::Kind;
    // Any other action dismisses a pending delete confirmation.
    if (region.kind != Kind::confirm_delete && region.kind != Kind::cancel_delete
        && region.kind != Kind::none)
        pending_delete_.clear();

    switch (region.kind) {
    case Kind::new_connection:
        begin_new();
        break;
    case Kind::import_profiles:
        begin_transfer(false);
        break;
    case Kind::export_profiles:
        begin_transfer(true);
        break;
    case Kind::quick_mode:
        quick_mode_ = next_mode(quick_mode_);
        break;
    case Kind::quick_connect:
        quick_connect();
        break;
    case Kind::quick_options:
        begin_quick();
        break;
    case Kind::card: {
        const auto visible = library_.search(search_);
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (visible[i].id == region.profile_id)
                selected_ = i;
        }
        break;
    }
    case Kind::connect:
        connect_profile(region.profile_id);
        break;
    case Kind::edit:
        if (const ConnectionProfile* p = library_.find(region.profile_id))
            begin_edit(*p);
        break;
    case Kind::duplicate:
        if (auto copy = library_.duplicate(region.profile_id)) {
            const SaveResult saved = library_.save();
            status_ = saved ? "Duplicated to '" + copy->name + "'."
                            : "Duplicate not saved: " + saved.message;
            status_error_ = !saved;
        }
        break;
    case Kind::remove:
        // Deleting asks first: the card turns into a confirmation with the
        // safe choice (Keep) focused.
        if (const ConnectionProfile* p = library_.find(region.profile_id)) {
            pending_delete_ = p->id;
            set_focus(Focus {Kind::cancel_delete, p->id, -1});
            status_ = "Delete '" + p->name
                + "'? Choose Delete to confirm, or Keep.";
            status_error_ = false;
        }
        break;
    case Kind::confirm_delete:
        if (region.profile_id == pending_delete_) {
            std::string name;
            if (const ConnectionProfile* p = library_.find(region.profile_id))
                name = p->name;
            library_.remove(region.profile_id);
            pending_delete_.clear();
            const SaveResult saved = library_.save();
            status_ = saved ? "Deleted '" + name + "'."
                            : "Delete not saved: " + saved.message;
            status_error_ = !saved;
            selected_ = 0;
            set_focus(Focus {Kind::search, {}, -1});
        }
        break;
    case Kind::cancel_delete:
        if (!pending_delete_.empty()) {
            const std::string id = pending_delete_;
            pending_delete_.clear();
            set_focus(Focus {Kind::remove, id, -1});
            status_ = "Kept the connection.";
            status_error_ = false;
        }
        break;
    case Kind::favorite_card:
        if (const ConnectionProfile* p = library_.find(region.profile_id)) {
            ConnectionProfile updated = *p;
            updated.favorite = !updated.favorite;
            library_.update(updated);
            const SaveResult saved = library_.save();
            if (!saved) {
                status_ = "Favorite not saved: " + saved.message;
                status_error_ = true;
            }
            sync_selection_to_focus(); // the card may have moved
        }
        break;
    case Kind::search:
    case Kind::quick_host:
        // Text fields collect typing once focused; a click only focuses.
        break;
    default:
        break;
    }
}

std::size_t LibraryScreen::cards_per_page() const
{
    const int logical_h = static_cast<int>(static_cast<float>(surface_.height()) / scale_);
    const int viewport = logical_h - cards_top - footer_h;
    return static_cast<std::size_t>(
        std::max(1, (viewport + card_gap) / (card_height + card_gap)));
}

void LibraryScreen::ensure_selection_visible(std::size_t visible_count)
{
    if (visible_count == 0) {
        selected_ = 0;
        scroll_ = 0;
        return;
    }
    if (selected_ >= visible_count)
        selected_ = visible_count - 1;
    const std::size_t per_page = cards_per_page();
    if (selected_ < scroll_)
        scroll_ = selected_;
    else if (selected_ >= scroll_ + per_page)
        scroll_ = selected_ - per_page + 1;
    if (scroll_ > visible_count - 1)
        scroll_ = 0;
}

// --------------------------------------------------------------------------
// Editor view
// --------------------------------------------------------------------------

std::optional<ConnectionProfile>
LibraryScreen::build_editor_profile(std::vector<FieldError>& errors) const
{
    errors.clear();
    ConnectionProfile profile;
    profile.id = editor_.id;
    profile.name = editor_.text[field_name];
    profile.host = editor_.text[field_host];
    profile.ssh_user = editor_.text[field_ssh_user];
    profile.identity_file = editor_.text[field_identity_file];
    profile.known_hosts_file = editor_.text[field_known_hosts_file];
    profile.cookie_source = editor_.text[field_cookie_source];
    profile.mode = editor_.mode;
    profile.favorite = editor_.favorite;
    profile.auto_reconnect = editor_.auto_reconnect;

    // Numbers are parsed strictly: an empty or non-numeric field is an error
    // on that field, never a silent 0 (issue #1 section 2).
    const auto number = [&](int field, int& out) {
        std::string why;
        if (auto value = parse_whole_number(editor_.text[static_cast<std::size_t>(field)], why))
            out = *value;
        else
            errors.push_back({std::string(validate_name(field)),
                              std::string(field_label(field)) + " " + why});
    };
    number(field_remote_port, profile.remote_port);
    number(field_ssh_port, profile.ssh_port);
    number(field_width, profile.width);
    number(field_height, profile.height);
    if (editor_.text[field_local_port].empty()) {
        profile.local_port = std::nullopt; // automatic
    } else {
        int local = 0;
        number(field_local_port, local);
        if (errors.empty() || errors.back().field != "localPort")
            profile.local_port = local;
    }

    // The documented default: a profile is named after its host.
    const bool name_defaulted = profile.name.empty();
    if (name_defaulted)
        profile.name = profile.host;

    // History the editor does not expose survives an edit untouched.
    if (const ConnectionProfile* existing = library_.find(editor_.id)) {
        profile.last_connected_at = existing->last_connected_at;
        profile.last_error = existing->last_error;
        profile.extra_fields = existing->extra_fields;
    }

    const ValidationResult validation = validate(profile);
    for (const FieldError& error : validation.errors) {
        const bool already = std::any_of(
            errors.begin(), errors.end(),
            [&](const FieldError& e) { return e.field == error.field; });
        if (already)
            continue; // the parse error for this field is the more useful one
        if (name_defaulted && error.field == "name")
            continue; // follows from the empty host, which is reported itself
        errors.push_back(error);
    }
    std::stable_sort(errors.begin(), errors.end(),
                     [](const FieldError& a, const FieldError& b) {
                         return tab_position(field_for_validate_name(a.field))
                              < tab_position(field_for_validate_name(b.field));
                     });
    if (!errors.empty())
        return std::nullopt;
    return profile;
}

ConnectionProfile LibraryScreen::preview_profile() const
{
    ConnectionProfile profile;
    profile.mode = editor_.mode;
    profile.host = editor_.text[field_host];
    std::string why;
    if (auto v = parse_whole_number(editor_.text[field_remote_port], why))
        profile.remote_port = *v;
    if (auto v = parse_whole_number(editor_.text[field_ssh_port], why))
        profile.ssh_port = *v;
    if (auto v = parse_whole_number(editor_.text[field_local_port], why))
        profile.local_port = *v;
    return profile;
}

void LibraryScreen::editor_changed()
{
    if (editor_.show_errors) {
        std::vector<FieldError> errors;
        (void)build_editor_profile(errors);
        editor_.errors = std::move(errors);
    }
}

void LibraryScreen::commit_editor(Commit how)
{
    std::vector<FieldError> errors;
    std::optional<ConnectionProfile> built = build_editor_profile(errors);
    editor_.show_errors = true;
    editor_.errors = errors;
    if (!built) {
        const int bad = field_for_validate_name(errors.front().field);
        if (bad >= 0)
            set_focus(Focus {Region::Kind::editor_field, {}, bad});
        // The status names every field with a problem -- including any that
        // has no box of its own -- and each box lists its own messages.
        const std::size_t n = errors.size();
        status_ = std::to_string(n) + (n == 1 ? " problem" : " problems")
            + " to fix before "
            + (how == Commit::connect_only ? "connecting: " : "saving: ");
        for (std::size_t i = 0; i < n; ++i) {
            const int field = field_for_validate_name(errors[i].field);
            if (i > 0)
                status_ += ", ";
            status_ += field >= 0 ? field_label(field) : errors[i].field;
        }
        status_ += ".";
        status_error_ = true;
        return;
    }
    ConnectionProfile profile = std::move(*built);

    if (how == Commit::connect_only) {
        connect_request_ = profile; // a quick connect: never saved
        return_to_list("Connecting to " + profile.host + "...");
        return;
    }

    const bool is_new = editor_.is_new || editor_.id.empty();
    if (is_new)
        profile = library_.add(profile);
    else
        library_.update(profile);

    const SaveResult saved = library_.save();
    if (!saved) {
        status_ = "Save failed: " + saved.message;
        status_error_ = true;
        return;
    }
    if (how == Commit::save_and_connect) {
        connect_request_ = profile;
        return_to_list("Saved '" + profile.name + "'. Connecting...");
        return;
    }
    return_to_list((is_new ? "Added '" : "Saved '") + profile.name + "'.");
}

void LibraryScreen::editor_key(Key k)
{
    using Kind = Region::Kind;
    switch (k) {
    case Key::page_up:
    case Key::page_down: {
        // A page is the visible band less a row, so a field half-visible at
        // the edge stays in sight across the jump.
        const EditorLayout layout = editor_layout();
        const int page = std::max(editor_row_h,
                                  layout.view_bottom - layout.view_top - editor_row_h);
        set_editor_scroll(editor_scroll_ + (k == Key::page_down ? page : -page), layout);
        break;
    }
    case Key::tab:
        move_focus(+1, true);
        break;
    case Key::back_tab:
        move_focus(-1, true);
        break;
    case Key::up:
        move_focus(-1, false);
        break;
    case Key::down:
        move_focus(+1, false);
        break;
    case Key::enter:
        if (focus_.kind == Kind::editor_field || focus_.kind == Kind::none)
            commit_editor(editor_.quick ? Commit::connect_only : Commit::save);
        else
            activate(focus_);
        break;
    case Key::escape:
        return_to_list("Edit cancelled.");
        break;
    default:
        break;
    }
}

void LibraryScreen::editor_pointer_press(const Region& region)
{
    switch (region.kind) {
    case Region::Kind::editor_field:
        // Focus was set by pointer_press; nothing else to do.
        break;
    case Region::Kind::mode_cycle:
        editor_.mode = next_mode(editor_.mode);
        editor_changed();
        break;
    case Region::Kind::favorite_toggle:
        editor_.favorite = !editor_.favorite;
        break;
    case Region::Kind::autoreconnect_toggle:
        editor_.auto_reconnect = !editor_.auto_reconnect;
        break;
    case Region::Kind::save:
        commit_editor(Commit::save);
        break;
    case Region::Kind::save_and_connect:
        commit_editor(Commit::save_and_connect);
        break;
    case Region::Kind::connect_now:
        commit_editor(Commit::connect_only);
        break;
    case Region::Kind::cancel:
        return_to_list("Edit cancelled.");
        break;
    default:
        break;
    }
}

// --------------------------------------------------------------------------
// Import / export view
// --------------------------------------------------------------------------

void LibraryScreen::commit_transfer()
{
    if (transfer_.path.empty()) {
        transfer_.error = "Choose a file path.";
        return;
    }
    const std::filesystem::path path = expand_user_path(transfer_.path);
    if (transfer_.is_export) {
        const SaveResult result = ProfileStore::export_file(path, library_.profiles());
        if (!result) {
            transfer_.error = "Could not export: " + result.message;
            return;
        }
        const std::size_t n = library_.size();
        return_to_list("Exported " + std::to_string(n) + " connection"
                       + (n == 1 ? "" : "s") + " to " + path.string()
                       + " (no secrets, no history).");
        return;
    }
    const ImportResult result = ProfileStore::import_file(path);
    if (!result.ok) {
        transfer_.error = "Could not import: " + result.error;
        return;
    }
    const std::size_t added = library_.import_profiles(result.profiles);
    const SaveResult saved = library_.save();
    if (!saved) {
        return_to_list("Imported, but not saved: " + saved.message, true);
        return;
    }
    std::string status = "Imported " + std::to_string(added) + " connection"
        + (added == 1 ? "" : "s") + ".";
    if (!result.skipped.empty()) {
        status += " Skipped " + std::to_string(result.skipped.size())
            + " invalid: " + result.skipped.front();
    }
    return_to_list(std::move(status), !result.skipped.empty());
}

void LibraryScreen::transfer_key(Key k)
{
    switch (k) {
    case Key::tab:
    case Key::down:
        move_focus(+1, k == Key::tab);
        break;
    case Key::back_tab:
    case Key::up:
        move_focus(-1, k == Key::back_tab);
        break;
    case Key::enter:
        if (focus_.kind == Region::Kind::transfer_path)
            commit_transfer();
        else
            activate(focus_);
        break;
    case Key::escape:
        return_to_list(transfer_.is_export ? "Export cancelled." : "Import cancelled.");
        break;
    default:
        break;
    }
}

void LibraryScreen::transfer_pointer_press(const Region& region)
{
    switch (region.kind) {
    case Region::Kind::transfer_confirm:
        commit_transfer();
        break;
    case Region::Kind::transfer_cancel:
        return_to_list(transfer_.is_export ? "Export cancelled." : "Import cancelled.");
        break;
    default:
        break;
    }
}

// --------------------------------------------------------------------------
// Rendering primitives
// --------------------------------------------------------------------------

float LibraryScreen::text_width(std::string_view text, float size, bool bold)
{
    Font font;
    font.size = size * scale_;
    if (bold)
        font.face = 0x0020; // B_BOLD_FACE
    return text_.width(text, font) / scale_;
}

float LibraryScreen::draw_text(std::string_view text, int lx, int lbaseline,
                               Color color, float size, bool bold, int lmax_width)
{
    if (text.empty())
        return 0;
    std::string shown(text);
    if (lmax_width > 0 && text_width(shown, size, bold) > static_cast<float>(lmax_width)) {
        // Elide at the end, on a code-point boundary.
        const std::string ellipsis = "\xE2\x80\xA6";
        while (!shown.empty()
               && text_width(shown + ellipsis, size, bold) > static_cast<float>(lmax_width))
            pop_utf8(shown);
        shown += ellipsis;
    }
    DrawState state;
    state.high = color;
    state.font.size = size * scale_;
    if (bold)
        state.font.face = 0x0020;
    const float advance = text_.draw(
        shown, Point {static_cast<float>(px(lx)), static_cast<float>(px(lbaseline))},
        state, surface_);
    return advance / scale_;
}

void LibraryScreen::draw_focus_ring(const Region& region)
{
    if (!focus_visible_ || !is_focused(region))
        return;
    const int off = px(ring_offset);
    round_rect(surface_, region.x - off, region.y - off, region.w + 2 * off,
               region.h + 2 * off, px(radius_control + ring_offset),
               theme::focus_ring, px(ring_thickness));
}

void LibraryScreen::draw_button(const Region& region, std::string_view label,
                                ButtonStyle style)
{
    const bool hovered = region.hit(pointer_x_, pointer_y_);
    const int r = px(radius_control);
    const int edge = std::max(1, px(1));
    Color label_color = theme::text_primary;
    switch (style) {
    case ButtonStyle::primary:
        round_rect(surface_, region.x, region.y, region.w, region.h, r, theme::accent);
        label_color = theme::on_accent;
        break;
    case ButtonStyle::danger:
        round_rect(surface_, region.x, region.y, region.w, region.h, r, theme::danger);
        label_color = theme::on_accent;
        break;
    case ButtonStyle::chip:
        bordered(surface_, region.x, region.y, region.w, region.h, r,
                 theme::accent_soft, theme::accent, edge);
        label_color = theme::accent;
        break;
    case ButtonStyle::secondary:
        bordered(surface_, region.x, region.y, region.w, region.h, r,
                 hovered ? theme::accent_soft : theme::field_background,
                 theme::border_strong, edge);
        break;
    }
    draw_focus_ring(region);

    const float size = 13;
    const bool bold = style != ButtonStyle::secondary;
    const float label_w = text_width(label, size, bold) * scale_;
    const int tx = region.x + std::max(px(8), (region.w - static_cast<int>(label_w)) / 2);
    const int baseline = region.y + region.h / 2 + px(5);
    DrawState state;
    state.high = label_color;
    state.font.size = size * scale_;
    if (bold)
        state.font.face = 0x0020;
    text_.draw(label, Point {static_cast<float>(tx), static_cast<float>(baseline)},
               state, surface_);
}

void LibraryScreen::draw_toggle(const Region& region, std::string_view label, bool on)
{
    // A checkbox: an accent-filled square with a white check when on.
    const int box = px(18);
    const int bx = region.x + px(2);
    const int by = region.y + (region.h - box) / 2;
    if (on) {
        round_rect(surface_, bx, by, box, box, px(4), theme::accent);
        const float s = scale_;
        const Point a {static_cast<float>(bx) + 4 * s, static_cast<float>(by) + 9 * s};
        const Point b {static_cast<float>(bx) + 7.5f * s, static_cast<float>(by) + 12.5f * s};
        const Point c {static_cast<float>(bx) + 14 * s, static_cast<float>(by) + 5.5f * s};
        DrawState pen;
        pen.pen_size = 2 * s;
        pen.high = theme::on_accent;
        const Point check[3] = {a, b, c};
        surface_.stroke_polyline(check, pen);
    } else {
        bordered(surface_, bx, by, box, box, px(4), theme::field_background,
                 theme::border_strong, std::max(1, px(1)));
    }
    draw_focus_ring(region);
    DrawState state;
    state.high = theme::text_primary;
    state.font.size = 13 * scale_;
    text_.draw(label,
               Point {static_cast<float>(bx + box + px(8)),
                      static_cast<float>(region.y + region.h / 2 + px(5))},
               state, surface_);
}

void LibraryScreen::draw_field(const Region& region, std::string_view label,
                               std::string_view value, std::string_view placeholder,
                               bool focused, std::string_view error)
{
    const bool bad = !error.empty();
    const int edge = std::max(1, px(1));
    // Label above the box.
    {
        DrawState state;
        state.high = bad ? theme::danger : theme::text_muted;
        state.font.size = 12 * scale_;
        state.font.face = 0x0020;
        text_.draw(label, Point {static_cast<float>(region.x),
                                 static_cast<float>(region.y - px(6))},
                   state, surface_);
    }
    const Color edge_color = bad ? theme::danger : (focused ? theme::accent : theme::border_strong);
    // A focused field gets a 2-unit border, so focus does not rely on colour.
    bordered(surface_, region.x, region.y, region.w, region.h, px(radius_control),
             theme::field_background, edge_color, focused ? px(2) : edge);
    draw_focus_ring(region);

    draw_text_value(region, value, placeholder, focused);

    if (bad) {
        DrawState err;
        err.high = theme::danger;
        err.font.size = 11 * scale_;
        std::string shown(error);
        const float max_w = static_cast<float>(region.w);
        if (text_.width(shown, err.font) > max_w) {
            const std::string ellipsis = "\xE2\x80\xA6";
            while (!shown.empty() && text_.width(shown + ellipsis, err.font) > max_w)
                pop_utf8(shown);
            shown += ellipsis;
        }
        text_.draw(shown, Point {static_cast<float>(region.x),
                                 static_cast<float>(region.y + region.h + px(13))},
                   err, surface_);
    }
}

// The inside of a text box: its value (or placeholder), and when focused the
// selection, any IME composition and the caret. One routine for every field
// -- editor, search, quick connect, import/export path -- so they all edit
// alike. The focused field scrolls horizontally to keep the caret in view; an
// unfocused one shows the start of its value.
void LibraryScreen::draw_text_value(const Region& region, std::string_view value,
                                    std::string_view placeholder, bool focused)
{
    const int inner_l = region.x + px(text_pad(region.kind));
    const int inner_r = region.x + region.w - px(text_pad(region.kind)); // exclusive
    const int baseline = region.y + region.h / 2 + px(5);
    const int band_y = region.y + px(caret_inset);
    const int band_h = region.h - 2 * px(caret_inset);
    const int caret_w = std::max(1, px(1));
    DrawState state;
    state.font.size = field_font * scale_;
    state.clip_rects.push_back(Rect {static_cast<float>(inner_l),
                                     static_cast<float>(region.y),
                                     static_cast<float>(inner_r - 1),
                                     static_cast<float>(region.y + region.h - 1)});
    state.clipping_set = true;
    const auto draw_at = [&](std::string_view text, float x, Color color) {
        if (text.empty())
            return;
        state.high = color;
        text_.draw(text, Point {x, static_cast<float>(baseline)}, state, surface_);
    };

    if (!focused) {
        if (value.empty())
            draw_at(placeholder, static_cast<float>(inner_l), theme::text_muted);
        else
            draw_at(value, static_cast<float>(inner_l), theme::text_primary);
        return;
    }

    clamp_edit();
    // What is on screen: the value, with any composition spliced in at the
    // caret (it is shown, never stored).
    const bool composing = !edit_.composition.empty();
    std::string shown(value);
    std::size_t caret_at = std::min(edit_.caret, shown.size());
    std::size_t comp_from = caret_at;
    std::size_t comp_to = caret_at;
    if (composing) {
        shown.insert(caret_at, edit_.composition);
        comp_to = comp_from + edit_.composition.size();
        caret_at = edit_.composition_cursor < 0
            ? comp_to
            : comp_from + cp_bytes(edit_.composition, edit_.composition_cursor);
    }
    const auto width_to = [&](std::size_t bytes) {
        return text_.width(std::string_view(shown).substr(0, bytes), state.font);
    };
    const int caret_px = static_cast<int>(std::lround(width_to(caret_at)));
    const int total_px = static_cast<int>(std::ceil(width_to(shown.size())));
    // Horizontal scroll: the caret stays inside the box, and no empty space
    // opens at the right while there is text scrolled off the left.
    const int room = std::max(0, inner_r - inner_l - caret_w);
    int& hscroll = edit_.hscroll;
    if (caret_px - hscroll > room)
        hscroll = caret_px - room;
    if (caret_px - hscroll < 0)
        hscroll = caret_px;
    if (total_px - hscroll < room)
        hscroll = std::max(0, total_px - room);
    const float origin = static_cast<float>(inner_l - hscroll);

    if (shown.empty()) {
        draw_at(placeholder, static_cast<float>(inner_l), theme::text_muted);
    } else if (has_selection() && !composing) {
        // Selected text is on_accent over an accent band (muted when the
        // window is inactive); the rest stays text_primary. Drawn as three
        // runs so no glyph is painted twice.
        const std::size_t from = std::min(edit_.caret, edit_.anchor);
        const std::size_t to = std::max(edit_.caret, edit_.anchor);
        const float x_from = origin + width_to(from);
        const float x_to = origin + width_to(to);
        const int band_l = std::max(inner_l, static_cast<int>(std::lround(x_from)));
        const int band_r = std::min(inner_r, static_cast<int>(std::lround(x_to)));
        fill_px(surface_, band_l, band_y, band_r - band_l, band_h,
                window_active_ ? theme::accent : theme::accent_soft);
        const std::string_view all(shown);
        draw_at(all.substr(0, from), origin, theme::text_primary);
        draw_at(all.substr(from, to - from), x_from,
                window_active_ ? theme::on_accent : theme::text_primary);
        draw_at(all.substr(to), x_to, theme::text_primary);
    } else {
        draw_at(shown, origin, theme::text_primary);
    }
    if (composing) {
        // The composition is underlined: it is not text yet.
        const int ul_l = std::max(inner_l, static_cast<int>(std::lround(origin + width_to(comp_from))));
        const int ul_r = std::min(inner_r, static_cast<int>(std::lround(origin + width_to(comp_to))));
        fill_px(surface_, ul_l, baseline + px(2), ul_r - ul_l, std::max(1, px(1)),
                theme::accent);
    }

    const int caret_x = std::clamp(inner_l + caret_px - hscroll, inner_l,
                                   std::max(inner_l, inner_r - caret_w));
    caret_rect_ = region;
    caret_rect_.x = caret_x;
    caret_rect_.w = caret_w;
    const bool on = caret_on();
    rendered_caret_phase_ = on ? 0 : 1;
    if (on && (!has_selection() || composing))
        fill_px(surface_, caret_x, band_y, caret_w, band_h, theme::accent);
}

// The route as hops joined by drawn arrows, so direct and tunnel routes look
// different at a glance (issue #1, Visual direction). The full form puts each
// hop in a chip (the editor's preview); the compact form is one line of muted
// text for a card, with `ly` as its baseline. Arrows are geometry because
// U+2192 is missing from common UI fonts and would render as nothing.
void LibraryScreen::draw_route(const ConnectionProfile& profile, int lx, int ly, int lw,
                               bool compact)
{
    const std::string route = profile.route_summary();
    std::vector<std::string> hops;
    std::size_t start = 0;
    for (;;) {
        const std::size_t arrow = route.find(" -> ", start);
        hops.push_back(route.substr(start, arrow - start));
        if (arrow == std::string::npos)
            break;
        start = arrow + 4;
    }
    const int chip_h = 26;
    const int arrow_w = compact ? 18 : 22;
    const int pad = compact ? 0 : 10;
    const float size = 12;
    int x = lx;
    for (std::size_t i = 0; i < hops.size(); ++i) {
        const bool last = i + 1 == hops.size();
        const int text_w = static_cast<int>(std::ceil(text_width(hops[i], size, false)));
        int item_w = text_w + 2 * pad;
        if (x + item_w > lx + lw)
            item_w = std::max(0, lx + lw - x);
        if (item_w <= 2 * pad + 8)
            break;
        if (compact) {
            draw_text(hops[i], x, ly, theme::text_muted, size, false, item_w);
        } else {
            round_rect(surface_, px(x), px(ly), px(x + item_w) - px(x),
                       px(ly + chip_h) - px(ly), px(13),
                       last ? theme::accent : theme::accent_soft);
            draw_text(hops[i], x + pad, ly + 17, last ? theme::on_accent : theme::accent,
                      size, last, item_w - 2 * pad);
        }
        x += item_w;
        if (last || x + arrow_w > lx + lw)
            break;
        // A shaft and a two-stroke head.
        const float y = compact ? static_cast<float>(px(ly)) - 4.0f * scale_
                                : static_cast<float>(px(ly + chip_h / 2));
        const float x0 = static_cast<float>(px(x + 5));
        const float x1 = static_cast<float>(px(x + arrow_w - 5));
        DrawState pen;
        pen.pen_size = std::max(1.0f, (compact ? 1.0f : 1.5f) * scale_);
        pen.high = theme::text_muted;
        const Point shaft[2] = {{x0, y}, {x1, y}};
        surface_.stroke_polyline(shaft, pen);
        const float head = (compact ? 3.0f : 4.0f) * scale_;
        const Point tip[3] = {{x1 - head, y - head}, {x1, y}, {x1 - head, y + head}};
        surface_.stroke_polyline(tip, pen);
        x += arrow_w;
    }
}

const Surface& LibraryScreen::render()
{
    regions_.clear();
    caret_rect_ = Region {};
    rendered_caret_phase_ = -1;
    surface_.clear(theme::background);
    if (view_ == View::list)
        render_list();
    else if (view_ == View::editor)
        render_editor();
    else
        render_transfer();
    hovered_ = -1;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        if (regions_[i].hit(pointer_x_, pointer_y_)) {
            hovered_ = static_cast<int>(i);
            break;
        }
    }
    dirty_ = false;
    return surface_;
}

// --------------------------------------------------------------------------
// List rendering
// --------------------------------------------------------------------------

void LibraryScreen::render_list()
{
    using Kind = Region::Kind;
    const int logical_w = static_cast<int>(static_cast<float>(surface_.width()) / scale_);
    const int logical_h = static_cast<int>(static_cast<float>(surface_.height()) / scale_);
    const int content_w = std::min(logical_w - 2 * margin, max_content_width);
    const int left = (logical_w - content_w) / 2;
    const int right = left + content_w;

    // Header: title, status, and the library-wide actions on the right.
    {
        const int new_w = 176;
        const int io_w = 92;
        const int new_x = right - new_w;
        const int export_x = new_x - button_gap - io_w;
        const int import_x = export_x - button_gap - io_w;
        Region import_button = make_region(Kind::import_profiles, import_x,
                                           header_button_y, io_w, header_button_h);
        Region export_button = make_region(Kind::export_profiles, export_x,
                                           header_button_y, io_w, header_button_h);
        Region new_button = make_region(Kind::new_connection, new_x, header_button_y,
                                        new_w, header_button_h);
        draw_text("DEBEOS REMOTE", left, 34, theme::accent, 11, true);
        draw_text("Connections", left, 66, theme::text_primary, 28, true);
        draw_button(import_button, "Import\xE2\x80\xA6", ButtonStyle::secondary);
        draw_button(export_button, "Export\xE2\x80\xA6", ButtonStyle::secondary);
        draw_button(new_button, "+  New connection", ButtonStyle::primary);
        add_region(import_button);
        add_region(export_button);
        add_region(new_button);
        draw_text(status_, left, 92, status_error_ ? theme::danger : theme::text_muted,
                  13, status_error_, content_w);
    }

    // Quick connect: connect to a host without saving a profile.
    {
        bordered(surface_, px(left), px(quick_top), px(right) - px(left),
                 px(quick_top + quick_h) - px(quick_top), px(radius_card),
                 theme::panel, theme::border, std::max(1, px(1)));
        draw_text("Quick connect", left + 18, quick_top + 38, theme::text_primary, 14,
                  true);
        const int control_y = quick_top + 14;
        const int control_h = 36;
        const int options_w = 128;
        const int connect_w = 104;
        const int mode_w = 150;
        const int options_x = right - 14 - options_w;
        const int connect_x = options_x - button_gap - connect_w;
        const int mode_x = connect_x - button_gap - mode_w;
        const int host_x = left + 140;
        const int host_w = mode_x - button_gap - host_x;
        Region host = make_region(Kind::quick_host, host_x, control_y, host_w, control_h);
        Region mode = make_region(Kind::quick_mode, mode_x, control_y, mode_w, control_h);
        Region connect = make_region(Kind::quick_connect, connect_x, control_y, connect_w,
                                     control_h);
        Region options = make_region(Kind::quick_options, options_x, control_y,
                                     options_w, control_h);
        // The field draws its own label above; the panel title stands in.
        const bool host_focused = focus_.kind == Kind::quick_host;
        bordered(surface_, host.x, host.y, host.w, host.h, px(radius_control),
                 theme::field_background,
                 host_focused ? theme::accent : theme::border_strong,
                 host_focused ? px(2) : std::max(1, px(1)));
        draw_focus_ring(host);
        draw_text_value(host, quick_host_, "user@host or host", host_focused);
        draw_button(mode, mode_label(quick_mode_), ButtonStyle::chip);
        draw_button(connect, "Connect", ButtonStyle::primary);
        draw_button(options, "More options\xE2\x80\xA6", ButtonStyle::secondary);
        add_region(host);
        add_region(mode);
        add_region(connect);
        add_region(options);
    }

    // Search.
    {
        Region search = make_region(Kind::search, left, search_top, content_w, search_h);
        const bool focused = focus_.kind == Kind::search;
        bordered(surface_, search.x, search.y, search.w, search.h, px(radius_control),
                 theme::field_background, focused ? theme::accent : theme::border_strong,
                 focused ? px(2) : std::max(1, px(1)));
        draw_focus_ring(search);
        draw_text_value(search, search_, "Search connections by name, host or user",
                        focused);
        add_region(search);
    }

    const auto visible = library_.search(search_);
    ensure_selection_visible(visible.size());

    if (visible.empty()) {
        if (library_.empty()) {
            draw_text("No saved connections yet", left, cards_top + 40,
                      theme::text_primary, 18, true);
            draw_text("Use Quick connect above to try a host, or New connection to "
                      "save one. Import\xE2\x80\xA6 brings in an export from another "
                      "machine.",
                      left, cards_top + 66, theme::text_muted, 13, false, content_w);
        } else {
            draw_text("No connections match \xE2\x80\x9C" + search_ + "\xE2\x80\x9D.",
                      left, cards_top + 40, theme::text_muted, 15, false, content_w);
        }
        return;
    }

    const std::size_t per_page = cards_per_page();
    int y = cards_top;
    for (std::size_t i = scroll_; i < visible.size() && i < scroll_ + per_page; ++i) {
        render_card(visible[i], i, left, y, content_w);
        y += card_height + card_gap;
    }

    if (scroll_ + per_page < visible.size() || scroll_ > 0) {
        draw_text("Showing " + std::to_string(scroll_ + 1) + "\xE2\x80\x93"
                      + std::to_string(std::min(visible.size(), scroll_ + per_page))
                      + " of " + std::to_string(visible.size())
                      + "   \xC2\xB7   Up/Down to move, Tab to reach a "
                        "card\xE2\x80\x99s buttons",
                  left, logical_h - 10, theme::text_muted, 11, false, content_w);
    }
}

void LibraryScreen::render_card(const ConnectionProfile& profile, std::size_t index,
                                int x, int y, int w)
{
    using Kind = Region::Kind;
    const bool is_selected = index == selected_;
    const bool confirming = profile.id == pending_delete_;
    const int edge = std::max(1, px(1));
    bordered(surface_, px(x), px(y), px(x + w) - px(x), px(y + card_height) - px(y),
             px(radius_card),
             confirming ? theme::danger_soft
                        : (is_selected ? theme::panel_selected : theme::panel),
             confirming ? theme::danger : (is_selected ? theme::accent : theme::border),
             is_selected || confirming ? px(2) : edge);

    // A favourite gets an accent bar down its leading edge as well as a filled
    // star, so the treatment does not rest on the star's colour alone.
    if (profile.favorite && !confirming)
        fill_px(surface_, px(x) + px(2), px(y + 14), px(4), px(card_height - 28),
                theme::star_on);

    Region star = make_region(Kind::favorite_card, x + 14, y + 12, 28, 28, profile.id);
    draw_focus_ring(star);
    // The star is drawn as geometry, not a glyph: U+2605/U+2606 are missing
    // from common UI fonts and would silently render as nothing.
    draw_star(surface_, static_cast<float>(star.x) + static_cast<float>(star.w) / 2,
              static_cast<float>(star.y) + static_cast<float>(star.h) / 2,
              9.5f * scale_, profile.favorite,
              profile.favorite ? theme::star_on : theme::text_muted, scale_);

    // Actions, right-aligned.
    struct Action {
        Kind kind;
        const char* label;
        int width;
        ButtonStyle style;
    };
    std::vector<Action> actions;
    if (confirming) {
        actions = {{Kind::confirm_delete, "Delete", 92, ButtonStyle::danger},
                   {Kind::cancel_delete, "Keep", 80, ButtonStyle::secondary}};
    } else {
        actions = {{Kind::connect, "Connect", 96, ButtonStyle::primary},
                   {Kind::edit, "Edit", 68, ButtonStyle::secondary},
                   {Kind::duplicate, "Duplicate", 96, ButtonStyle::secondary},
                   {Kind::remove, "Delete", 80, ButtonStyle::secondary}};
    }
    int total = 0;
    for (const Action& a : actions)
        total += a.width;
    total += button_gap * static_cast<int>(actions.size() - 1);
    int bx = x + w - 16 - total;
    const int by = y + (card_height - button_h) / 2;
    const int text_x = x + 52;
    const int text_max = bx - 16 - text_x;

    draw_text(profile.name.empty() ? "(unnamed)" : profile.name, text_x, y + 30,
              theme::text_primary, 16, true, text_max);
    draw_route(profile, text_x, y + 50, text_max, true);
    if (confirming) {
        draw_text("Delete this connection? This cannot be undone.", text_x, y + 70,
                  theme::danger, 12, true, text_max);
    } else {
        const std::string meta = card_meta(profile, clock_());
        const float meta_w = draw_text(meta, text_x, y + 70, theme::text_muted, 12,
                                       false, text_max);
        if (!profile.last_error.empty()) {
            const int ex = text_x + static_cast<int>(std::ceil(meta_w));
            draw_text("  \xC2\xB7  " + profile.last_error, ex, y + 70, theme::danger, 12,
                      false, text_x + text_max - ex);
        }
    }

    std::vector<Region> buttons;
    for (const Action& a : actions) {
        Region button = make_region(a.kind, bx, by, a.width, button_h, profile.id);
        draw_button(button, a.label, a.style);
        buttons.push_back(std::move(button));
        bx += a.width + button_gap;
    }
    add_region(star);
    for (auto& button : buttons)
        add_region(std::move(button));
    // The body last, so a button or the star wins the hit test.
    add_region(make_region(Kind::card, x, y, w, card_height, profile.id));
}

// --------------------------------------------------------------------------
// Editor rendering
// --------------------------------------------------------------------------

LibraryScreen::EditorLayout LibraryScreen::editor_layout() const
{
    EditorLayout layout;
    const int logical_w = static_cast<int>(static_cast<float>(surface_.width()) / scale_);
    const int logical_h = static_cast<int>(static_cast<float>(surface_.height()) / scale_);
    layout.content_w = std::min(logical_w - 2 * margin, max_content_width);
    layout.left = (logical_w - layout.content_w) / 2;
    layout.right = layout.left + layout.content_w;

    const int col_gap = 24;
    const int col_w = (layout.content_w - col_gap) / 2;
    for (int i = 0; i < editor_field_count; ++i) {
        const int field = editor_tab_order[static_cast<std::size_t>(i)];
        const int column = i < editor_rows ? 0 : 1;
        const int row = i < editor_rows ? i : i - editor_rows;
        Box box {layout.left + column * (col_w + col_gap),
                 editor_form_top + row * editor_row_h + field_label_rise, col_w, field_h};
        // Width and height share the last right-hand row.
        if (field == field_width || field == field_height) {
            box.w = (col_w - 12) / 2;
            if (field == field_height) {
                box.x += box.w + 12;
                box.y = editor_form_top
                    + (tab_position(field_width) - editor_rows) * editor_row_h
                    + field_label_rise;
            }
        }
        layout.fields[static_cast<std::size_t>(field)] = box;
    }
    const int toggles_y = editor_form_top + editor_rows * editor_row_h + 4;
    layout.mode = Box {layout.left, toggles_y, 220, 34};
    layout.favorite = Box {layout.left + 240, toggles_y, 120, 34};
    layout.reconnect = Box {layout.left + 376, toggles_y, 170, 34};
    layout.route_y = toggles_y + 50;
    layout.content_bottom = layout.route_y + chip_h + 8;

    // The action bar is pinned to the bottom; the form scrolls in the band
    // between the status line and it. At 1024x700 the whole form fits, so
    // nothing scrolls there.
    layout.actions_y = logical_h - margin - editor_actions_h;
    layout.view_top = editor_view_top;
    layout.view_bottom = std::max(layout.view_top + 1, layout.actions_y - editor_view_gap);
    return layout;
}

void LibraryScreen::set_editor_scroll(int scroll, const EditorLayout& layout)
{
    editor_max_scroll_ = std::max(0, layout.content_bottom - layout.view_bottom);
    scroll = std::clamp(scroll, 0, editor_max_scroll_);
    if (scroll != editor_scroll_) {
        editor_scroll_ = scroll;
        dirty_ = true;
    }
}

// Scroll just far enough that the focused control -- a field with its label
// above and its error line below, or a toggle -- is wholly inside the band.
void LibraryScreen::reveal_editor_focus(const EditorLayout& layout)
{
    Box box;
    int above = 4;
    int below = 4;
    switch (focus_.kind) {
    case Region::Kind::editor_field:
        if (focus_.field < 0 || focus_.field >= editor_field_count)
            return;
        box = layout.fields[static_cast<std::size_t>(focus_.field)];
        above = field_label_rise;
        below = editor_row_h - field_label_rise - field_h; // the error line
        break;
    case Region::Kind::mode_cycle: box = layout.mode; break;
    case Region::Kind::favorite_toggle: box = layout.favorite; break;
    case Region::Kind::autoreconnect_toggle: box = layout.reconnect; break;
    default:
        return; // the action bar never scrolls
    }
    const int top = box.y - above;
    const int bottom = box.y + box.h + below;
    int scroll = editor_scroll_;
    if (bottom - scroll > layout.view_bottom)
        scroll = bottom - layout.view_bottom;
    if (top - scroll < layout.view_top)
        scroll = top - layout.view_top;
    set_editor_scroll(scroll, layout);
}

void LibraryScreen::render_editor()
{
    using Kind = Region::Kind;
    const EditorLayout layout = editor_layout();
    const int left = layout.left;
    const int right = layout.right;
    const int content_w = layout.content_w;

    // Settle the scroll first: clamp it to this size, and bring a newly
    // focused control into view.
    set_editor_scroll(editor_scroll_, layout);
    if (reveal_pending_) {
        reveal_pending_ = false;
        reveal_editor_focus(layout);
    }
    const int s = editor_scroll_;
    const int band_top = px(layout.view_top);
    const int band_bottom = px(layout.view_bottom);
    // Body regions are clipped to the band, so nothing hidden under the header
    // or the action bar can be clicked.
    const auto add_body_region = [&](Region region) {
        if (clip_rows(region, band_top, band_bottom))
            add_region(std::move(region));
    };

    // Messages per field: every error for that field, joined.
    const auto field_error = [&](int field) {
        std::string joined;
        for (const FieldError& e : editor_.errors) {
            if (field_for_validate_name(e.field) != field)
                continue;
            if (!joined.empty())
                joined += "; ";
            joined += e.message;
        }
        return joined;
    };

    // The scrolling form, drawn first; the header and the action bar are
    // painted over whatever of it runs outside the band.
    for (int field : editor_tab_order) {
        const Box& box = layout.fields[static_cast<std::size_t>(field)];
        Region region = make_region(Kind::editor_field, box.x, box.y - s, box.w, box.h, {},
                                    field);
        std::string label = field_label(field);
        if ((field == field_ssh_port || field == field_ssh_user
             || field == field_local_port || field == field_identity_file
             || field == field_known_hosts_file)
            && editor_.mode != ConnectionMode::ssh)
            label += " (SSH only)";
        const bool focused = focus_.kind == Kind::editor_field && focus_.field == field;
        draw_field(region, label, editor_.text[static_cast<std::size_t>(field)],
                   field_placeholder(field), focused, field_error(field));
        add_body_region(std::move(region));
    }
    {
        Region mode = make_region(Kind::mode_cycle, layout.mode.x, layout.mode.y - s,
                                  layout.mode.w, layout.mode.h);
        draw_button(mode, "Mode: " + mode_label(editor_.mode), ButtonStyle::chip);
        add_body_region(mode);
        Region fav = make_region(Kind::favorite_toggle, layout.favorite.x,
                                 layout.favorite.y - s, layout.favorite.w,
                                 layout.favorite.h);
        draw_toggle(fav, "Favorite", editor_.favorite);
        add_body_region(fav);
        Region reconnect = make_region(Kind::autoreconnect_toggle, layout.reconnect.x,
                                       layout.reconnect.y - s, layout.reconnect.w,
                                       layout.reconnect.h);
        draw_toggle(reconnect, "Auto-reconnect", editor_.auto_reconnect);
        add_body_region(reconnect);
    }
    draw_text("ROUTE", left, layout.route_y - s + 17, theme::text_muted, 11, true);
    draw_route(preview_profile(), left + 64, layout.route_y - s, content_w - 64);

    // A caret scrolled under the header or the action bar is not on screen.
    if (caret_rect_.w > 0
        && (caret_rect_.y < band_top || caret_rect_.y + caret_rect_.h > band_bottom))
        caret_rect_ = Region {};

    // Header.
    fill_px(surface_, 0, 0, surface_.width(), band_top, theme::background);
    const char* title = editor_.quick ? "Quick connect"
                      : editor_.is_new ? "New connection"
                                       : "Edit connection";
    draw_text("DEBEOS REMOTE", left, 34, theme::accent, 11, true);
    draw_text(title, left, 66, theme::text_primary, 28, true);
    draw_text(status_, left, 92, status_error_ ? theme::danger : theme::text_muted, 13,
              status_error_, content_w);

    // Action bar.
    fill_px(surface_, 0, band_bottom, surface_.width(), surface_.height() - band_bottom,
            theme::background);
    const int edge = std::max(1, px(1));
    // A hairline marks each edge that has more form beyond it.
    if (s > 0)
        fill_px(surface_, px(left), band_top, px(right) - px(left), edge, theme::border);
    if (s < editor_max_scroll_)
        fill_px(surface_, px(left), band_bottom - edge, px(right) - px(left), edge,
                theme::border);
    if (editor_max_scroll_ > 0) {
        // A scroll indicator in the right margin: the band is to the form as
        // the thumb is to the track.
        const int track = layout.view_bottom - layout.view_top;
        const int content = track + editor_max_scroll_;
        const int thumb = std::max(scrollbar_min, track * track / content);
        const int thumb_y = layout.view_top + (track - thumb) * s / editor_max_scroll_;
        round_rect(surface_, px(right + 12), px(thumb_y),
                   px(right + 12 + scrollbar_w) - px(right + 12),
                   px(thumb_y + thumb) - px(thumb_y), px(scrollbar_w / 2),
                   theme::border_strong);
    }
    const int actions_y = layout.actions_y;
    if (editor_.quick) {
        Region connect = make_region(Kind::connect_now, left, actions_y, 128, editor_actions_h);
        draw_button(connect, "Connect", ButtonStyle::primary);
        add_region(connect);
    } else {
        Region save = make_region(Kind::save, left, actions_y, 128, editor_actions_h);
        draw_button(save, "Save", ButtonStyle::primary);
        add_region(save);
    }
    Region both = make_region(Kind::save_and_connect, left + 140, actions_y, 160,
                              editor_actions_h);
    draw_button(both, "Save & connect", ButtonStyle::secondary);
    add_region(both);
    Region cancel = make_region(Kind::cancel, right - 120, actions_y, 120, editor_actions_h);
    draw_button(cancel, "Cancel", ButtonStyle::secondary);
    add_region(cancel);
}

// --------------------------------------------------------------------------
// Import / export rendering
// --------------------------------------------------------------------------

void LibraryScreen::render_transfer()
{
    using Kind = Region::Kind;
    const int logical_w = static_cast<int>(static_cast<float>(surface_.width()) / scale_);
    const int content_w = std::min(logical_w - 2 * margin, max_content_width);
    const int left = (logical_w - content_w) / 2;
    const bool is_export = transfer_.is_export;

    draw_text("DEBEOS REMOTE", left, 34, theme::accent, 11, true);
    draw_text(is_export ? "Export connections" : "Import connections", left, 66,
              theme::text_primary, 28, true);
    if (is_export) {
        draw_text("Writes every saved connection to a file you can copy to another "
                  "machine.",
                  left, 100, theme::text_muted, 13, false, content_w);
        draw_text("No passwords, keys or cookies are included \xE2\x80\x94 only the paths "
                  "to them. Connection history is left out.",
                  left, 120, theme::text_muted, 13, false, content_w);
    } else {
        draw_text("Adds the connections in an export file. Existing connections are "
                  "never replaced.",
                  left, 100, theme::text_muted, 13, false, content_w);
        draw_text("Invalid entries are skipped and named. History and unknown fields "
                  "are not imported.",
                  left, 120, theme::text_muted, 13, false, content_w);
    }
    Region path = make_region(Kind::transfer_path, left, 160, content_w, 36);
    draw_field(path, "File", transfer_.path, "~/haiku-remote-connections.json",
               focus_.kind == Kind::transfer_path, transfer_.error);
    add_region(path);

    Region confirm = make_region(Kind::transfer_confirm, left, 230, 128, 38);
    draw_button(confirm, is_export ? "Export" : "Import", ButtonStyle::primary);
    add_region(confirm);
    Region cancel = make_region(Kind::transfer_cancel, left + 140, 230, 120, 38);
    draw_button(cancel, "Cancel", ButtonStyle::secondary);
    add_region(cancel);
}

} // namespace haiku_remote
