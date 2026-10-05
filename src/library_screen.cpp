#include "haiku_remote/library_screen.hpp"

#include "haiku_remote/profile_library.hpp"

#include <algorithm>
#include <cctype>

namespace haiku_remote {

namespace {

// Dark theme, chosen to read against app_server's own grey desktop without a
// theme engine. Plain structs so the palette is a glance, not a lookup.
constexpr Color bg {24, 26, 32, 255};
constexpr Color panel {36, 39, 48, 255};
constexpr Color panel_selected {50, 56, 74, 255};
constexpr Color field_bg {28, 30, 38, 255};
constexpr Color border {70, 76, 92, 255};
constexpr Color accent {74, 144, 226, 255};
constexpr Color text_primary {234, 237, 242, 255};
constexpr Color text_muted {150, 156, 170, 255};
constexpr Color danger {214, 92, 92, 255};
constexpr Color star_on {240, 200, 80, 255};
constexpr Color white {255, 255, 255, 255};

constexpr int margin = 24;
constexpr int card_height = 92;
constexpr int card_gap = 10;
constexpr int cards_top = 132;

constexpr int button_w = 86;
constexpr int button_h = 28;
constexpr int button_gap = 8;

int parse_int(const std::string& text)
{
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(text, &consumed);
        if (consumed != text.size())
            return 0;
        return value;
    } catch (...) {
        return 0;
    }
}

// Map a validate() field name back to the editor field it highlights.
int field_for_validate_name(std::string_view name)
{
    if (name == "name")
        return LibraryScreen::field_name;
    if (name == "host")
        return LibraryScreen::field_host;
    if (name == "remotePort")
        return LibraryScreen::field_remote_port;
    if (name == "sshPort")
        return LibraryScreen::field_ssh_port;
    if (name == "width")
        return LibraryScreen::field_width;
    if (name == "height")
        return LibraryScreen::field_height;
    return -1;
}

const char* field_label(int field)
{
    switch (field) {
    case LibraryScreen::field_name: return "Name";
    case LibraryScreen::field_host: return "Host";
    case LibraryScreen::field_ssh_user: return "SSH user";
    case LibraryScreen::field_remote_port: return "Remote port";
    case LibraryScreen::field_ssh_port: return "SSH port";
    case LibraryScreen::field_identity_file: return "Identity file";
    case LibraryScreen::field_known_hosts_file: return "Known hosts file";
    case LibraryScreen::field_cookie_source: return "Cookie source";
    case LibraryScreen::field_width: return "Width";
    case LibraryScreen::field_height: return "Height";
    default: return "";
    }
}

} // namespace

LibraryScreen::LibraryScreen(ConnectionLibrary& library, int width, int height)
    : library_(library), surface_(std::max(width, 480), std::max(height, 320))
{
    const std::size_t count = library_.size();
    status_ = count == 0
                  ? "No saved connections yet. Choose New connection to add one."
                  : std::to_string(count) + " saved connection"
                        + (count == 1 ? "" : "s") + ".";
}

void LibraryScreen::resize(int width, int height)
{
    width = std::max(width, 480);
    height = std::max(height, 320);
    if (width == surface_.width() && height == surface_.height())
        return;
    surface_ = Surface(width, height);
    dirty_ = true;
}

