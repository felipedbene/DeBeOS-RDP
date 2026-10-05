#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/connect_screen.hpp"
#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/library_screen.hpp"
#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/profile_library.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/transport.hpp"

#include <memory>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace haiku_remote;

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto present_interval = std::chrono::milliseconds(16);
constexpr auto batch_idle_time = std::chrono::milliseconds(6);
constexpr auto maximum_frame_latency = std::chrono::milliseconds(32);

struct Options {
    TransportOptions transport;
    int width = 1280;
    int height = 800;
    bool stats = false;
    // True once any connection-selecting argument (--host, --url, a credential,
    // ...) was seen. When false, the client opens the connection library
    // instead of connecting; the CLI path is thereby preserved untouched.
    bool target_specified = false;
};

int parse_integer(std::string_view value, std::string_view name,
                  int minimum, int maximum)
{
    std::size_t parsed = 0;
    const long long number = std::stoll(std::string(value), &parsed);
    if (parsed != value.size() || number < minimum || number > maximum)
        throw std::runtime_error("invalid " + std::string(name) + ": "
                                 + std::string(value));
    return static_cast<int>(number);
}

struct Damage {
    int left = std::numeric_limits<int>::max();
    int top = std::numeric_limits<int>::max();
    int right = -1;
    int bottom = -1;

    void include(int x1, int y1, int x2, int y2)
    {
        left = std::min(left, x1);
        top = std::min(top, y1);
        right = std::max(right, x2);
        bottom = std::max(bottom, y2);
    }

    [[nodiscard]] bool empty() const { return right < left || bottom < top; }
    [[nodiscard]] std::uint64_t pixels() const
    {
        if (empty())
            return 0;
        return static_cast<std::uint64_t>(right - left + 1)
            * static_cast<std::uint64_t>(bottom - top + 1);
    }
};

struct Samples {
    double total_ms = 0;
    double maximum_ms = 0;
    std::uint64_t count = 0;

    void add(Clock::duration duration)
    {
        const double value = std::chrono::duration<double, std::milli>(duration).count();
        total_ms += value;
        maximum_ms = std::max(maximum_ms, value);
        ++count;
    }

    [[nodiscard]] double average() const
    {
        return count == 0 ? 0 : total_ms / static_cast<double>(count);
    }
};

struct PerformanceStats {
    Clock::time_point interval_start = Clock::now();
    std::size_t previous_message_count = 0;
    std::size_t previous_draw_string_replies = 0;
    std::size_t previous_string_width_replies = 0;
    std::uint64_t bytes = 0;
    std::uint64_t receive_batches = 0;
    std::uint64_t frames = 0;
    std::uint64_t damaged_pixels = 0;
    std::uint64_t full_frame_pixels = 0;
    Samples ingest;
    Samples copy;
    Samples x11;
    Samples batch;
    Samples response;
    Samples input_present;

    std::string report(std::size_t message_count, std::size_t draw_string_replies,
                       std::size_t string_width_replies, Clock::time_point now)
    {
        const double seconds =
            std::chrono::duration<double>(now - interval_start).count();
        const auto messages = message_count - previous_message_count;
        // Each of these blocked app_server's drawing thread for a round trip of
        // this link, so they are reported as a rate and, multiplied by the link
        // RTT, are the server-side stall this client imposed.
        const auto strings = draw_string_replies - previous_draw_string_replies;
        const auto widths = string_width_replies - previous_string_width_replies;
        std::ostringstream text;
        text << std::fixed << std::setprecision(1)
             << frames / seconds << " fps"
             << " | " << messages / seconds << " msg/s"
             << " | " << receive_batches / seconds << " rx/s"
             << " | " << bytes / seconds / (1024.0 * 1024.0) << " MiB/s"
             << " | decode " << ingest.total_ms / seconds << " ms/s"
             << " | copy " << copy.average() << " ms"
             << " | X11 " << x11.average() << " ms"
             << " | damage "
             << (frames == 0 || full_frame_pixels == 0
                     ? 0
                     : 100.0 * damaged_pixels
                         / static_cast<double>(frames * full_frame_pixels))
             << '%'
             << " | batch " << batch.average() << '/' << batch.maximum_ms << " ms";
        if (response.count != 0)
            text << " | response " << response.average() << '/'
                 << response.maximum_ms << " ms";
        if (input_present.count != 0)
            text << " | input " << input_present.average() << '/'
                 << input_present.maximum_ms << " ms";
        text << " | sync " << strings << " str/" << widths << " sw"
             << " | total " << message_count;

        interval_start = now;
        previous_message_count = message_count;
        previous_draw_string_replies = draw_string_replies;
        previous_string_width_replies = string_width_replies;
        bytes = 0;
        receive_batches = 0;
        frames = 0;
        damaged_pixels = 0;
        ingest = {};
        copy = {};
        x11 = {};
        batch = {};
        response = {};
        input_present = {};
        return text.str();
    }
};

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc)
                throw std::runtime_error("missing value for " + argument);
            return argv[i];
        };
        if (parse_transport_argument(options.transport, argument, value)) {
            options.target_specified = true;
        }
        else if (argument == "--width")
            options.width = parse_integer(
                value(), "width", 1, Surface::max_dimension);
        else if (argument == "--height")
            options.height = parse_integer(
                value(), "height", 1, Surface::max_dimension);
        else if (argument == "--stats") options.stats = true;
        else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: haiku-remote-x11" << transport_usage()
                      << "\n  [--width PX] [--height PX] [--stats]\n\n"
                      << exit_status_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    return options;
}

