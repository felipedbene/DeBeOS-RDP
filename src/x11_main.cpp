#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/transport.hpp"

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
        if (parse_transport_argument(options.transport, argument, value)) {}
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

int main(int argc, char** argv)
{
    try {
        const auto options = parse_options(argc, argv);
        std::string socket_error;
        const auto transport = make_transport(options.transport, socket_error);
        if (transport == nullptr) {
            std::cerr << socket_error << '\n';
            return exit_status::failed;
        }
        if (!transport->connect(socket_error)) {
            std::cerr << "connect to " << transport->describe() << " failed: "
                      << socket_error << '\n';
            // See exit_status_usage(): a credential this client was never given
            // is its own exit code, so a harness can tell a local misinvocation
            // from a refusal out on the wire.
            return connect_exit_status(*transport);
        }

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
                session_failed = !report_session_end(
                    transport->peer_closed() || session.server_closed(),
                    session.message_count(), socket_error);
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