float LibraryScreen::draw_text(std::string_view text, int x, int baseline,
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

void LibraryScreen::add_region(Region region)
{
    regions_.push_back(std::move(region));
}

std::optional<ConnectionProfile> LibraryScreen::take_connect_request()
{
    auto request = std::move(connect_request_);
    connect_request_.reset();
    return request;
}

// --------------------------------------------------------------------------
// Input
// --------------------------------------------------------------------------

void LibraryScreen::pointer_move(int x, int y)
{
    pointer_x_ = x;
    pointer_y_ = y;
}

void LibraryScreen::pointer_press(int x, int y)
{
    pointer_x_ = x;
    pointer_y_ = y;
    for (const Region& region : regions_) {
        if (!region.hit(x, y))
            continue;
        if (view_ == View::list)
            list_pointer_press(region);
        else
            editor_pointer_press(region);
        dirty_ = true;
        return;
    }
}

void LibraryScreen::text_input(std::string_view utf8)
{
    if (utf8.empty())
        return;
    if (view_ == View::list) {
        // Printable text filters the list.
        for (char c : utf8) {
            if (static_cast<unsigned char>(c) >= 0x20)
                search_.push_back(c);
        }
        selected_ = 0;
        scroll_ = 0;
    } else {
        editor_text(utf8);
    }
    dirty_ = true;
}

void LibraryScreen::key(Key k)
{
    if (k == Key::none)
        return;
    if (view_ == View::list) {
        switch (k) {
        case Key::backspace:
        case Key::del:
            if (!search_.empty())
                search_.pop_back();
            selected_ = 0;
            scroll_ = 0;
            break;
        case Key::up:
            if (selected_ > 0)
                --selected_;
            break;
        case Key::down:
            ++selected_; // clamped at render against the visible count
            break;
        case Key::enter: {
            const auto visible = library_.search(search_);
            if (selected_ < visible.size())
                connect_profile(visible[selected_].id);
            break;
        }
        case Key::escape:
            if (!search_.empty())
                search_.clear();
            break;
        default:
            break;
        }
    } else {
        editor_key(k);
    }
    dirty_ = true;
}

// --------------------------------------------------------------------------
// List view
// --------------------------------------------------------------------------

void LibraryScreen::begin_new()
{
    editor_ = EditorState {};
    editor_.is_new = true;
    editor_.mode = ConnectionMode::ssh;
    editor_.favorite = false;
    editor_.auto_reconnect = true;
    editor_.text[field_name] = "New connection";
    editor_.text[field_ssh_user] = "baron";
    editor_.text[field_remote_port] = "10900";
    editor_.text[field_ssh_port] = "22";
    editor_.text[field_width] = "1280";
    editor_.text[field_height] = "800";
    editor_.focus = field_name;
    view_ = View::editor;
    status_ = "Editing a new connection.";
}

void LibraryScreen::begin_edit(const ConnectionProfile& profile)
{
    editor_ = EditorState {};
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
    editor_.text[field_identity_file] = profile.identity_file;
    editor_.text[field_known_hosts_file] = profile.known_hosts_file;
    editor_.text[field_cookie_source] = profile.cookie_source;
    editor_.text[field_width] = std::to_string(profile.width);
    editor_.text[field_height] = std::to_string(profile.height);
    editor_.focus = field_name;
    view_ = View::editor;
    status_ = "Editing '" + profile.name + "'.";
}

void LibraryScreen::connect_profile(const std::string& id)
{
    const ConnectionProfile* profile = library_.find(id);
    if (profile == nullptr)
        return;
    connect_request_ = *profile;
    status_ = "Connecting to '" + profile->name + "'...";
}

void LibraryScreen::list_pointer_press(const Region& region)
{
    switch (region.kind) {
    case Region::Kind::new_connection:
        begin_new();
        break;
    case Region::Kind::connect:
        connect_profile(region.profile_id);
        break;
    case Region::Kind::edit:
        if (const ConnectionProfile* p = library_.find(region.profile_id))
            begin_edit(*p);
        break;
    case Region::Kind::duplicate:
        if (auto copy = library_.duplicate(region.profile_id)) {
            const SaveResult saved = library_.save();
            status_ = saved ? "Duplicated to '" + copy->name + "'."
                            : "Duplicate not saved: " + saved.message;
        }
        break;
    case Region::Kind::remove:
        if (library_.find(region.profile_id) != nullptr) {
            library_.remove(region.profile_id);
            const SaveResult saved = library_.save();
            status_ = saved ? "Deleted connection."
                            : "Delete not saved: " + saved.message;
            selected_ = 0;
        }
        break;
    case Region::Kind::favorite_card:
        if (const ConnectionProfile* p = library_.find(region.profile_id)) {
            ConnectionProfile updated = *p;
            updated.favorite = !updated.favorite;
            library_.update(updated);
            const SaveResult saved = library_.save();
            if (!saved)
                status_ = "Favorite not saved: " + saved.message;
        }
        break;
    case Region::Kind::search:
        // The search box is always collecting text in list view; a click is a
        // no-op beyond keeping focus there.
        break;
    default:
        break;
    }
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

    const int viewport = surface_.height() - cards_top - margin;
    const auto per_page = static_cast<std::size_t>(
        std::max(1, viewport / (card_height + card_gap)));
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

bool LibraryScreen::field_is_numeric(int field) const
{
    return field == field_remote_port || field == field_ssh_port
        || field == field_width || field == field_height;
}

ConnectionProfile LibraryScreen::build_editor_profile() const
{
    ConnectionProfile profile;
    profile.id = editor_.id;
    profile.name = editor_.text[field_name];
    profile.host = editor_.text[field_host];
    profile.ssh_user = editor_.text[field_ssh_user];
    profile.remote_port = parse_int(editor_.text[field_remote_port]);
    profile.ssh_port = parse_int(editor_.text[field_ssh_port]);
    profile.identity_file = editor_.text[field_identity_file];
    profile.known_hosts_file = editor_.text[field_known_hosts_file];
    profile.cookie_source = editor_.text[field_cookie_source];
    profile.width = parse_int(editor_.text[field_width]);
    profile.height = parse_int(editor_.text[field_height]);
    profile.mode = editor_.mode;
    profile.favorite = editor_.favorite;
    profile.auto_reconnect = editor_.auto_reconnect;
    // Preserve the history fields the editor does not expose.
    if (const ConnectionProfile* existing = library_.find(editor_.id)) {
        profile.last_connected_at = existing->last_connected_at;
        profile.last_error = existing->last_error;
        profile.local_port = existing->local_port;
    }
    return profile;
}

void LibraryScreen::commit_editor()
{
    ConnectionProfile profile = build_editor_profile();
    const ValidationResult validation = validate(profile);
    if (!validation.ok()) {
        const FieldError& first = validation.errors.front();
        editor_.error = first.field + ": " + first.message;
        editor_.error_field = first.field;
        const int bad = field_for_validate_name(first.field);
        if (bad >= 0)
            editor_.focus = bad;
        status_ = "Could not save: " + editor_.error;
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
        editor_.error = saved.message;
        return;
    }
    status_ = (is_new ? "Added '" : "Saved '") + profile.name + "'.";
    editor_.error.clear();
    editor_.error_field.clear();
    view_ = View::list;
}

void LibraryScreen::editor_text(std::string_view utf8)
{
    std::string& value = editor_.text[static_cast<std::size_t>(editor_.focus)];
    const bool numeric = field_is_numeric(editor_.focus);
    for (char c : utf8) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20)
            continue;
        if (numeric && !std::isdigit(byte))
            continue;
        value.push_back(c);
    }
}