std::uint32_t button_mask(unsigned state)
{
    std::uint32_t result = 0;
    if ((state & Button1Mask) != 0) result |= buttons::primary;
    if ((state & Button3Mask) != 0) result |= buttons::secondary;
    if ((state & Button2Mask) != 0) result |= buttons::tertiary;
    return result;
}

std::uint32_t modifier_mask(unsigned state)
{
    std::uint32_t result = 0;
    if ((state & ShiftMask) != 0)
        result |= modifiers::shift | modifiers::left_shift;
    if ((state & ControlMask) != 0)
        result |= modifiers::control | modifiers::left_control;
    if ((state & LockMask) != 0)
        result |= modifiers::caps_lock;
    // Match the physical PC layout: Alt is Haiku Command, Super is Option.
    if ((state & Mod1Mask) != 0)
        result |= modifiers::command | modifiers::left_command;
    if ((state & Mod4Mask) != 0)
        result |= modifiers::option | modifiers::left_option;
    return result;
}

std::int32_t haiku_key(KeySym key)
{
    switch (key) {
    case XK_Escape: return 0x01;
    case XK_F1: return 0x02;
    case XK_F2: return 0x03;
    case XK_F3: return 0x04;
    case XK_F4: return 0x05;
    case XK_F5: return 0x06;
    case XK_F6: return 0x07;
    case XK_F7: return 0x08;
    case XK_F8: return 0x09;
    case XK_F9: return 0x0a;
    case XK_F10: return 0x0b;
    case XK_F11: return 0x0c;
    case XK_F12: return 0x0d;
    case XK_BackSpace: return 0x1e;
    case XK_Insert: return 0x1f;
    case XK_Home: return 0x20;
    case XK_Page_Up: return 0x21;
    case XK_Tab: return 0x26;
    case XK_Delete: return 0x34;
    case XK_End: return 0x35;
    case XK_Page_Down: return 0x36;
    case XK_Return:
    case XK_ISO_Enter:
    case XK_KP_Enter: return 0x47;
    case XK_Up:
    case XK_KP_Up: return 0x57;
    case XK_space: return 0x5e;
    case XK_Left:
    case XK_KP_Left: return 0x61;
    case XK_Down:
    case XK_KP_Down: return 0x62;
    case XK_Right:
    case XK_KP_Right: return 0x63;
    default: return 0;
    }
}

std::string_view haiku_special_text(KeySym key)
{
    switch (key) {
    case XK_Home:
    case XK_KP_Home: return std::string_view("\x01", 1);
    case XK_End:
    case XK_KP_End: return std::string_view("\x04", 1);
    case XK_Insert:
    case XK_KP_Insert: return std::string_view("\x05", 1);
    case XK_BackSpace: return std::string_view("\x08", 1);
    case XK_Tab:
    case XK_ISO_Left_Tab: return std::string_view("\x09", 1);
    case XK_Return:
    case XK_ISO_Enter:
    case XK_KP_Enter: return std::string_view("\x0a", 1);
    case XK_Page_Up:
    case XK_KP_Page_Up: return std::string_view("\x0b", 1);
    case XK_Page_Down:
    case XK_KP_Page_Down: return std::string_view("\x0c", 1);
    case XK_Escape: return std::string_view("\x1b", 1);
    case XK_Left:
    case XK_KP_Left: return std::string_view("\x1c", 1);
    case XK_Right:
    case XK_KP_Right: return std::string_view("\x1d", 1);
    case XK_Up:
    case XK_KP_Up: return std::string_view("\x1e", 1);
    case XK_Down:
    case XK_KP_Down: return std::string_view("\x1f", 1);
    case XK_Delete:
    case XK_KP_Delete: return std::string_view("\x7f", 1);
    default: return {};
    }
}

std::int32_t first_utf8_scalar(std::string_view text)
{
    if (text.empty())
        return 0;
    const auto first = static_cast<unsigned char>(text[0]);
    if (first < 0x80)
        return first;
    int count = 0;
    std::uint32_t value = 0;
    if ((first & 0xe0) == 0xc0) {
        count = 2;
        value = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        count = 3;
        value = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        count = 4;
        value = first & 0x07;
    } else {
        return 0;
    }
    if (text.size() < static_cast<std::size_t>(count))
        return 0;
    for (int i = 1; i < count; ++i) {
        const auto byte = static_cast<unsigned char>(text[static_cast<std::size_t>(i)]);
        if ((byte & 0xc0) != 0x80)
            return 0;
        value = (value << 6) | (byte & 0x3f);
    }
    return static_cast<std::int32_t>(value);
}

Damage copy_surface_to_image(const Surface& surface, XImage& image, bool force_full)
{
    const auto source = surface.pixels();
    const bool native_bgra = image.bits_per_pixel == 32
        && image.byte_order == LSBFirst
        && image.red_mask == 0x00ff0000
        && image.green_mask == 0x0000ff00
        && image.blue_mask == 0x000000ff;
    if (native_bgra) {
        if (force_full) {
            for (int y = 0; y < surface.height(); ++y) {
                std::memcpy(image.data + y * image.bytes_per_line,
                            source.data()
                                + static_cast<std::size_t>(y * surface.stride()),
                            static_cast<std::size_t>(surface.stride()));
            }
            return {0, 0, surface.width() - 1, surface.height() - 1};
        }

        Damage damage;
        for (int y = 0; y < surface.height(); ++y) {
            const auto* source_row = source.data()
                + static_cast<std::size_t>(y * surface.stride());
            auto* image_row = reinterpret_cast<std::uint8_t*>(
                image.data + y * image.bytes_per_line);
            if (std::memcmp(source_row, image_row,
                            static_cast<std::size_t>(surface.stride())) == 0)
                continue;

            int left = 0;
            while (left < surface.width()
                   && std::memcmp(source_row + left * 4,
                                  image_row + left * 4, 4) == 0)
                ++left;
            int right = surface.width() - 1;
            while (right > left
                   && std::memcmp(source_row + right * 4,
                                  image_row + right * 4, 4) == 0)
                --right;
            std::memcpy(image_row + left * 4, source_row + left * 4,
                        static_cast<std::size_t>((right - left + 1) * 4));
            damage.include(left, y, right, y);
        }
        return damage;
    }
    for (int y = 0; y < surface.height(); ++y) {
        for (int x = 0; x < surface.width(); ++x) {
            const auto color = surface.pixel(x, y);
            const auto value = (static_cast<unsigned long>(color.r) << 16)
                | (static_cast<unsigned long>(color.g) << 8)
                | color.b;
            XPutPixel(&image, x, y, value);
        }
    }
    return {0, 0, surface.width() - 1, surface.height() - 1};
}

