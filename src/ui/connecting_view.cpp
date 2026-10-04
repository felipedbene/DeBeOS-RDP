#include "haiku_remote/ui/connecting_view.hpp"

#include <SDL.h>

#include <algorithm>
#include <string>

namespace haiku_remote::ui {

void ConnectingView::render(Widgets& ui, LibraryController& controller, int width,
                            int height)
{
    const Theme& t = ui.theme();
    const int margin = 24;
    const Snapshot snap = controller.snapshot();
    const ConnectionProfile* profile = controller.connecting_profile();
    const bool failed = controller.mode() == AppMode::Failed;

    const std::string name = profile != nullptr && !profile->name.empty()
        ? profile->name
        : "connection";
    ui.label(margin, margin + t.font_size,
             (failed ? "Could not connect to " : "Connecting to ") + name,
             failed ? t.danger : t.text);

    int y = margin + ui.line_height() + 12;

    if (!failed) {
        ui.label(margin, y + t.font_size,
                 std::string("Stage: ") + std::string(coordinator_state_name(snap.state)),
                 t.text);
        y += ui.line_height();
        const long seconds = static_cast<long>(snap.elapsed.count() / 1000);
        ui.label(margin, y + t.font_size,
                 "Elapsed: " + std::to_string(seconds) + " s", t.text_dim);
        y += ui.line_height();
        if (snap.reconnect_attempt > 0) {
            ui.label(margin, y + t.font_size,
                     "Reconnect attempt " + std::to_string(snap.reconnect_attempt),
                     t.text_dim);
            y += ui.line_height();
        }
    } else {
        ui.label(margin, y + t.font_size,
                 std::string("Reason: ") + std::string(reason_code(snap.reason)),
                 t.danger);
        y += ui.line_height();
        if (!snap.message.empty()) {
            ui.label_clipped(margin, y + t.font_size, snap.message, t.text,
                             width - 2 * margin);
            y += ui.line_height();
        }
    }

    // The bounded diagnostic ring (always useful; the copyable record on
    // failure). A simple wheel-scrollable list.
    y += 8;
    ui.label(margin, y + t.font_size, "Diagnostics:", t.text_dim);
    y += ui.line_height() + 4;
    const int log_top = y;
    const int actions_h = 48;
    const int log_h = height - log_top - margin - actions_h;
    const Box log_box{margin, log_top, width - 2 * margin, std::max(0, log_h)};
    ui.fill(log_box, t.field);
    ui.outline(log_box, t.border);

    const int line_h = ui.line_height();
    const int visible_lines = std::max(1, log_box.h / line_h);
    const int total = static_cast<int>(snap.diag.size());
    const int max_scroll = std::max(0, total - visible_lines);
    diag_scroll_ -= ui.input().wheel;
    diag_scroll_ = std::clamp(diag_scroll_, 0, max_scroll);
    for (int i = 0; i < visible_lines && (i + diag_scroll_) < total; ++i) {
        const auto& entry = snap.diag[static_cast<std::size_t>(i + diag_scroll_)];
        ui.label_clipped(log_box.x + 8, log_box.y + i * line_h + t.font_size + 4,
                         entry, t.text, log_box.w - 16);
    }

    // Actions.
    const int by = height - margin - 32;
    if (failed) {
        if (ui.button(Box{margin, by, 160, 32}, "Copy diagnostics")) {
            std::string clip;
            clip += "reason: ";
            clip += reason_code(snap.reason);
            clip += '\n';
            if (!snap.message.empty())
                clip += snap.message + "\n";
            for (const auto& entry : snap.diag)
                clip += entry + "\n";
            SDL_SetClipboardText(clip.c_str());
        }
        if (ui.button_accent(Box{margin + 170, by, 160, 32}, "Back to library"))
            controller.dismiss_failure();
    } else {
        if (ui.button_danger(Box{margin, by, 120, 32}, "Cancel"))
            controller.cancel();
    }
}

} // namespace haiku_remote::ui