void LibraryScreen::editor_key(Key k)
{
    switch (k) {
    case Key::backspace:
    case Key::del: {
        std::string& value = editor_.text[static_cast<std::size_t>(editor_.focus)];
        if (!value.empty())
            value.pop_back();
        break;
    }
    case Key::tab:
        editor_.focus = (editor_.focus + 1) % editor_field_count;
        break;
    case Key::back_tab:
        editor_.focus = (editor_.focus + editor_field_count - 1)
            % editor_field_count;
        break;
    case Key::up:
        if (editor_.focus > 0)
            --editor_.focus;
        break;
    case Key::down:
        if (editor_.focus < editor_field_count - 1)
            ++editor_.focus;
        break;
    case Key::enter:
        commit_editor();
        break;
    case Key::escape:
        view_ = View::list;
        status_ = "Edit cancelled.";
        break;
    default:
        break;
    }
}

void LibraryScreen::editor_pointer_press(const Region& region)
{
    switch (region.kind) {
    case Region::Kind::editor_field:
        editor_.focus = region.field;
        break;
    case Region::Kind::mode_cycle:
        editor_.mode = editor_.mode == ConnectionMode::direct
                           ? ConnectionMode::ssh
                       : editor_.mode == ConnectionMode::ssh
                           ? ConnectionMode::wss
                           : ConnectionMode::direct;
        break;
    case Region::Kind::favorite_toggle:
        editor_.favorite = !editor_.favorite;
        break;
    case Region::Kind::autoreconnect_toggle:
        editor_.auto_reconnect = !editor_.auto_reconnect;
        break;
    case Region::Kind::save:
        commit_editor();
        break;
    case Region::Kind::cancel:
        view_ = View::list;
        status_ = "Edit cancelled.";
        break;
    default:
        break;
    }
}

// --------------------------------------------------------------------------
// Rendering
// --------------------------------------------------------------------------

namespace {

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

} // namespace