// Core Xlib has no ARGB cursor -- Xrender and Xcursor are not dependencies of
// this build -- so the server's cursor becomes an XCreatePixmapCursor: the alpha
// channel is its mask and luminance picks black or white per pixel. Shape and
// hotspot are exact; only colour is reduced. Returns None when the bitmap cannot
// be represented, in which case the caller keeps the host pointer.
::Cursor build_x_cursor(Display* display, Window window,
                        const CursorState& cursor)
{
    const int width = cursor.bitmap.width;
    const int height = cursor.bitmap.height;
    if (width <= 0 || height <= 0)
        return None;
    // RP_SET_CURSOR also carries drag feedback images, which have no size limit
    // of their own, and an X cursor larger than the display supports is a
    // protocol error rather than a scaled-down cursor. Ask instead of guessing.
    unsigned best_width = 0;
    unsigned best_height = 0;
    if (XQueryBestCursor(display, window, static_cast<unsigned>(width),
                         static_cast<unsigned>(height), &best_width,
                         &best_height) == 0
        || best_width < static_cast<unsigned>(width)
        || best_height < static_cast<unsigned>(height)) {
        return None;
    }

    // XCreateBitmapFromData takes LSB-first bits with each row padded to a whole
    // byte. A set mask bit means "drawn"; a set source bit selects the
    // foreground colour.
    const auto row_bytes = static_cast<std::size_t>((width + 7) / 8);
    std::vector<char> source(row_bytes * static_cast<std::size_t>(height), 0);
    std::vector<char> mask(source.size(), 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto pixel = (static_cast<std::size_t>(y)
                * static_cast<std::size_t>(width)
                + static_cast<std::size_t>(x)) * 4;
            const unsigned blue = cursor.bitmap.bgra[pixel];
            const unsigned green = cursor.bitmap.bgra[pixel + 1];
            const unsigned red = cursor.bitmap.bgra[pixel + 2];
            const unsigned alpha = cursor.bitmap.bgra[pixel + 3];
            if (alpha < 128)
                continue;
            const auto index = static_cast<std::size_t>(y) * row_bytes
                + static_cast<std::size_t>(x / 8);
            const auto bit = static_cast<char>(1 << (x % 8));
            mask[index] = static_cast<char>(mask[index] | bit);
            const unsigned luminance = (red * 77 + green * 151 + blue * 28) >> 8;
            if (luminance >= 128)
                source[index] = static_cast<char>(source[index] | bit);
        }
    }

    const Pixmap source_pixmap = XCreateBitmapFromData(
        display, window, source.data(), static_cast<unsigned>(width),
        static_cast<unsigned>(height));
    const Pixmap mask_pixmap = XCreateBitmapFromData(
        display, window, mask.data(), static_cast<unsigned>(width),
        static_cast<unsigned>(height));
    XColor foreground {};
    foreground.red = foreground.green = foreground.blue = 0xffff;
    XColor background {};
    // The hotspot is an offset inside the bitmap and X rejects one outside it.
    // The server already clamps it (ServerCursor.cpp:52), but a clamp here costs
    // nothing and a rejected cursor costs the pointer.
    const int hotspot_x = std::clamp(
        raster_coordinate(std::floor(static_cast<double>(cursor.hotspot.x)), 0),
        0, width - 1);
    const int hotspot_y = std::clamp(
        raster_coordinate(std::floor(static_cast<double>(cursor.hotspot.y)), 0),
        0, height - 1);
    const ::Cursor result = XCreatePixmapCursor(
        display, source_pixmap, mask_pixmap, &foreground, &background,
        static_cast<unsigned>(hotspot_x), static_cast<unsigned>(hotspot_y));
    XFreePixmap(display, source_pixmap);
    XFreePixmap(display, mask_pixmap);
    return result;
}

// A 1x1 cursor with an empty mask: the pointer is still there and still delivers
// motion, it just draws nothing. That is what RP_SET_CURSOR_VISIBLE false means,
// and it is how the HTML5 client does it too (container.style.cursor = 'none').
::Cursor build_invisible_cursor(Display* display, Window window)
{
    const char nothing = 0;
    const Pixmap empty = XCreateBitmapFromData(display, window, &nothing, 1, 1);
    XColor black {};
    const ::Cursor result =
        XCreatePixmapCursor(display, empty, empty, &black, &black, 0, 0);
    XFreePixmap(display, empty);
    return result;
}

