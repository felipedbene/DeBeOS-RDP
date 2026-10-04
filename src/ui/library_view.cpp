#include "haiku_remote/ui/library_view.hpp"

#include <algorithm>
#include <string>

namespace haiku_remote::ui {

namespace {

constexpr int kQuickHostFieldId = 1;

std::string last_activity(const ConnectionProfile& profile)
{
    if (!profile.last_error.empty())
        return "last error: " + profile.last_error;
    if (profile.last_connected_at > 0)
        return "connected before";
    return "never connected";
}

} // namespace

void LibraryView::on_enter()
{
    confirm_delete_.reset();
    scroll_ = 0;
}

void LibraryView::render(Widgets& ui, LibraryController& controller, int width,
                         int height)
{
    const Theme& t = ui.theme();
    const int margin = 16;
    const int content_w = width - 2 * margin;

    // ---- Header: title, New connection, and a compact quick-connect. ----
    ui.label(margin, margin + t.font_size, "DeBeOS Remote", t.text);
    const std::string status =
        std::to_string(controller.profiles().size()) + " saved connection(s)";
    ui.label(margin, margin + t.font_size + ui.line_height(), status, t.text_dim);

    const int row_h = 30;
    int header_y = margin;
    const Box new_btn{width - margin - 150, header_y, 150, row_h};
    if (ui.button_accent(new_btn, "+ New connection")) {
        controller.open_new_editor();
        return;
    }

    // Quick-connect: an ephemeral, never-saved direct host (host[:port]).
    const int quick_y = margin + 2 * ui.line_height() + 10;
    ui.label(margin, quick_y + t.font_size, "Quick connect:", t.text_dim);
    const int quick_label_w = ui.measure("Quick connect:") + 12;
    const Box quick_field{margin + quick_label_w, quick_y, content_w - quick_label_w - 110,
                          row_h};
    ui.field(kQuickHostFieldId, quick_field, quick_host_, "host or host:port");
    const Box quick_btn{width - margin - 100, quick_y, 100, row_h};
    const bool quick_go = ui.button(quick_btn, "Connect", !quick_host_.empty())
        || (ui.input().enter && ui.focused() == kQuickHostFieldId
            && !quick_host_.empty());
    if (quick_go) {
        ConnectionProfile ephemeral;
        ephemeral.mode = ConnectionMode::direct;
        std::string host = quick_host_.text();
        int port = 10900;
        const auto colon = host.rfind(':');
        if (colon != std::string::npos) {
            try {
                port = std::stoi(host.substr(colon + 1));
                host = host.substr(0, colon);
            } catch (const std::exception&) {
                // Not a port suffix; connect to the whole string on the default.
            }
        }
        ephemeral.host = host;
        ephemeral.name = host;
        ephemeral.remote_port = port;
        controller.connect_config(coordinator_config_for(ephemeral));
        return;
    }

    // ---- The profile cards, favorite-first, scrollable. ----
    const int list_top = quick_y + row_h + 16;
    const int card_h = 92;
    const int card_gap = 10;
    const auto& profiles = controller.profiles();

    const int total_h =
        static_cast<int>(profiles.size()) * (card_h + card_gap);
    const int viewport_h = height - list_top - margin;
    const int max_scroll = std::max(0, total_h - viewport_h);
    scroll_ -= ui.input().wheel * 40;
    scroll_ = std::clamp(scroll_, 0, max_scroll);

    if (profiles.empty()) {
        ui.label(margin, list_top + t.font_size,
                 "No saved connections yet. Add one with New connection.",
                 t.text_dim);
        return;
    }

    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const ConnectionProfile& profile = profiles[i];
        const int card_y =
            list_top + static_cast<int>(i) * (card_h + card_gap) - scroll_;
        if (card_y + card_h < list_top || card_y > height) // off-screen: skip
            continue;

        const Box card{margin, card_y, content_w, card_h};
        const bool selected = controller.selected() && *controller.selected() == i;
        if (ui.row(card, selected))
            controller.select(i);

        const int tx = card.x + 12;
        const std::string title =
            (profile.favorite ? "* " : "") + (profile.name.empty() ? "(unnamed)"
                                                                   : profile.name);
        ui.label_clipped(tx, card_y + 6 + t.font_size, title, t.text, content_w - 320);
        ui.label_clipped(tx, card_y + 6 + t.font_size + ui.line_height(),
                         profile.route_summary(), t.text_dim, content_w - 24);
        ui.label(tx, card_y + card_h - 10, last_activity(profile),
                 profile.last_error.empty() ? t.text_dim : t.danger);

        // Per-card actions along the top-right.
        const int btn_w = 70;
        const int btn_h = 26;
        const int by = card_y + 8;
        int bx = card.x + card.w - 12 - btn_w;
        const auto next_btn = [&]() {
            const Box b{bx, by, btn_w, btn_h};
            bx -= btn_w + 6;
            return b;
        };

        if (confirm_delete_ && *confirm_delete_ == i) {
            ui.label(tx, card_y + card_h - 10 - ui.line_height(),
                     "Delete this connection?", t.danger);
            if (ui.button_danger(next_btn(), "Delete")) {
                confirm_delete_.reset();
                controller.remove(i);
                return;
            }
            if (ui.button(next_btn(), "Cancel")) {
                confirm_delete_.reset();
                return;
            }
            continue;
        }

        if (ui.button_accent(next_btn(), "Connect")) {
            controller.connect(i);
            return;
        }
        if (ui.button(next_btn(), "Edit")) {
            controller.open_editor(i);
            return;
        }
        if (ui.button(next_btn(), profile.favorite ? "Unfav" : "Fav")) {
            controller.toggle_favorite(i);
            return;
        }
        if (ui.button(next_btn(), "Dup")) {
            controller.duplicate(i);
            return;
        }
        if (ui.button_danger(next_btn(), "Delete")) {
            confirm_delete_ = i;
            return;
        }
    }
}

} // namespace haiku_remote::ui