void LibraryScreen::draw_button(const Region& region, std::string_view label,
                                bool primary)
{
    fill(surface_, region.x, region.y, region.w, region.h,
         primary ? accent : panel);
    frame(surface_, region.x, region.y, region.w, region.h,
          primary ? accent : border);
    DrawState measure;
    measure.font.size = 13;
    const float advance = text_.width(label, measure.font);
    const int tx = region.x + std::max(8, (region.w - static_cast<int>(advance)) / 2);
    const int baseline = region.y + region.h * 2 / 3 + 2;
    draw_text(label, tx, baseline, primary ? white : text_primary, 13);
}

void LibraryScreen::draw_field(int field, int x, int y, int w, int h,
                               std::string_view label, std::string_view value,
                               bool focused, bool bad)
{
    draw_text(label, x, y - 6, text_muted, 12);
    fill(surface_, x, y, w, h, field_bg);
    frame(surface_, x, y, w, h, bad ? danger : (focused ? accent : border));
    const int baseline = y + h * 2 / 3 + 2;
    std::string shown(value);
    if (focused)
        shown.push_back('_'); // a plain caret; good enough without blink timing
    draw_text(shown, x + 8, baseline, text_primary, 13);
    add_region(Region {Region::Kind::editor_field, {}, field, x, y, w, h});
}

const Surface& LibraryScreen::render()
{
    regions_.clear();
    surface_.clear(bg);
    if (view_ == View::list)
        render_list();
    else
        render_editor();
    dirty_ = false;
    return surface_;
}

void LibraryScreen::render_list()
{
    const int content_w = surface_.width() - 2 * margin;

    draw_text("DeBeOS Remote \xE2\x80\x94 Connections", margin, 44,
              text_primary, 24);
    draw_text(status_, margin, 70, text_muted, 13);

    // New connection button, top-right.
    Region new_button {Region::Kind::new_connection, {}, -1,
                       surface_.width() - margin - 170, 28, 170, 36};
    add_region(new_button);
    draw_button(new_button, "+  New connection", true);

    // Search box.
    const int search_y = 88;
    Region search {Region::Kind::search, {}, -1, margin, search_y, content_w, 32};
    add_region(search);
    fill(surface_, search.x, search.y, search.w, search.h, field_bg);
    frame(surface_, search.x, search.y, search.w, search.h, border);
    const std::string search_label =
        search_.empty() ? std::string("Search connections by name, host or user")
                         : search_;
    draw_text(search_label, search.x + 10, search.y + 22,
              search_.empty() ? text_muted : text_primary, 13);

    const auto visible = library_.search(search_);
    ensure_selection_visible(visible.size());

    if (visible.empty()) {
        draw_text(library_.empty()
                      ? "No connections yet \xE2\x80\x94 add one with New connection."
                      : "No connections match the search.",
                  margin, cards_top + 30, text_muted, 15);
        return;
    }

    const int viewport = surface_.height() - cards_top - margin;
    const auto per_page = static_cast<std::size_t>(
        std::max(1, viewport / (card_height + card_gap)));

    int y = cards_top;
    for (std::size_t i = scroll_;
         i < visible.size() && i < scroll_ + per_page; ++i) {
        const ConnectionProfile& profile = visible[i];
        const bool is_selected = i == selected_;
        fill(surface_, margin, y, content_w, card_height,
             is_selected ? panel_selected : panel);
        frame(surface_, margin, y, content_w, card_height,
              is_selected ? accent : border);

        // Favorite star (clickable).
        Region star {Region::Kind::favorite_card, profile.id, -1,
                     margin + 10, y + 10, 26, 26};
        add_region(star);
        draw_text(profile.favorite ? "\xE2\x98\x85" : "\xE2\x98\x86", star.x,
                  star.y + 21, profile.favorite ? star_on : text_muted, 20);

        const int text_x = margin + 46;
        draw_text(profile.name.empty() ? "(unnamed)" : profile.name, text_x,
                  y + 28, text_primary, 17);
        draw_text(profile.route_summary(), text_x, y + 50, text_muted, 12);

        std::string meta = std::string(mode_name(profile.mode)) + "  \xC2\xB7  "
            + (profile.host.empty() ? "(no host)" : profile.host) + ":"
            + std::to_string(profile.remote_port);
        if (profile.last_connected_at == 0)
            meta += "  \xC2\xB7  never connected";
        if (!profile.last_error.empty())
            meta += "  \xC2\xB7  " + profile.last_error;
        draw_text(meta, text_x, y + 72,
                  profile.last_error.empty() ? text_muted : danger, 12);

        // Action buttons, right-aligned.
        const std::array<std::pair<Region::Kind, const char*>, 4> actions = {{
            {Region::Kind::connect, "Connect"},
            {Region::Kind::edit, "Edit"},
            {Region::Kind::duplicate, "Duplicate"},
            {Region::Kind::remove, "Delete"},
        }};
        int bx = surface_.width() - margin - 12
            - static_cast<int>(actions.size()) * (button_w + button_gap)
            + button_gap;
        const int by = y + (card_height - button_h) / 2;
        for (const auto& [kind, label] : actions) {
            Region button {kind, profile.id, -1, bx, by, button_w, button_h};
            add_region(button);
            draw_button(button, label, kind == Region::Kind::connect);
            bx += button_w + button_gap;
        }

        y += card_height + card_gap;
    }

    if (scroll_ + per_page < visible.size() || scroll_ > 0) {
        draw_text("Showing " + std::to_string(scroll_ + 1) + "\xE2\x80\x93"
                      + std::to_string(std::min(visible.size(),
                                                scroll_ + per_page))
                      + " of " + std::to_string(visible.size())
                      + "  (arrow keys to scroll)",
                  margin, surface_.height() - 8, text_muted, 11);
    }
}