// Describe how the session ended and say whether that ending was a success.
// Returns true for an orderly end (exit 0), false for a failure (exit non-zero).
//
// Every post-connect ending used to collapse onto "receive failed: ..." and
// exit 0, which made three very different outcomes indistinguishable: a session
// that ran and was shut down, a transport fault, and a session the server
// refused before sending a single byte of drawing. The third is the one that
// costs time -- it presents as "the desktop is black" while the process reports
// success -- so it gets its own message and its own exit status.
bool report_session_end(bool orderly, std::size_t messages,
                        std::string_view reason)
{
    if (!orderly) {
        std::cerr << "receive failed: " << reason << '\n';
        return false;
    }
    if (messages == 0) {
        std::cerr << "the server closed the connection before sending any"
                     " drawing data: the session was refused or never started\n";
        return false;
    }
    std::cerr << "session ended: the server closed the connection after "
              << messages << " messages\n";
    return true;
}

} // namespace

// Run one connected remote-desktop session over an ALREADY-connected transport.
// Part 3 split this out of run_session() so the library path can connect with a
// visible progress screen first (connect_with_progress) and hand the live
// transport straight here, while the CLI path still connects inline below.
// `refused_no_data` is set true when the session ended by an orderly close with
// no drawing ever received -- the "refused / nothing to show" case the library
// path turns into an actionable error rather than a black flash.
int run_session_loop(const Options& options,
                     const std::unique_ptr<Transport>& transport,
                     bool& refused_no_data)
{
    refused_no_data = false;
    try {
        std::string socket_error;
        Display* display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            std::cerr << "could not open X display\n";
            return exit_status::failed;
        }
        const int screen = DefaultScreen(display);
        Window window = XCreateSimpleWindow(
            display, RootWindow(display, screen), 0, 0,
            static_cast<unsigned>(options.width),
            static_cast<unsigned>(options.height), 0,
            BlackPixel(display, screen), BlackPixel(display, screen));
        XStoreName(display, window, "Haiku Remote");
        XSelectInput(display, window,
            ExposureMask | KeyPressMask | KeyReleaseMask
            | ButtonPressMask | ButtonReleaseMask | PointerMotionMask
            | FocusChangeMask | StructureNotifyMask);

        XSizeHints hints {};
        hints.flags = PMinSize | PMaxSize;
        hints.min_width = hints.max_width = options.width;
        hints.min_height = hints.max_height = options.height;
        XSetWMNormalHints(display, window, &hints);

        const Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display, window, const_cast<Atom*>(&wm_delete), 1);
        XMapWindow(display, window);

        auto* image_data = static_cast<char*>(
            std::calloc(static_cast<std::size_t>(options.width)
                            * static_cast<std::size_t>(options.height),
                        4));
        if (image_data == nullptr)
            throw std::bad_alloc();
        XImage* image = XCreateImage(
            display, DefaultVisual(display, screen),
            static_cast<unsigned>(DefaultDepth(display, screen)),
            ZPixmap, 0, image_data,
            static_cast<unsigned>(options.width),
            static_cast<unsigned>(options.height), 32, 0);
        if (image == nullptr) {
            std::free(image_data);
            throw std::runtime_error("could not create XImage");
        }
        GC gc = XCreateGC(display, window, 0, nullptr);

        Session session(
            options.width, options.height,
            [&](std::span<const std::uint8_t> bytes) {
                return transport->send_all(bytes, socket_error);
            },
            [](std::string_view line) { std::cerr << line << '\n'; });
        // Session::start() returns void and ignores whether the handshake went
        // out, so a connection that dies between connect() and the first write
        // otherwise looks like a session that simply received nothing. The send
        // callback only assigns socket_error on failure, so an empty string
        // after start() means both handshake messages were written.
        socket_error.clear();
        session.start();
        if (!socket_error.empty())
            throw std::runtime_error("handshake send to " + transport->describe()
                                     + " failed: " + socket_error);

        bool running = true;
        bool session_failed = false;
        bool dirty = true;
        bool expose_requested = true;
        auto dirty_since = Clock::now();
        auto last_network_activity = dirty_since;
        auto last_present = dirty_since - present_interval;
        PerformanceStats performance;
        performance.full_frame_pixels =
            static_cast<std::uint64_t>(options.width)
            * static_cast<std::uint64_t>(options.height);
        std::optional<Clock::time_point> pending_input;
        bool input_response_seen = false;
        std::uint32_t last_modifiers = 0;
        Time last_click_time = 0;
        unsigned last_click_button = 0;
        int last_click_x = 0;
        int last_click_y = 0;
        int click_count = 0;
        std::array<std::uint8_t, 256 * 1024> buffer {};

        const auto send = [&](std::vector<std::uint8_t> message,
                              bool user_input = false) {
            if (options.stats && user_input && !pending_input.has_value()) {
                pending_input = Clock::now();
                input_response_seen = false;
            }
            if (!session.send_client_message(message)) {
                std::cerr << "send failed: " << socket_error << '\n';
                session_failed = true;
                running = false;
            }
        };
        const ::Cursor invisible_cursor =
            build_invisible_cursor(display, window);
        ::Cursor server_cursor = None;
        std::uint64_t applied_cursor_generation = 0;
        std::optional<bool> applied_cursor_shown;
        // The X pointer position and the server's cursor position track each
        // other -- we send RP_MOUSE_MOVED and the desktop answers
        // RP_MOVE_CURSOR_TO with the same point -- so the native cursor needs no
        // per-frame repositioning; only the shape and visibility change.
        const auto sync_cursor = [&]() {
            const auto& remote = session.cursor();
            if (remote.generation == 0)
                return; // No RP_SET_CURSOR yet: leave the host pointer alone.
            if (remote.generation != applied_cursor_generation) {
                applied_cursor_generation = remote.generation;
                if (server_cursor != None)
                    XFreeCursor(display, server_cursor);
                server_cursor = build_x_cursor(display, window, remote);
                applied_cursor_shown.reset();
            }
            if (applied_cursor_shown.has_value()
                && *applied_cursor_shown == remote.visible) {
                return;
            }
            applied_cursor_shown = remote.visible;
            if (server_cursor == None) {
                // Unrepresentable shape: a host arrow beats no pointer at all.
                XUndefineCursor(display, window);
            } else {
                XDefineCursor(display, window,
                              remote.visible ? server_cursor : invisible_cursor);
            }
            XFlush(display);
        };

        const auto update_modifiers = [&](unsigned state) {
            const auto current = modifier_mask(state);
            if (current != last_modifiers) {
                last_modifiers = current;
                send(InputEncoder::modifiers_changed(current));
            }
        };

        while (running) {
            const int received = transport->receive(buffer, 2, socket_error);
            if (received < 0) {
                const bool orderly =
                    transport->peer_closed() || session.server_closed();
                if (orderly && session.message_count() == 0)
                    refused_no_data = true;
                session_failed = !report_session_end(
                    orderly, session.message_count(), socket_error);
                break;
            }
            if (received > 0) {
                const auto ingest_start = Clock::now();
                session.ingest(
                    std::span(buffer.data(), static_cast<std::size_t>(received)));
                const auto now = Clock::now();
                if (options.stats) {
                    performance.bytes += static_cast<std::uint64_t>(received);
                    ++performance.receive_batches;
                    performance.ingest.add(now - ingest_start);
                    if (pending_input.has_value() && !input_response_seen) {
                        performance.response.add(now - *pending_input);
                        input_response_seen = true;
                    }
                }
                if (!dirty)
                    dirty_since = now;
                last_network_activity = now;
                dirty = true;
            }
            // RP_CLOSE_CONNECTION arrives before the EOF does. Stop on it
            // rather than idling against a socket that will never say anything
            // again; clearing `running` rather than breaking lets this last
            // iteration present the frame the close arrived with.
            if (session.server_closed() && running) {
                if (session.message_count() == 0)
                    refused_no_data = true;
                session_failed = !report_session_end(
                    true, session.message_count(), socket_error);
                running = false;
            }

            while (XPending(display) > 0) {
                XEvent event {};
                XNextEvent(display, &event);
                switch (event.type) {
                case Expose:
                    dirty = true;
                    expose_requested = true;
                    break;
                case ClientMessage:
                    if (static_cast<Atom>(event.xclient.data.l[0]) == wm_delete)
                        running = false;
                    break;
                case MotionNotify:
                    // Keep ordering relative to buttons/keys, but collapse a
                    // contiguous run of stale pointer positions.
                    while (XPending(display) > 0) {
                        XEvent next {};
                        XPeekEvent(display, &next);
                        if (next.type != MotionNotify)
                            break;
                        XNextEvent(display, &event);
                    }
                    send(InputEncoder::mouse_moved(
                        static_cast<float>(event.xmotion.x),
                        static_cast<float>(event.xmotion.y)), true);
                    break;
                case ButtonPress: {
                    if (event.xbutton.button >= Button4
                        && event.xbutton.button <= 7) {
                        float dx = 0;
                        float dy = 0;
                        if (event.xbutton.button == Button4) dy = -1;
                        if (event.xbutton.button == Button5) dy = 1;
                        if (event.xbutton.button == 6) dx = -1;
                        if (event.xbutton.button == 7) dx = 1;
                        send(InputEncoder::mouse_wheel(dx, dy), true);
                        break;
                    }
                    const bool repeated = event.xbutton.button == last_click_button
                        && event.xbutton.time - last_click_time < 400
                        && std::abs(event.xbutton.x - last_click_x) <= 4
                        && std::abs(event.xbutton.y - last_click_y) <= 4;
                    click_count = repeated ? click_count + 1 : 1;
                    last_click_time = event.xbutton.time;
                    last_click_button = event.xbutton.button;
                    last_click_x = event.xbutton.x;
                    last_click_y = event.xbutton.y;
                    unsigned state = event.xbutton.state;
                    if (event.xbutton.button == Button1) state |= Button1Mask;
                    if (event.xbutton.button == Button2) state |= Button2Mask;
                    if (event.xbutton.button == Button3) state |= Button3Mask;
                    send(InputEncoder::mouse_down(
                        static_cast<float>(event.xbutton.x),
                        static_cast<float>(event.xbutton.y),
                        button_mask(state), click_count), true);
                    break;
                }
                case ButtonRelease: {
                    if (event.xbutton.button >= Button4
                        && event.xbutton.button <= 7)
                        break;
                    unsigned state = event.xbutton.state;
                    if (event.xbutton.button == Button1) state &= ~Button1Mask;
                    if (event.xbutton.button == Button2) state &= ~Button2Mask;
                    if (event.xbutton.button == Button3) state &= ~Button3Mask;
                    send(InputEncoder::mouse_up(
                        static_cast<float>(event.xbutton.x),
                        static_cast<float>(event.xbutton.y),
                        button_mask(state)), true);
                    break;
                }
                case KeyPress:
                case KeyRelease: {
                    char text_buffer[64] {};
                    KeySym symbol = NoSymbol;
                    XComposeStatus compose {};
                    const int length = XLookupString(
                        &event.xkey, text_buffer,
                        static_cast<int>(sizeof(text_buffer) - 1),
                        &symbol, &compose);
                    std::string text(
                        text_buffer, static_cast<std::size_t>(std::max(length, 0)));
                    const auto special_text = haiku_special_text(symbol);
                    if (!special_text.empty())
                        text.assign(special_text);
                    update_modifiers(event.xkey.state);
                    send(InputEncoder::key(
                        event.type == KeyPress, text,
                        first_utf8_scalar(text), haiku_key(symbol)), true);
                    break;
                }
                case FocusOut:
                    last_modifiers = 0;
                    send(InputEncoder::modifiers_changed(0));
                    break;
                default:
                    break;
                }
            }

            sync_cursor();

            const auto now = Clock::now();
            const bool frame_due = now - last_present >= present_interval;
            const bool batch_complete =
                now - last_network_activity >= batch_idle_time;
            const bool latency_reached =
                now - dirty_since >= maximum_frame_latency;
            if (dirty && (expose_requested
                    || (frame_due && (batch_complete || latency_reached)))) {
                const auto copy_start = Clock::now();
                const auto damage = copy_surface_to_image(
                    session.surface(), *image, expose_requested);
                const auto x11_start = Clock::now();
                if (!damage.empty()) {
                    XPutImage(display, window, gc, image,
                              damage.left, damage.top, damage.left, damage.top,
                              static_cast<unsigned>(damage.right - damage.left + 1),
                              static_cast<unsigned>(damage.bottom - damage.top + 1));
                    XFlush(display);
                }
                const auto present_complete = Clock::now();
                if (options.stats) {
                    performance.copy.add(x11_start - copy_start);
                    if (!damage.empty()) {
                        ++performance.frames;
                        performance.damaged_pixels += damage.pixels();
                        performance.x11.add(present_complete - x11_start);
                        performance.batch.add(present_complete - dirty_since);
                        if (pending_input.has_value() && input_response_seen) {
                            performance.input_present.add(
                                present_complete - *pending_input);
                            pending_input.reset();
                            input_response_seen = false;
                        }
                    }
                }
                dirty = false;
                expose_requested = false;
                last_present = present_complete;
            }

            if (options.stats
                && now - performance.interval_start >= std::chrono::seconds(1)) {
                const auto details =
                    performance.report(session.message_count(),
                                       session.draw_string_replies(),
                                       session.string_width_replies(), now);
                const auto title = "Haiku Remote | " + details;
                XStoreName(display, window, title.c_str());
                XFlush(display);
                std::cerr << "[perf] " << details << '\n';
            }
        }

        if (server_cursor != None)
            XFreeCursor(display, server_cursor);
        XFreeCursor(display, invisible_cursor);
        XFreeGC(display, gc);
        XDestroyImage(image);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        // A user-initiated quit (WM_DELETE_WINDOW) leaves session_failed false
        // and is still a success; only the error and refused-session paths are
        // not.
        return session_failed ? exit_status::failed : exit_status::ok;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return exit_status::usage;
    }
}

// The CLI path, unchanged in behaviour: resolve a transport from the options,
// connect it inline (reporting a connect failure exactly as before), then run
// the session loop. The automation/debugging entry point is untouched.
int run_session(const Options& options)
{
    std::string socket_error;
    auto transport = make_transport(options.transport, socket_error);
    if (transport == nullptr) {
        std::cerr << socket_error << '\n';
        return exit_status::failed;
    }
    if (!connect_with_broker_trust(*transport,
                                   terminal_trust_policy(options.transport),
                                   socket_error)) {
        std::cerr << "connect to " << transport->describe() << " failed: "
                  << socket_error << '\n';
        // See exit_status_usage(): a credential this client was never given is
        // its own exit code, so a harness can tell a local misinvocation from a
        // refusal out on the wire.
        return connect_exit_status(*transport);
    }
    bool refused = false;
    return run_session_loop(options, transport, refused);
}

// The result of the connection-progress window: the connected transport (plus
// the managed substrate it needs kept alive) on success, or the user's choice
// on the error screen.
struct ConnectOutcome {
    ConnectResult result;
    ConnectScreen::Action action = ConnectScreen::Action::none; // only on failure
};

// Open a window and run connect_with_progress for `profile`, painting the
// progress checklist as the flow advances. This replaces the old silent black
// window between Connect and the session. On success the window is torn down
// and the live transport returned (the session opens its own window). On
// failure the window stays up showing the actionable error until the user picks
// Back or Retry (or closes it, treated as Back).
ConnectOutcome run_connect(const ConnectionProfile& profile)
{
    ConnectOutcome out;
    const std::string title = profile.name.empty() ? profile.host : profile.name;

    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        // No display: connect headless so a session can still be attempted, and
        // report any failure through the library card rather than a window.
        std::cerr << "could not open X display for the connect screen\n";
        ConnectFlow flow(plan_launch(profile));
        out.result = connect_with_progress(profile, flow, [] {});
        if (!out.result.ok)
            out.action = ConnectScreen::Action::back;
        return out;
    }

    const int screen = DefaultScreen(display);
    const int sw = DisplayWidth(display, screen);
    const int sh = DisplayHeight(display, screen);
    const int width = std::min(900, std::max(560, sw - 80));
    const int height = std::min(560, std::max(360, sh - 120));

    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen), 0, 0,
        static_cast<unsigned>(width), static_cast<unsigned>(height), 0,
        BlackPixel(display, screen), BlackPixel(display, screen));
    XStoreName(display, window, "DeBeOS Remote \xE2\x80\x94 Connecting");
    XSelectInput(display, window,
                 ExposureMask | KeyPressMask | ButtonPressMask
                     | PointerMotionMask | StructureNotifyMask);
    const Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, const_cast<Atom*>(&wm_delete), 1);
    XMapWindow(display, window);

    auto* image_data = static_cast<char*>(std::calloc(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 4));
    XImage* image = image_data == nullptr
                        ? nullptr
                        : XCreateImage(display, DefaultVisual(display, screen),
                                       static_cast<unsigned>(
                                           DefaultDepth(display, screen)),
                                       ZPixmap, 0, image_data,
                                       static_cast<unsigned>(width),
                                       static_cast<unsigned>(height), 32, 0);
    if (image == nullptr) {
        std::free(image_data);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        // Degrade to a headless connect rather than failing outright.
        ConnectFlow flow(plan_launch(profile));
        out.result = connect_with_progress(profile, flow, [] {});
        if (!out.result.ok)
            out.action = ConnectScreen::Action::back;
        return out;
    }
    GC gc = XCreateGC(display, window, 0, nullptr);

    ConnectFlow flow(plan_launch(profile));
    ConnectScreen ui(flow, title, width, height);
    bool closed = false;

    const auto blit = [&]() {
        const Surface& surface = ui.render();
        copy_surface_to_image(surface, *image, true);
        XPutImage(display, window, gc, image, 0, 0, 0, 0,
                  static_cast<unsigned>(width), static_cast<unsigned>(height));
        XFlush(display);
    };
    const auto pump = [&]() {
        while (XPending(display) > 0) {
            XEvent event {};
            XNextEvent(display, &event);
            if (event.type == Expose)
                ui.mark_dirty();
            else if (event.type == ClientMessage
                     && static_cast<Atom>(event.xclient.data.l[0]) == wm_delete)
                closed = true;
        }
    };
    // Called by connect_with_progress at every stage boundary: repaint the new
    // flow state and keep the window responsive. The blocking SSH/connect calls
    // run between these, with the current stage label already on screen.
    const auto on_progress = [&]() {
        ui.mark_dirty();
        blit();
        pump();
    };

    // Block on window events until the screen yields an action (a window
    // close counts as the refusing choice). Shared by the error screen and the
    // broker trust prompt.
    const auto wait_for_action = [&]() -> ConnectScreen::Action {
        ui.mark_dirty();
        blit();
        ConnectScreen::Action action = ConnectScreen::Action::none;
        while (!closed && action == ConnectScreen::Action::none) {
            if (ui.dirty())
                blit();
            XEvent event {};
            XNextEvent(display, &event);
            switch (event.type) {
            case Expose:
                ui.mark_dirty();
                break;
            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) == wm_delete)
                    closed = true;
                break;
            case MotionNotify:
                ui.pointer_move(event.xmotion.x, event.xmotion.y);
                break;
            case ButtonPress:
                if (event.xbutton.button == Button1)
                    ui.pointer_press(event.xbutton.x, event.xbutton.y);
                break;
            case KeyPress: {
                KeySym symbol = NoSymbol;
                char buffer[8] {};
                XComposeStatus compose {};
                XLookupString(&event.xkey, buffer, sizeof(buffer) - 1, &symbol,
                              &compose);
                if (symbol == XK_Return || symbol == XK_KP_Enter
                    || symbol == XK_ISO_Enter)
                    ui.key(ConnectScreen::Key::enter);
                else if (symbol == XK_Escape)
                    ui.key(ConnectScreen::Key::escape);
                break;
            }
            default:
                break;
            }
            action = ui.take_action();
        }
        return action;
    };

    // Trust on first use: an unknown or changed broker certificate is put to
    // the user in this window. Closing it is a refusal.
    const TrustPrompt ask_trust = [&](const BrokerCheck& check) -> TrustChoice {
        ui.show_trust_prompt(check);
        const ConnectScreen::Action action = wait_for_action();
        ui.clear_trust_prompt();
        if (closed)
            return TrustChoice::reject;
        if (action == ConnectScreen::Action::trust)
            return TrustChoice::trust;
        if (action == ConnectScreen::Action::replace)
            return TrustChoice::replace;
        return TrustChoice::reject;
    };

    blit(); // first frame before the first (blocking) step
    out.result = connect_with_progress(profile, flow, on_progress, nullptr,
                                       ask_trust);

    if (!out.result.ok) {
        // The error screen: wait for Back / Retry (or a window close == Back).
        const ConnectScreen::Action action = closed ? ConnectScreen::Action::back
                                                    : wait_for_action();
        out.action = closed ? ConnectScreen::Action::back : action;
    }

    XFreeGC(display, gc);
    XDestroyImage(image); // frees image_data
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return out;
}