void LibraryScreen::render_editor()
{
    const int content_w = surface_.width() - 2 * margin;

    draw_text(editor_.is_new ? "New connection" : "Edit connection", margin, 44,
              text_primary, 24);
    draw_text("Tab moves between fields \xE2\x80\xA2 Enter saves \xE2\x80\xA2 "
              "Esc cancels",
              margin, 70, text_muted, 13);

    const int form_top = 104;
    const int row_h = 58;
    const int col_gap = 24;
    const int col_w = (content_w - col_gap) / 2;
    const int field_h = 30;

    const std::array<int, 5> left = {field_name, field_host, field_ssh_user,
                                     field_remote_port, field_ssh_port};
    const std::array<int, 5> right = {field_identity_file, field_known_hosts_file,
                                      field_cookie_source, field_width,
                                      field_height};

    for (int row = 0; row < 5; ++row) {
        const int ly = form_top + row * row_h + 16;
        const int lf = left[static_cast<std::size_t>(row)];
        draw_field(lf, margin, ly, col_w, field_h, field_label(lf),
                   editor_.text[static_cast<std::size_t>(lf)],
                   editor_.focus == lf,
                   field_for_validate_name(editor_.error_field) == lf);

        const int rf = right[static_cast<std::size_t>(row)];
        draw_field(rf, margin + col_w + col_gap, ly, col_w, field_h,
                   field_label(rf), editor_.text[static_cast<std::size_t>(rf)],
                   editor_.focus == rf,
                   field_for_validate_name(editor_.error_field) == rf);
    }

    // Toggle row: mode, favorite, auto-reconnect.
    const int toggles_y = form_top + 5 * row_h + 28;
    Region mode {Region::Kind::mode_cycle, {}, -1, margin, toggles_y, 190, 30};
    add_region(mode);
    draw_button(mode, "Mode: " + std::string(mode_name(editor_.mode)), false);

    Region fav {Region::Kind::favorite_toggle, {}, -1, margin + 206, toggles_y,
                150, 30};
    add_region(fav);
    draw_button(fav, editor_.favorite ? "\xE2\x98\x85 Favorite" : "\xE2\x98\x86 Favorite",
                false);

    Region reconnect {Region::Kind::autoreconnect_toggle, {}, -1,
                      margin + 372, toggles_y, 190, 30};
    add_region(reconnect);
    draw_button(reconnect,
                editor_.auto_reconnect ? "Auto-reconnect: on"
                                       : "Auto-reconnect: off",
                false);

    // Route preview and error line.
    const ConnectionProfile preview = build_editor_profile();
    draw_text("Route: " + preview.route_summary(), margin, toggles_y + 58,
              text_muted, 12);
    if (!editor_.error.empty())
        draw_text(editor_.error, margin, toggles_y + 80, danger, 13);

    // Save / Cancel.
    const int actions_y = surface_.height() - margin - 36;
    Region save {Region::Kind::save, {}, -1, margin, actions_y, 120, 36};
    add_region(save);
    draw_button(save, "Save", true);

    Region cancel {Region::Kind::cancel, {}, -1, margin + 132, actions_y, 120, 36};
    add_region(cancel);
    draw_button(cancel, "Cancel", false);
}

} // namespace haiku_remote