// Show the connection library in its own window and block until the user picks a
// connection or closes the window. Returns the chosen profile, or nullopt when
// the window was closed without connecting. The library screen is drawn with the
// same Surface + TextEngine software renderer the session uses -- the X11 code
// here only translates events and blits the finished frame, exactly as the
// session loop does.
std::optional<ConnectionProfile> run_library(ConnectionLibrary& library)
{
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "could not open X display\n";
        return std::nullopt;
    }
    const int screen = DefaultScreen(display);
    const int sw = DisplayWidth(display, screen);
    const int sh = DisplayHeight(display, screen);
    const int width = std::min(1000, std::max(640, sw - 80));
    const int height = std::min(720, std::max(480, sh - 120));

    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen), 0, 0,
        static_cast<unsigned>(width), static_cast<unsigned>(height), 0,
        BlackPixel(display, screen), BlackPixel(display, screen));
    XStoreName(display, window, "DeBeOS Remote \xE2\x80\x94 Connections");
    XSelectInput(display, window,
                 ExposureMask | KeyPressMask | ButtonPressMask
                     | PointerMotionMask | StructureNotifyMask);
    const Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, const_cast<Atom*>(&wm_delete), 1);
    XMapWindow(display, window);

    auto* image_data = static_cast<char*>(std::calloc(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 4));
    if (image_data == nullptr) {
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return std::nullopt;
    }
    XImage* image = XCreateImage(
        display, DefaultVisual(display, screen),
        static_cast<unsigned>(DefaultDepth(display, screen)), ZPixmap, 0,
        image_data, static_cast<unsigned>(width),
        static_cast<unsigned>(height), 32, 0);
    if (image == nullptr) {
        std::free(image_data);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return std::nullopt;
    }
    GC gc = XCreateGC(display, window, 0, nullptr);

    LibraryScreen ui(library, width, height);
    std::optional<ConnectionProfile> chosen;
    bool running = true;
    bool closed = false;
    while (running) {
        if (ui.dirty()) {
            const Surface& surface = ui.render();
            copy_surface_to_image(surface, *image, true);
            XPutImage(display, window, gc, image, 0, 0, 0, 0,
                      static_cast<unsigned>(width),
                      static_cast<unsigned>(height));
            XFlush(display);
        }

        XEvent event {};
        XNextEvent(display, &event);
        switch (event.type) {
        case Expose:
            ui.mark_dirty();
            break;
        case ClientMessage:
            if (static_cast<Atom>(event.xclient.data.l[0]) == wm_delete) {
                running = false;
                closed = true;
            }
            break;
        case MotionNotify:
            ui.pointer_move(event.xmotion.x, event.xmotion.y);
            break;
        case ButtonPress:
            if (event.xbutton.button == Button1)
                ui.pointer_press(event.xbutton.x, event.xbutton.y);
            break;
        case KeyPress: {
            char text_buffer[64] {};
            KeySym symbol = NoSymbol;
            XComposeStatus compose {};
            const int length = XLookupString(
                &event.xkey, text_buffer,
                static_cast<int>(sizeof(text_buffer) - 1), &symbol, &compose);
            LibraryScreen::Key key = LibraryScreen::Key::none;
            switch (symbol) {
            case XK_BackSpace: key = LibraryScreen::Key::backspace; break;
            case XK_Delete:
            case XK_KP_Delete: key = LibraryScreen::Key::del; break;
            case XK_Return:
            case XK_ISO_Enter:
            case XK_KP_Enter: key = LibraryScreen::Key::enter; break;
            case XK_Tab:
                key = (event.xkey.state & ShiftMask) != 0
                          ? LibraryScreen::Key::back_tab
                          : LibraryScreen::Key::tab;
                break;
            case XK_ISO_Left_Tab: key = LibraryScreen::Key::back_tab; break;
            case XK_Up:
            case XK_KP_Up: key = LibraryScreen::Key::up; break;
            case XK_Down:
            case XK_KP_Down: key = LibraryScreen::Key::down; break;
            case XK_Escape: key = LibraryScreen::Key::escape; break;
            default: break;
            }
            if (key != LibraryScreen::Key::none)
                ui.key(key);
            else if (length > 0
                     && static_cast<unsigned char>(text_buffer[0]) >= 0x20)
                ui.text_input(std::string(
                    text_buffer, static_cast<std::size_t>(length)));
            break;
        }
        default:
            break;
        }

        if (auto request = ui.take_connect_request()) {
            chosen = std::move(request);
            running = false;
        }
    }

    XFreeGC(display, gc);
    XDestroyImage(image); // frees image_data
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return closed ? std::nullopt : chosen;
}

int main(int argc, char** argv)
{
    try {
        const Options options = parse_options(argc, argv);
        // A connection named on the command line connects straight away: the
        // automation/debugging path is unchanged.
        if (options.target_specified)
            return run_session(options);

        // No CLI target: open the connection library. Picking a connection
        // runs a session, after which the library reopens so the app stays
        // usable; closing the library window exits.
        ConnectionLibrary library;
        const LoadResult load = library.load();
        if (load.status == LoadStatus::recovered)
            std::cerr << "connection library: " << load.message << '\n';

        for (;;) {
            auto picked = run_library(library);
            if (!picked)
                return exit_status::ok;

            // Connect with a visible progress screen, then run the session. The
            // ManagedConnection the connect produced owns any ssh child
            // (the tunnel route); it lives inside `outcome.result` and is kept in scope
            // for the whole run_session_loop() call, which is what keeps the
            // tunnel up -- dropping `outcome` at the end tears it down, so the
            // ssh child is never orphaned. A connect failure shows an actionable
            // error with Back/Retry instead of a black window; Retry re-runs the
            // connect for the same profile, Back returns to the library. Either
            // way the failure is recorded on the library card too.
            for (;;) {
                ConnectOutcome outcome = run_connect(*picked);
                if (outcome.result.ok) {
                    std::cerr << "route: " << outcome.result.route_note << '\n';
                    Options session_options = options;
                    session_options.width = picked->width;
                    session_options.height = picked->height;
                    bool refused = false;
                    const int status = run_session_loop(
                        session_options, outcome.result.transport, refused);
                    if (status == exit_status::ok) {
                        library.mark_connected(picked->id);
                    } else if (refused) {
                        const ConnectError e = classify_connect_failure(
                            ConnectFailPoint::session_refused, ConnectFailure::none,
                            "the server closed the connection before sending any"
                            " drawing");
                        library.set_last_error(picked->id, e.summary());
                        std::cerr << "session for '" << picked->name
                                  << "' was refused: " << e.summary() << '\n';
                    } else {
                        library.set_last_error(
                            picked->id, "Last attempt failed (exit "
                                            + std::to_string(status) + ").");
                        std::cerr << "session for '" << picked->name
                                  << "' ended with status " << status << '\n';
                    }
                    (void)library.save();
                    break;
                }

                // Connect failed: the error screen already told the user; record
                // it on the card as well.
                library.set_last_error(picked->id, outcome.result.error.summary());
                std::cerr << "could not connect '" << picked->name
                          << "': " << outcome.result.error.raw << '\n';
                (void)library.save();
                if (outcome.action == ConnectScreen::Action::retry)
                    continue; // retry the same profile
                break;        // Back to the library
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return exit_status::usage;
    }
}
