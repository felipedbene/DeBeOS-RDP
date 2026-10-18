// The X11 front end: the same app as the SDL one, drawn with Xlib.
//
// Everything that is not a window-system call is shared with the SDL front end
// rather than reimplemented here: the connection library (LibraryScreen), the
// progress window (ConnectScreen over AsyncConnect), the in-session toolbar
// (DesktopShell), the desktop's state machine with its reconnects and live
// resolution changes (DesktopController), the library -> connect -> desktop flow
// (run_profile_flow), and the HAIKU_REMOTE_TEST_SCRIPT hooks the end-to-end test
// drives both front ends with. What is here is the Xlib around them: windows and
// XImages, event translation, the real X selections behind copy and paste, and
// the native pointer.

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/connect_screen.hpp"
#include "haiku_remote/desktop_session.hpp"
#include "haiku_remote/desktop_shell.hpp"
#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/launch_arguments.hpp"
#include "haiku_remote/library_screen.hpp"
#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/profile_library.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/test_script.hpp"
#include "haiku_remote/transport.hpp"
#include "haiku_remote/viewport.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace haiku_remote;

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto present_interval = std::chrono::milliseconds(16);
constexpr auto batch_idle_time = std::chrono::milliseconds(6);
constexpr auto maximum_frame_latency = std::chrono::milliseconds(32);

struct Options {
    // == launch.transport once parsed; what the endpoint path connects with.
    TransportOptions transport;
    int width = 1280;
    int height = 800;
    bool size_explicit = false;
    bool stats = false;
    // The shared launch arguments (launch_arguments.hpp) and what they select,
    // exactly as the SDL front end parses them: nothing connection-selecting
    // opens the library, --profile / --ssh-host connect that profile at once,
    // and the endpoint arguments connect straight away.
    LaunchArguments launch;
    LaunchTarget target = LaunchTarget::library;
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
        if (parse_launch_argument(options.launch, argument, value)) {
        } else if (argument == "--width") {
            options.width = parse_integer(value(), "width", 1, Surface::max_dimension);
            options.size_explicit = true;
        } else if (argument == "--height") {
            options.height = parse_integer(value(), "height", 1, Surface::max_dimension);
            options.size_explicit = true;
        } else if (argument == "--stats") {
            options.stats = true;
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: haiku-remote-x11" << transport_usage()
                      << "\n  [--width PX] [--height PX] [--stats]\n"
                      << launch_usage()
                      << "\n  [--profiles connections.json]   alone: open the"
                         " library from that file\n"
                         "  (no connection argument at all opens the"
                         " connection library)\n\n"
                      << exit_status_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    options.target = resolve_launch_target(options.launch);
    options.transport = options.launch.transport;
    return options;
}

// --- quitting ------------------------------------------------------------------
//
// Closing a window and quitting the application are different requests (issue
// #1): closing a desktop returns to the library, SIGTERM and SIGINT quit, tearing
// the session and its tunnel down on the way out. The handler only sets a flag
// and pokes a pipe every event wait below polls, so a blocked wait ends at once.

std::atomic<bool> quit_signalled {false};
int signal_pipe[2] = {-1, -1};

void on_quit_signal(int)
{
    quit_signalled.store(true);
    if (signal_pipe[1] >= 0) {
        const char byte = 'q';
        [[maybe_unused]] const auto written = ::write(signal_pipe[1], &byte, 1);
    }
}

void install_quit_signals()
{
    if (::pipe(signal_pipe) == 0) {
        for (const int fd : signal_pipe)
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    struct sigaction action {};
    action.sa_handler = on_quit_signal;
    sigemptyset(&action.sa_mask);
    ::sigaction(SIGTERM, &action, nullptr);
    ::sigaction(SIGINT, &action, nullptr);
}

// The first time a window loop sees the signal: say so once, like the SDL front
// end does when SDL turns the signal into SDL_QUIT.
bool quit_now()
{
    static bool reported = false;
    if (!quit_signalled.load())
        return false;
    if (!reported)
        std::cerr << "quit requested (SIGTERM, SIGINT or the system):"
                     " ending the session and exiting\n";
    reported = true;
    return true;
}

// A pipe a worker thread writes to and an event wait polls: how a connect
// worker's progress ends the UI thread's wait without a timer.
class WakePipe {
public:
    WakePipe()
    {
        if (::pipe(fds_) != 0) {
            fds_[0] = fds_[1] = -1;
            return;
        }
        for (const int fd : fds_)
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    ~WakePipe()
    {
        for (const int fd : fds_)
            if (fd >= 0)
                ::close(fd);
    }
    WakePipe(const WakePipe&) = delete;
    WakePipe& operator=(const WakePipe&) = delete;

    [[nodiscard]] int fd() const { return fds_[0]; }
    void wake() const
    {
        if (fds_[1] < 0)
            return;
        const char byte = 'w';
        [[maybe_unused]] const auto written = ::write(fds_[1], &byte, 1);
    }
    void drain() const
    {
        char sink[64];
        while (fds_[0] >= 0 && ::read(fds_[0], sink, sizeof sink) > 0) {
        }
    }

private:
    int fds_[2] = {-1, -1};
};

// Block until the display has an event, `extra` (if >= 0) is readable, a quit
// signal arrives, or `timeout_ms` passes (< 0: no limit).
void wait_for_events(Display* display, int extra, int timeout_ms)
{
    if (XPending(display) > 0)
        return;
    pollfd fds[3] = {{ConnectionNumber(display), POLLIN, 0},
                     {signal_pipe[0], POLLIN, 0},
                     {extra, POLLIN, 0}};
    const nfds_t count = extra >= 0 ? 3 : 2;
    ::poll(fds, count, timeout_ms);
    if ((fds[1].revents & POLLIN) != 0) {
        char sink[16];
        while (::read(signal_pipe[0], sink, sizeof sink) > 0) {
        }
    }
}

// --- pixels ----------------------------------------------------------------------

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

// Copy `surface` into the BGRA rows at `target` (`stride` bytes apart, at least
// as large), returning the rows and columns that changed; `force_full` copies
// and reports everything.
Damage copy_surface_rows(const Surface& surface, std::uint8_t* target, int stride,
                         int width, int height, bool force_full)
{
    const auto source = surface.pixels();
    width = std::min(surface.width(), width);
    height = std::min(surface.height(), height);
    const auto row_bytes = static_cast<std::size_t>(width) * 4;
    if (force_full) {
        for (int y = 0; y < height; ++y)
            std::memcpy(target + static_cast<std::ptrdiff_t>(y) * stride,
                        source.data() + static_cast<std::size_t>(y * surface.stride()),
                        row_bytes);
        return {0, 0, width - 1, height - 1};
    }
    Damage damage;
    for (int y = 0; y < height; ++y) {
        const auto* source_row = source.data() + static_cast<std::size_t>(y * surface.stride());
        auto* target_row = target + static_cast<std::ptrdiff_t>(y) * stride;
        if (std::memcmp(source_row, target_row, row_bytes) == 0)
            continue;
        int left = 0;
        while (left < width
               && std::memcmp(source_row + left * 4, target_row + left * 4, 4) == 0)
            ++left;
        int right = width - 1;
        while (right > left
               && std::memcmp(source_row + right * 4, target_row + right * 4, 4) == 0)
            --right;
        std::memcpy(target_row + left * 4, source_row + left * 4,
                    static_cast<std::size_t>((right - left + 1) * 4));
        damage.include(left, y, right, y);
    }
    return damage;
}

// Whether `image` holds pixels exactly as a Surface does: 32-bit little-endian
// TrueColor, the common visual, where BGRA bytes are copied as they are.
bool native_bgra(const XImage& image)
{
    return image.bits_per_pixel == 32 && image.byte_order == LSBFirst
        && image.red_mask == 0x00ff0000 && image.green_mask == 0x0000ff00
        && image.blue_mask == 0x000000ff;
}

// Place the BGRA pixels of `area` (rows `stride` bytes apart, the first byte
// pixel (0, 0)) into `image` on a visual that is not native_bgra(), one
// XPutPixel each -- raw device values, never converted through a colour space.
void store_pixels(XImage& image, const std::uint8_t* bgra, int stride, IntRect area)
{
    for (int y = area.top; y <= area.bottom; ++y) {
        const std::uint8_t* row = bgra + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = area.left; x <= area.right; ++x) {
            const std::uint8_t* p = row + static_cast<std::ptrdiff_t>(x) * 4;
            XPutPixel(&image, x, y,
                      (static_cast<unsigned long>(p[2]) << 16)
                          | (static_cast<unsigned long>(p[1]) << 8) | p[0]);
        }
    }
}

// Copy `surface` into `image` (which is at least as large), returning the rows
// and columns that changed. Raw device pixels both ways: on the common 32-bit
// little-endian TrueColor visual the bytes are copied as they are, and on any
// other visual each pixel is placed with XPutPixel -- never converted through a
// colour space.
Damage copy_surface_to_image(const Surface& surface, XImage& image, bool force_full)
{
    if (native_bgra(image))
        return copy_surface_rows(surface, reinterpret_cast<std::uint8_t*>(image.data),
                                 image.bytes_per_line, image.width, image.height,
                                 force_full);
    const int width = std::min(surface.width(), image.width);
    const int height = std::min(surface.height(), image.height);
    store_pixels(image, surface.pixels().data(), surface.stride(),
                 {0, 0, width - 1, height - 1});
    return {0, 0, width - 1, height - 1};
}

// A top-level window and the XImage it is painted from, the size of what it
// shows: the screen a library or progress window renders, or for a desktop the
// whole window, letterbox bars included.
class XWindow {
public:
    XWindow(Display* display, const std::string& title, int width, int height,
            long events, int min_width, int min_height, bool resizable)
        : display_(display), screen_(DefaultScreen(display)),
          window_w_(width), window_h_(height)
    {
        window_ = XCreateSimpleWindow(display, RootWindow(display, screen_), 0, 0,
                                      static_cast<unsigned>(width),
                                      static_cast<unsigned>(height), 0,
                                      BlackPixel(display, screen_),
                                      BlackPixel(display, screen_));
        XStoreName(display, window_, title.c_str());
        XSelectInput(display, window_, events);
        XSizeHints hints {};
        hints.flags = PMinSize;
        hints.min_width = resizable ? std::min(width, min_width) : width;
        hints.min_height = resizable ? std::min(height, min_height) : height;
        if (!resizable) {
            hints.flags |= PMaxSize;
            hints.max_width = width;
            hints.max_height = height;
        }
        XSetWMNormalHints(display, window_, &hints);
        wm_delete_ = XInternAtom(display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display, window_, &wm_delete_, 1);
        gc_ = XCreateGC(display, window_, 0, nullptr);
        ensure_image(width, height);
        XMapWindow(display, window_);
        current() = this;
    }

    ~XWindow()
    {
        if (current() == this)
            current() = nullptr;
        if (image_ != nullptr)
            XDestroyImage(image_); // frees its data
        XFreeGC(display_, gc_);
        XDestroyWindow(display_, window_);
        XFlush(display_);
    }

    XWindow(const XWindow&) = delete;
    XWindow& operator=(const XWindow&) = delete;

    [[nodiscard]] Display* display() const { return display_; }
    [[nodiscard]] Window id() const { return window_; }
    [[nodiscard]] XImage& image() { return *image_; }
    [[nodiscard]] int window_width() const { return window_w_; }
    [[nodiscard]] int window_height() const { return window_h_; }

    // The XImage, reallocated when the size it must hold changed (true then).
    // A fresh one is all zero, so the next copy reports the whole area as damage.
    bool ensure_image(int width, int height)
    {
        if (image_ != nullptr && image_->width == width && image_->height == height)
            return false;
        if (image_ != nullptr)
            XDestroyImage(image_);
        image_ = make_image(width, height);
        return true;
    }

    [[nodiscard]] XImage* make_image(int width, int height) const
    {
        auto* data = static_cast<char*>(std::calloc(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 4));
        if (data == nullptr)
            throw std::bad_alloc();
        XImage* image = XCreateImage(
            display_, DefaultVisual(display_, screen_),
            static_cast<unsigned>(DefaultDepth(display_, screen_)), ZPixmap, 0, data,
            static_cast<unsigned>(width), static_cast<unsigned>(height), 32, 0);
        if (image == nullptr) {
            std::free(data);
            throw std::runtime_error("could not create XImage");
        }
        return image;
    }

    void put(int x, int y, int w, int h)
    {
        if (w <= 0 || h <= 0)
            return;
        XPutImage(display_, window_, gc_, image_, x, y, x, y,
                  static_cast<unsigned>(w), static_cast<unsigned>(h));
    }

    // The whole of `surface` into the image and onto the window.
    void blit(const Surface& surface)
    {
        ensure_image(surface.width(), surface.height());
        copy_surface_to_image(surface, *image_, true);
        put(0, 0, surface.width(), surface.height());
        XFlush(display_);
    }

    [[nodiscard]] bool is_close(const XEvent& event) const
    {
        return event.type == ClientMessage
            && static_cast<Atom>(event.xclient.data.l[0]) == wm_delete_;
    }

    // A ConfigureNotify: true when the window's size changed.
    bool configured(const XEvent& event)
    {
        if (event.type != ConfigureNotify || event.xconfigure.window != window_)
            return false;
        if (event.xconfigure.width == window_w_ && event.xconfigure.height == window_h_)
            return false;
        window_w_ = event.xconfigure.width;
        window_h_ = event.xconfigure.height;
        return true;
    }

    // What a user closing the window sends: WM_DELETE_WINDOW, through the
    // server, so the loop sees it exactly as it would from a window manager.
    void request_close()
    {
        XEvent event {};
        event.xclient.type = ClientMessage;
        event.xclient.window = window_;
        event.xclient.message_type = XInternAtom(display_, "WM_PROTOCOLS", False);
        event.xclient.format = 32;
        event.xclient.data.l[0] = static_cast<long>(wm_delete_);
        event.xclient.data.l[1] = CurrentTime;
        XSendEvent(display_, window_, False, NoEventMask, &event);
        XFlush(display_);
    }

    // A real primary-button press at (x, y), through the server.
    void send_click(int x, int y)
    {
        XEvent event {};
        event.xbutton.type = ButtonPress;
        event.xbutton.window = window_;
        event.xbutton.root = RootWindow(display_, screen_);
        event.xbutton.x = x;
        event.xbutton.y = y;
        event.xbutton.button = Button1;
        event.xbutton.same_screen = True;
        event.xbutton.time = CurrentTime;
        XSendEvent(display_, window_, False, ButtonPressMask, &event);
        XFlush(display_);
    }

    // The window open now, for the test script's close step.
    static XWindow*& current()
    {
        static XWindow* window = nullptr;
        return window;
    }

private:
    Display* display_;
    int screen_;
    Window window_ = 0;
    GC gc_ = nullptr;
    Atom wm_delete_ = 0;
    XImage* image_ = nullptr;
    int window_w_;
    int window_h_;
};

// --- the clipboard ----------------------------------------------------------------
//
// The X CLIPBOARD selection, for real: what is copied here can be pasted into
// any other X client and the other way round. A selection is not a buffer that
// can be written and left -- its owner must answer every request for it for as
// long as it owns it -- so ownership lives on a connection of its own, served by
// a thread of its own, for the life of the application: a copy made in the
// library is still pasteable after the library window has closed. (Nothing can
// keep it alive past the process without a clipboard manager; that is X.)
//
// Text is served and requested as UTF8_STRING, with STRING (Latin-1) as the
// fallback in both directions; TARGETS and TIMESTAMP are answered (ICCCM 2.6.2).
// A selection larger than one request can carry crosses as an ICCCM INCR
// transfer (ICCCM 2.7.2), in both directions: served in chunks of at most the
// server's maximum request size, each written when the requestor deletes the
// last, and received chunk by chunk the same way. Either peer may vanish in the
// middle -- the window it used is destroyed -- and that ends the transfer
// cleanly: a paste whose owner went away reads as empty rather than as a
// truncated text, and a requestor that went away is forgotten, together with
// the X errors writing to its window would raise (which Xlib's default handler
// turns into an exit).
class X11Clipboard {
public:
    X11Clipboard()
    {
        display_ = XOpenDisplay(nullptr);
        if (display_ == nullptr)
            return;
        install_error_handler(display_);
        window_ = XCreateSimpleWindow(display_, DefaultRootWindow(display_), -10, -10,
                                      1, 1, 0, 0, 0);
        XSelectInput(display_, window_, PropertyChangeMask);
        clipboard_ = XInternAtom(display_, "CLIPBOARD", False);
        utf8_ = XInternAtom(display_, "UTF8_STRING", False);
        targets_ = XInternAtom(display_, "TARGETS", False);
        text_ = XInternAtom(display_, "TEXT", False);
        timestamp_target_ = XInternAtom(display_, "TIMESTAMP", False);
        incr_ = XInternAtom(display_, "INCR", False);
        property_ = XInternAtom(display_, "HAIKU_REMOTE_SELECTION", False);
        timestamp_ = XInternAtom(display_, "HAIKU_REMOTE_TIMESTAMP", False);
        // One request must not exceed the server's maximum, header included
        // (ICCCM 2.5: INCR above it), and no INCR chunk may either.
        max_bytes_ = static_cast<std::size_t>(XMaxRequestSize(display_)) * 4 - 256;
        XFlush(display_);
        thread_ = std::thread([this] { run(); });
    }

    ~X11Clipboard()
    {
        if (display_ == nullptr)
            return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.wake();
        thread_.join();
        XDestroyWindow(display_, window_);
        XCloseDisplay(display_);
    }

    X11Clipboard(const X11Clipboard&) = delete;
    X11Clipboard& operator=(const X11Clipboard&) = delete;

    // The clipboard's text now; empty when there is none, its owner did not
    // answer, or it went away in the middle of the transfer. The clipboard
    // thread bounds every wait it makes (receive_timeout without progress), so
    // this returns; the cap here is only a backstop.
    std::string get()
    {
        if (display_ == nullptr)
            return local_;
        std::unique_lock<std::mutex> lock(mutex_);
        const std::uint64_t id = ++requested_;
        wake_.wake();
        cv_.wait_for(lock, std::chrono::seconds(60), [&] { return answered_ >= id; });
        return answered_ >= id ? answer_ : std::string();
    }

    // Take the clipboard with `text`.
    void set(std::string text)
    {
        if (display_ == nullptr) {
            local_ = std::move(text);
            return;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        offered_ = std::move(text);
        const std::uint64_t id = ++offers_;
        wake_.wake();
        // Wait until the ownership is taken, so a get() straight after reads
        // this text and not the previous owner's.
        cv_.wait_for(lock, std::chrono::milliseconds(1500),
                     [&] { return taken_ >= id; });
    }

private:
    // How long a transfer may make no progress before it is abandoned: the
    // first reply to a conversion, each INCR chunk received, and each chunk
    // served (the requestor deleting the last one).
    static constexpr auto reply_timeout = std::chrono::milliseconds(1200);
    static constexpr auto receive_timeout = std::chrono::seconds(3);
    static constexpr auto serve_timeout = std::chrono::seconds(5);

    // An INCR transfer being served: what is left of `data` goes to
    // `requestor`'s `property`, one chunk each time the requestor deletes it.
    struct Outgoing {
        Window requestor = None;
        Atom property = None;
        Atom type = None;
        std::string data;
        std::size_t offset = 0;
        Clock::time_point last_progress {};
    };

    // X errors on the clipboard's own connection are expected -- a peer's
    // window can be destroyed between any two requests that name it -- and
    // are dropped; any other display's still reach the handler that was there
    // before (by default Xlib's, which prints the error and exits).
    static Display*& error_display()
    {
        static Display* display = nullptr;
        return display;
    }
    using ErrorHandler = int (*)(Display*, XErrorEvent*);
    static ErrorHandler& previous_handler()
    {
        static ErrorHandler handler = nullptr;
        return handler;
    }
    static int on_error(Display* display, XErrorEvent* error)
    {
        if (display == error_display())
            return 0;
        return previous_handler() != nullptr ? previous_handler()(display, error) : 0;
    }
    static void install_error_handler(Display* display)
    {
        error_display() = display;
        const ErrorHandler previous = XSetErrorHandler(on_error);
        if (previous != on_error)
            previous_handler() = previous;
    }

    void run()
    {
        for (;;) {
            if (XPending(display_) == 0) {
                pollfd fds[2] = {{ConnectionNumber(display_), POLLIN, 0},
                                 {wake_.fd(), POLLIN, 0}};
                const bool timed = converting_ || receiving_ || !outgoing_.empty();
                ::poll(fds, 2, timed ? 100 : -1);
            }
            wake_.drain();
            std::string offer;
            bool do_offer = false;
            bool do_get = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_)
                    return;
                if (offers_ > handled_offers_) {
                    handled_offers_ = offers_;
                    offer = offered_;
                    do_offer = true;
                }
                do_get = requested_ > started_requests_ && !converting_ && !receiving_;
                if (do_get)
                    started_requests_ = requested_;
            }
            if (do_offer)
                take_ownership(std::move(offer));
            if (do_get)
                begin_get();
            while (XPending(display_) > 0) {
                XEvent event {};
                XNextEvent(display_, &event);
                handle(event);
            }
            const auto now = Clock::now();
            if ((converting_ || receiving_) && now >= convert_deadline_) {
                std::cerr << "clipboard: the selection owner stopped answering"
                          << (receiving_ ? " in the middle of an INCR transfer" : "")
                          << "; nothing pasted\n";
                answer({});
            }
            for (std::size_t i = 0; i < outgoing_.size();) {
                if (now - outgoing_[i].last_progress >= serve_timeout) {
                    std::cerr << "clipboard: a requestor stopped reading an INCR"
                                 " transfer; abandoned it\n";
                    drop_outgoing(i);
                } else {
                    ++i;
                }
            }
        }
    }

    // A server timestamp, which ICCCM asks for instead of CurrentTime: a
    // zero-length append to our own property, and the PropertyNotify it causes.
    Time server_time()
    {
        XChangeProperty(display_, window_, timestamp_, XA_STRING, 8, PropModeAppend,
                        nullptr, 0);
        XEvent event {};
        XIfEvent(display_, &event,
                 [](Display*, XEvent* candidate, XPointer arg) -> Bool {
                     return candidate->type == PropertyNotify
                         && candidate->xproperty.atom == *reinterpret_cast<Atom*>(arg);
                 },
                 reinterpret_cast<XPointer>(&timestamp_));
        return event.xproperty.time;
    }

    void take_ownership(std::string text)
    {
        owned_text_ = std::move(text);
        owned_since_ = server_time();
        XSetSelectionOwner(display_, clipboard_, window_, owned_since_);
        own_ = XGetSelectionOwner(display_, clipboard_) == window_;
        if (!own_)
            std::cerr << "clipboard: could not take the X CLIPBOARD selection\n";
        XFlush(display_);
        std::lock_guard<std::mutex> lock(mutex_);
        taken_ = handled_offers_;
        cv_.notify_all();
    }

    void begin_get()
    {
        if (own_) {
            answer(owned_text_);
            return;
        }
        owner_ = XGetSelectionOwner(display_, clipboard_);
        if (owner_ == None) {
            answer({});
            return;
        }
        // Watch the owner's window: its DestroyNotify is how an owner that
        // goes away in the middle of a transfer is noticed at once.
        XSelectInput(display_, owner_, StructureNotifyMask);
        XDeleteProperty(display_, window_, property_);
        tried_string_ = false;
        converting_ = true;
        convert_deadline_ = Clock::now() + reply_timeout;
        XConvertSelection(display_, clipboard_, utf8_, property_, window_, server_time());
        XFlush(display_);
    }

    void answer(std::string text)
    {
        converting_ = false;
        receiving_ = false;
        incoming_.clear();
        incoming_.shrink_to_fit();
        if (owner_ != None) {
            XSelectInput(display_, owner_, NoEventMask);
            owner_ = None;
        }
        XFlush(display_);
        std::lock_guard<std::mutex> lock(mutex_);
        answer_ = std::move(text);
        answered_ = started_requests_;
        cv_.notify_all();
    }

    // The received bytes as UTF-8: STRING is Latin-1, anything else is taken
    // as UTF-8 already.
    std::string decoded(Atom type, std::string bytes) const
    {
        if (type != XA_STRING)
            return bytes;
        return latin1_to_utf8(reinterpret_cast<const unsigned char*>(bytes.data()),
                              bytes.size());
    }

    // The whole of `property` on our window, read in pieces however long it is
    // (deleting it once all is read, when `remove`). False when there is none.
    bool read_property(bool remove, Atom& type, int& format, std::string& out)
    {
        out.clear();
        long offset = 0;
        for (;;) {
            unsigned long count = 0;
            unsigned long after = 0;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(display_, window_, property_, offset, 1L << 18, remove,
                                   AnyPropertyType, &type, &format, &count, &after, &data)
                    != Success
                || type == None) {
                if (data != nullptr)
                    XFree(data);
                return false;
            }
            const std::size_t unit = format == 32 ? sizeof(long) : format == 16 ? 2 : 1;
            if (data != nullptr) {
                out.append(reinterpret_cast<const char*>(data), count * unit);
                XFree(data);
            }
            if (after == 0)
                return true;
            // 32-bit offsets count 4-byte units of the property's wire form.
            offset += static_cast<long>(count * static_cast<unsigned long>(format) / 32);
        }
    }

    static std::string latin1_to_utf8(const unsigned char* data, std::size_t size)
    {
        std::string text;
        for (std::size_t i = 0; i < size; ++i) {
            const unsigned char c = data[i];
            if (c < 0x80) {
                text.push_back(static_cast<char>(c));
            } else {
                text.push_back(static_cast<char>(0xC0 | (c >> 6)));
                text.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
        return text;
    }

    // UTF-8 down to Latin-1 for a STRING request; a character Latin-1 cannot
    // hold becomes '?', as ICCCM leaves it to the owner.
    static std::string utf8_to_latin1(const std::string& text)
    {
        std::string out;
        for (std::size_t i = 0; i < text.size();) {
            const std::int32_t c = first_utf8_scalar(std::string_view(text).substr(i));
            const auto lead = static_cast<unsigned char>(text[i]);
            const std::size_t length = lead < 0x80 ? 1
                : (lead & 0xe0) == 0xc0           ? 2
                : (lead & 0xf0) == 0xe0           ? 3
                : (lead & 0xf8) == 0xf0           ? 4
                                                  : 1;
            out.push_back(c > 0 && c < 0x100 ? static_cast<char>(c) : '?');
            i += length;
        }
        return out;
    }

    // The requestor of transfer `i` is done with, or gone: forget it, and stop
    // listening to its window once no other transfer uses it.
    void drop_outgoing(std::size_t i)
    {
        const Window requestor = outgoing_[i].requestor;
        outgoing_.erase(outgoing_.begin() + static_cast<std::ptrdiff_t>(i));
        const bool still_used = std::any_of(outgoing_.begin(), outgoing_.end(),
                                            [&](const Outgoing& o) {
                                                return o.requestor == requestor;
                                            });
        if (!still_used)
            XSelectInput(display_, requestor, NoEventMask);
        XFlush(display_);
    }

    // The requestor deleted the property of transfer `i`: the next chunk, or
    // after the last one the zero-length property that ends the transfer.
    void serve_next_chunk(std::size_t i)
    {
        Outgoing& out = outgoing_[i];
        const std::size_t length = std::min(max_bytes_, out.data.size() - out.offset);
        XChangeProperty(display_, out.requestor, out.property, out.type, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(out.data.data() + out.offset),
                        static_cast<int>(length));
        out.offset += length;
        out.last_progress = Clock::now();
        if (length == 0)
            drop_outgoing(i);
        else
            XFlush(display_);
    }

    void handle(const XEvent& event)
    {
        switch (event.type) {
        case SelectionClear:
            // Transfers already under way finish with the text they started
            // with (ICCCM 2.7.2 allows either; finishing is kinder).
            if (event.xselectionclear.selection == clipboard_)
                own_ = false;
            break;
        case DestroyNotify: {
            const Window gone = event.xdestroywindow.window;
            if (gone == owner_ && (converting_ || receiving_)) {
                std::cerr << "clipboard: the selection owner went away"
                          << (receiving_ ? " in the middle of an INCR transfer" : "")
                          << "; nothing pasted\n";
                owner_ = None; // its window, and our selection on it, are gone
                answer({});
            }
            for (std::size_t i = 0; i < outgoing_.size();) {
                if (outgoing_[i].requestor == gone) {
                    std::cerr << "clipboard: the requestor went away in the middle of"
                                 " an INCR transfer; abandoned it\n";
                    outgoing_.erase(outgoing_.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            break;
        }
        case PropertyNotify: {
            const XPropertyEvent& property = event.xproperty;
            if (property.window == window_) {
                // A chunk of an INCR transfer we are receiving: read and delete
                // it, which asks for the next; a zero-length one is the end.
                if (receiving_ && property.atom == property_
                    && property.state == PropertyNewValue) {
                    Atom type = None;
                    int format = 0;
                    std::string chunk;
                    if (!read_property(true, type, format, chunk))
                        break;
                    convert_deadline_ = Clock::now() + receive_timeout;
                    if (chunk.empty()) {
                        answer(decoded(incoming_type_, std::move(incoming_)));
                        break;
                    }
                    if (format == 8) {
                        incoming_type_ = type;
                        incoming_ += chunk;
                    }
                    XFlush(display_);
                }
                break;
            }
            if (property.state != PropertyDelete)
                break;
            for (std::size_t i = 0; i < outgoing_.size(); ++i) {
                if (outgoing_[i].requestor == property.window
                    && outgoing_[i].property == property.atom) {
                    serve_next_chunk(i);
                    break;
                }
            }
            break;
        }
        case SelectionNotify: {
            if (!converting_ || event.xselection.requestor != window_)
                break;
            if (event.xselection.property == None) {
                if (!tried_string_) {
                    tried_string_ = true;
                    XConvertSelection(display_, clipboard_, XA_STRING, property_,
                                      window_, event.xselection.time);
                    XFlush(display_);
                } else {
                    answer({});
                }
                break;
            }
            Atom type = None;
            int format = 0;
            std::string bytes;
            // Read and delete: for INCR, the deletion is what starts it.
            if (!read_property(true, type, format, bytes)) {
                answer({});
                break;
            }
            if (type == incr_) {
                converting_ = false;
                receiving_ = true;
                incoming_.clear();
                incoming_type_ = None;
                convert_deadline_ = Clock::now() + receive_timeout;
                XFlush(display_);
                break;
            }
            answer(format == 8 ? decoded(type, std::move(bytes)) : std::string());
            break;
        }
        case SelectionRequest:
            serve(event.xselectionrequest);
            break;
        default:
            break;
        }
    }

    void serve(const XSelectionRequestEvent& request)
    {
        XEvent reply {};
        reply.xselection.type = SelectionNotify;
        reply.xselection.requestor = request.requestor;
        reply.xselection.selection = request.selection;
        reply.xselection.target = request.target;
        reply.xselection.time = request.time;
        reply.xselection.property = None;
        // An obsolete requestor names no property: use the target.
        const Atom property = request.property != None ? request.property : request.target;
        const bool ours = own_ && request.selection == clipboard_
            && (request.time == CurrentTime || request.time >= owned_since_);
        if (ours && request.target == targets_) {
            const Atom list[] = {targets_, timestamp_target_, utf8_, text_, XA_STRING};
            XChangeProperty(display_, request.requestor, property, XA_ATOM, 32,
                            PropModeReplace, reinterpret_cast<const unsigned char*>(list),
                            static_cast<int>(std::size(list)));
            reply.xselection.property = property;
        } else if (ours && request.target == timestamp_target_) {
            // The time the selection was taken (ICCCM 2.6.2), as an INTEGER.
            const long when = static_cast<long>(owned_since_);
            XChangeProperty(display_, request.requestor, property, XA_INTEGER, 32,
                            PropModeReplace, reinterpret_cast<const unsigned char*>(&when), 1);
            reply.xselection.property = property;
        } else if (ours
                   && (request.target == utf8_ || request.target == text_
                       || request.target == XA_STRING)) {
            const Atom type = request.target == XA_STRING ? XA_STRING : utf8_;
            std::string bytes
                = request.target == XA_STRING ? utf8_to_latin1(owned_text_) : owned_text_;
            if (bytes.size() <= max_bytes_) {
                XChangeProperty(display_, request.requestor, property, type, 8,
                                PropModeReplace,
                                reinterpret_cast<const unsigned char*>(bytes.data()),
                                static_cast<int>(bytes.size()));
            } else {
                // INCR (ICCCM 2.7.2): listen for the requestor deleting the
                // property -- and for its window going away -- before telling
                // it, then a lower bound on the size, then the chunks.
                for (std::size_t i = 0; i < outgoing_.size(); ++i) {
                    if (outgoing_[i].requestor == request.requestor
                        && outgoing_[i].property == property) {
                        outgoing_.erase(outgoing_.begin() + static_cast<std::ptrdiff_t>(i));
                        break;
                    }
                }
                XSelectInput(display_, request.requestor,
                             PropertyChangeMask | StructureNotifyMask);
                const long size = static_cast<long>(bytes.size());
                XChangeProperty(display_, request.requestor, property, incr_, 32,
                                PropModeReplace, reinterpret_cast<const unsigned char*>(&size),
                                1);
                Outgoing out;
                out.requestor = request.requestor;
                out.property = property;
                out.type = type;
                out.data = std::move(bytes);
                out.last_progress = Clock::now();
                outgoing_.push_back(std::move(out));
            }
            reply.xselection.property = property;
        }
        XSendEvent(display_, request.requestor, False, NoEventMask, &reply);
        XFlush(display_);
    }

    Display* display_ = nullptr;
    Window window_ = 0;
    Atom clipboard_ = 0, utf8_ = 0, targets_ = 0, text_ = 0, timestamp_target_ = 0;
    Atom incr_ = 0, property_ = 0, timestamp_ = 0;
    std::size_t max_bytes_ = 0;
    WakePipe wake_;
    std::thread thread_;
    std::string local_; // no display: an in-process stand-in

    // The thread's own state.
    bool own_ = false;
    std::string owned_text_;
    Time owned_since_ = CurrentTime;
    // Receiving: waiting for the owner's SelectionNotify (converting_), then,
    // for INCR, for its chunks (receiving_), until convert_deadline_.
    bool converting_ = false;
    bool receiving_ = false;
    bool tried_string_ = false;
    Window owner_ = None;
    std::string incoming_;
    Atom incoming_type_ = None;
    Clock::time_point convert_deadline_ {};
    std::vector<Outgoing> outgoing_;

    // Shared with the UI thread, under mutex_.
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::string offered_;
    std::uint64_t offers_ = 0;
    std::uint64_t handled_offers_ = 0;
    std::uint64_t taken_ = 0;
    std::uint64_t requested_ = 0;
    std::uint64_t started_requests_ = 0;
    std::uint64_t answered_ = 0;
    std::string answer_;
};

// --- keyboard ---------------------------------------------------------------------

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

// Typed text arrives as UTF-8 through an input method when one opens (any
// script, and IMEs with their own preedit window); without one, XLookupString's
// Latin-1 is converted to UTF-8. An input method needs the user's character-type
// locale (LC_CTYPE only: numeric formatting stays "C") and XMODIFIERS.
class InputMethod {
public:
    InputMethod(Display* display, Window window)
    {
        if (std::setlocale(LC_CTYPE, "") == nullptr || XSupportsLocale() == False)
            std::setlocale(LC_CTYPE, "C");
        (void)XSetLocaleModifiers("");
        method_ = XOpenIM(display, nullptr, nullptr, nullptr);
        if (method_ != nullptr)
            context_ = XCreateIC(method_, XNInputStyle,
                                 XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
                                 window, XNFocusWindow, window, nullptr);
        if (context_ != nullptr)
            XSetICFocus(context_);
    }
    ~InputMethod()
    {
        if (context_ != nullptr)
            XDestroyIC(context_);
        if (method_ != nullptr)
            XCloseIM(method_);
    }
    InputMethod(const InputMethod&) = delete;
    InputMethod& operator=(const InputMethod&) = delete;

    struct Key {
        KeySym symbol = NoSymbol;
        std::string text; // UTF-8
    };

    // The key and the text it composes. A release composes through
    // XLookupString: an input method answers presses only.
    Key lookup(XKeyEvent& event) const
    {
        Key key;
        char buffer[64] {};
        if (context_ != nullptr && event.type == KeyPress) {
            Status status = 0;
            const int length = Xutf8LookupString(context_, &event, buffer,
                                                 static_cast<int>(sizeof buffer - 1),
                                                 &key.symbol, &status);
            if (status == XLookupChars)
                key.symbol = NoSymbol;
            if ((status == XLookupChars || status == XLookupBoth) && length > 0)
                key.text.assign(buffer, static_cast<std::size_t>(length));
            return key;
        }
        XComposeStatus compose {};
        const int length = XLookupString(&event, buffer,
                                         static_cast<int>(sizeof buffer - 1),
                                         &key.symbol, &compose);
        for (int i = 0; i < length; ++i) { // Latin-1 -> UTF-8
            const auto c = static_cast<unsigned char>(buffer[i]);
            if (c < 0x80) {
                key.text.push_back(static_cast<char>(c));
            } else {
                key.text.push_back(static_cast<char>(0xC0 | (c >> 6)));
                key.text.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
        return key;
    }

private:
    XIM method_ = nullptr;
    XIC context_ = nullptr;
};

// X has no click count: a press soon after, and near, the last one of the same
// button continues the run (double-click a word, triple-click all).
class ClickCounter {
public:
    int press(const XButtonEvent& event)
    {
        const bool again = event.button == button_ && event.time - time_ < 400
            && std::abs(event.x - x_) <= 4 && std::abs(event.y - y_) <= 4;
        count_ = again ? count_ + 1 : 1;
        time_ = event.time;
        button_ = event.button;
        x_ = event.x;
        y_ = event.y;
        return count_;
    }

private:
    Time time_ = 0;
    unsigned button_ = 0;
    int x_ = -100;
    int y_ = -100;
    int count_ = 0;
};

// --- the native pointer --------------------------------------------------------
//
// Core Xlib has no ARGB cursor -- Xrender and Xcursor are not dependencies of
// this build -- so the server's cursor becomes an XCreatePixmapCursor: the alpha
// channel is its mask and luminance picks black or white per pixel. Shape and
// hotspot are exact; only colour is reduced. Returns None when the bitmap cannot
// be represented, in which case the caller keeps the host pointer.
::Cursor build_x_cursor(Display* display, Window window, const CursorState& cursor)
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
                         static_cast<unsigned>(height), &best_width, &best_height)
            == 0
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
            const auto pixel = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                                + static_cast<std::size_t>(x))
                * 4;
            const unsigned blue = cursor.bitmap.bgra[pixel];
            const unsigned green = cursor.bitmap.bgra[pixel + 1];
            const unsigned red = cursor.bitmap.bgra[pixel + 2];
            const unsigned alpha = cursor.bitmap.bgra[pixel + 3];
            if (alpha < 128)
                continue;
            const auto index
                = static_cast<std::size_t>(y) * row_bytes + static_cast<std::size_t>(x / 8);
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
        raster_coordinate(std::floor(static_cast<double>(cursor.hotspot.x)), 0), 0,
        width - 1);
    const int hotspot_y = std::clamp(
        raster_coordinate(std::floor(static_cast<double>(cursor.hotspot.y)), 0), 0,
        height - 1);
    const ::Cursor result = XCreatePixmapCursor(
        display, source_pixmap, mask_pixmap, &foreground, &background,
        static_cast<unsigned>(hotspot_x), static_cast<unsigned>(hotspot_y));
    XFreePixmap(display, source_pixmap);
    XFreePixmap(display, mask_pixmap);
    return result;
}

// A 1x1 cursor with an empty mask: the pointer is still there and still delivers
// motion, it just draws nothing. That is what RP_SET_CURSOR_VISIBLE false means.
::Cursor build_invisible_cursor(Display* display, Window window)
{
    const char nothing = 0;
    const Pixmap empty = XCreateBitmapFromData(display, window, &nothing, 1, 1);
    XColor black {};
    const ::Cursor result = XCreatePixmapCursor(display, empty, empty, &black, &black, 0, 0);
    XFreePixmap(display, empty);
    return result;
}

// --- --stats -------------------------------------------------------------------

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

// The --stats title-bar report: what presenting costs on this X server.
struct PerformanceStats {
    Clock::time_point interval_start = Clock::now();
    std::size_t previous_message_count = 0;
    std::size_t previous_draw_string_replies = 0;
    std::uint64_t frames = 0;
    std::uint64_t damaged_pixels = 0;
    std::uint64_t full_frame_pixels = 0;
    Samples copy;
    Samples x11;
    Samples input_present;

    std::string report(std::size_t message_count, std::size_t draw_string_replies,
                       Clock::time_point now)
    {
        const double seconds = std::chrono::duration<double>(now - interval_start).count();
        const auto messages = message_count - previous_message_count;
        // Each of these blocked app_server's drawing thread for a round trip of
        // this link, so they are reported as a rate.
        const auto strings = draw_string_replies - previous_draw_string_replies;
        std::ostringstream text;
        text << std::fixed << std::setprecision(1) << frames / seconds << " fps"
             << " | " << messages / seconds << " msg/s"
             << " | copy " << copy.average() << " ms"
             << " | X11 " << x11.average() << " ms"
             << " | damage "
             << (frames == 0 || full_frame_pixels == 0
                     ? 0
                     : 100.0 * damaged_pixels
                         / static_cast<double>(frames * full_frame_pixels))
             << '%';
        if (input_present.count != 0)
            text << " | input " << input_present.average() << '/'
                 << input_present.maximum_ms << " ms";
        text << " | sync " << strings << " str | total " << message_count;
        interval_start = now;
        previous_message_count = message_count;
        previous_draw_string_replies = draw_string_replies;
        frames = 0;
        damaged_pixels = 0;
        copy = {};
        x11 = {};
        input_present = {};
        return text.str();
    }
};

} // namespace

// --- the connection library ----------------------------------------------------

// Show the connection library and block until the user picks a connection or
// closes the window. The window is resizable down to the size the list still
// lays out at, and the screen re-lays itself out at every new size; copy, cut
// and paste go through the X CLIPBOARD selection.
std::optional<ConnectionProfile> run_library(ConnectionLibrary& library,
                                             X11Clipboard& clipboard)
{
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "could not open X display\n";
        return std::nullopt;
    }
    const int screen = DefaultScreen(display);
    // 1024x700 is the size issue #1 requires the library to be usable at.
    const int width = std::min(1024, DisplayWidth(display, screen));
    const int height = std::min(700, DisplayHeight(display, screen));

    std::optional<ConnectionProfile> chosen;
    bool closed = false;
    {
        XWindow window(display, "DeBeOS Remote \xE2\x80\x94 Connections", width, height,
                       ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask
                           | PointerMotionMask | StructureNotifyMask | FocusChangeMask,
                       LibraryScreen::min_window_width, LibraryScreen::min_window_height,
                       true);
        LibraryScreen ui(library, width, height);
        ui.set_clipboard(
            [&clipboard] {
                std::string text = clipboard.get();
                // HAIKU_REMOTE_TEST_SCRIPT's clipboard:echo -- see test_script.hpp.
                if (test_script_take("clipboard") == std::optional<std::string>("echo")) {
                    std::cerr << "e2e: clipboard echo read " << text.size() << " bytes\n";
                    if (!text.empty()) {
                        clipboard.set(text);
                        std::cerr << "e2e: clipboard echo took the selection with them\n";
                    }
                    return std::string();
                }
                return text;
            },
            [&clipboard](const std::string& text) { clipboard.set(text); });
        InputMethod input(display, window.id());
        ClickCounter clicks;

        if (test_script_active())
            std::cerr << "e2e: library shown with " << library.size() << " profiles\n";
        bool running = true;
        while (running) {
            if (quit_now()) {
                closed = true;
                break;
            }
            if (ui.dirty()) {
                window.blit(ui.render());
                test_script_library_step(ui, library, [&window](int x, int y) {
                    window.send_click(x, y);
                });
            }
            // Sleep on the display until an event arrives or the caret's blink
            // phase is up -- no polling loop.
            if (XPending(display) == 0) {
                wait_for_events(display, -1, ui.next_wake_ms());
                if (XPending(display) == 0) {
                    ui.tick();
                    continue;
                }
            }
            XEvent event {};
            XNextEvent(display, &event);
            if (XFilterEvent(&event, None) == True)
                continue; // the input method consumed it
            switch (event.type) {
            case Expose:
                ui.mark_dirty();
                break;
            case ConfigureNotify:
                if (window.configured(event)) {
                    ui.resize(window.window_width(), window.window_height());
                    ui.mark_dirty();
                }
                break;
            case FocusIn:
                ui.set_window_active(true);
                break;
            case FocusOut:
                ui.set_window_active(false);
                break;
            case ClientMessage:
                if (window.is_close(event)) {
                    running = false;
                    closed = true;
                }
                break;
            case MotionNotify:
                ui.pointer_move(event.xmotion.x, event.xmotion.y);
                break;
            case ButtonPress:
                if (event.xbutton.button == Button1) {
                    ui.pointer_press(event.xbutton.x, event.xbutton.y,
                                     clicks.press(event.xbutton),
                                     (event.xbutton.state & ShiftMask) != 0
                                         ? LibraryScreen::mod_shift
                                         : LibraryScreen::mod_none);
                } else if (event.xbutton.button == Button4
                           || event.xbutton.button == Button5) {
                    ui.wheel(event.xbutton.button == Button4 ? 1.0f : -1.0f);
                }
                break;
            case ButtonRelease:
                if (event.xbutton.button == Button1)
                    ui.pointer_release(event.xbutton.x, event.xbutton.y);
                break;
            case MappingNotify:
                XRefreshKeyboardMapping(&event.xmapping);
                break;
            case KeyPress: {
                const InputMethod::Key typed = input.lookup(event.xkey);
                using Key = LibraryScreen::Key;
                const bool shift = (event.xkey.state & ShiftMask) != 0;
                const bool control = (event.xkey.state & ControlMask) != 0;
                unsigned mods = shift ? LibraryScreen::mod_shift : LibraryScreen::mod_none;
                if (control)
                    mods |= LibraryScreen::mod_word;
                Key key = Key::none;
                switch (typed.symbol) {
                case XK_BackSpace: key = Key::backspace; break;
                case XK_Delete:
                case XK_KP_Delete: key = shift ? Key::cut : Key::del; break;
                case XK_Insert:
                case XK_KP_Insert:
                    key = shift ? Key::paste : control ? Key::copy : Key::none;
                    break;
                case XK_Return:
                case XK_ISO_Enter:
                case XK_KP_Enter: key = Key::enter; break;
                case XK_Tab: key = shift ? Key::back_tab : Key::tab; break;
                case XK_ISO_Left_Tab: key = Key::back_tab; break;
                case XK_Up:
                case XK_KP_Up: key = Key::up; break;
                case XK_Down:
                case XK_KP_Down: key = Key::down; break;
                case XK_Left:
                case XK_KP_Left: key = Key::left; break;
                case XK_Right:
                case XK_KP_Right: key = Key::right; break;
                case XK_Home:
                case XK_KP_Home: key = Key::home; break;
                case XK_End:
                case XK_KP_End: key = Key::end; break;
                case XK_Page_Up:
                case XK_KP_Page_Up: key = Key::page_up; break;
                case XK_Page_Down:
                case XK_KP_Page_Down: key = Key::page_down; break;
                case XK_Escape: key = Key::escape; break;
                case XK_a:
                case XK_A: key = control ? Key::select_all : Key::none; break;
                case XK_c:
                case XK_C: key = control ? Key::copy : Key::none; break;
                case XK_x:
                case XK_X: key = control ? Key::cut : Key::none; break;
                case XK_v:
                case XK_V: key = control ? Key::paste : Key::none; break;
                default: break;
                }
                if (key != Key::none)
                    ui.key(key, mods);
                else if (!control && !typed.text.empty()
                         && static_cast<unsigned char>(typed.text[0]) >= 0x20)
                    ui.text_input(typed.text);
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
    }
    XCloseDisplay(display);
    return closed ? std::nullopt : chosen;
}

// --- the connection-progress window --------------------------------------------

// Run the connect for `profile` on a worker thread (AsyncConnect), painting the
// progress checklist, the elapsed time and a Cancel button while ssh and the
// sockets block. Cancel (the button, Escape, or closing the window) stops the
// attempt: its ssh child is killed and reaped, its socket shut down, and nothing
// is left running when this returns. On failure the window stays up showing the
// actionable error and the diagnostic log until the user picks Back or Retry (or
// closes it, treated as Back); Copy diagnostics puts the whole log on the
// clipboard. The same screen and the same worker as the SDL front end.
ConnectOutcome run_connect(const ConnectionProfile& profile, X11Clipboard& clipboard)
{
    ConnectOutcome out;
    const std::string title = profile.name.empty() ? profile.host : profile.name;
    ConnectOptions options;
    options.session_log = [](std::string_view line) { write_stderr_line(line); };

    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        // No display: connect headless so a session can still be attempted, and
        // report any failure through the library card rather than a window.
        std::cerr << "could not open X display for the connect screen\n";
        AsyncConnect attempt(profile, options, [] {});
        attempt.wait();
        out.result = attempt.take_result();
        if (!out.result.ok)
            out.action = ConnectScreen::Action::back;
        return out;
    }
    const int screen = DefaultScreen(display);
    const int width = std::min(900, DisplayWidth(display, screen));
    const int height = std::min(560, DisplayHeight(display, screen));

    {
        XWindow window(display, "DeBeOS Remote \xE2\x80\x94 Connecting", width, height,
                       ExposureMask | KeyPressMask | ButtonPressMask | PointerMotionMask
                           | StructureNotifyMask,
                       width, height, false);
        // The screen draws `shown`, the UI thread's copy of the worker's flow.
        ConnectFlow shown(plan_launch(profile));
        ConnectScreen ui(shown, title, width, height);
        bool closed = false;

        // One translation of X input into ConnectScreen input, for every phase.
        const auto handle = [&](XEvent& event) {
            switch (event.type) {
            case Expose:
                ui.mark_dirty();
                break;
            case ClientMessage:
                if (window.is_close(event))
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
                XLookupString(&event.xkey, buffer, sizeof buffer - 1, &symbol, &compose);
                if (symbol == XK_Return || symbol == XK_KP_Enter || symbol == XK_ISO_Enter)
                    ui.key(ConnectScreen::Key::enter);
                else if (symbol == XK_Escape)
                    ui.key(ConnectScreen::Key::escape);
                else if (symbol == XK_ISO_Left_Tab
                         || (symbol == XK_Tab && (event.xkey.state & ShiftMask) != 0))
                    ui.key(ConnectScreen::Key::back_tab);
                else if (symbol == XK_Tab)
                    ui.key(ConnectScreen::Key::tab);
                break;
            }
            default:
                break;
            }
        };

        WakePipe wake;
        AsyncConnect attempt(profile, options, [&wake] { wake.wake(); });

        bool prompt_showing = false;
        bool refresh = true; // the worker published, or the clock ticked
        std::size_t stages_reported = 0;
        while (!attempt.finished()) {
            if (refresh) {
                shown = attempt.flow();
                ui.mark_dirty(); // `shown` changed under the screen
                refresh = false;
                test_script_report_stages(attempt.log(), stages_reported);
            }
            ui.set_elapsed(attempt.elapsed());
            ui.set_cancelling(attempt.cancel_requested());
            const auto question = attempt.trust_question();
            if (question && !prompt_showing) {
                ui.show_trust_prompt(*question);
                prompt_showing = true;
            } else if (!question && prompt_showing) {
                ui.clear_trust_prompt();
                prompt_showing = false;
            }
            if (ui.dirty())
                window.blit(ui.render());

            // Wake for input, for the worker, or once a quarter second for the
            // elapsed-time clock.
            wait_for_events(display, wake.fd(), 250);
            wake.drain();
            refresh = true;
            while (XPending(display) > 0) {
                XEvent event {};
                XNextEvent(display, &event);
                handle(event);
            }
            if ((closed || quit_now()) && !attempt.cancel_requested())
                attempt.cancel();
            switch (ui.take_action()) {
            case ConnectScreen::Action::cancel:
                attempt.cancel();
                break;
            case ConnectScreen::Action::trust:
                attempt.answer_trust(TrustChoice::trust);
                break;
            case ConnectScreen::Action::replace:
                attempt.answer_trust(TrustChoice::replace);
                break;
            case ConnectScreen::Action::reject:
                attempt.answer_trust(TrustChoice::reject);
                break;
            default:
                break;
            }
        }

        out.result = attempt.take_result();
        shown = attempt.flow();
        test_script_report_stages(attempt.log(), stages_reported);
        // A cancel that raced a successful connect still wins: the user said stop.
        if (out.result.cancelled || attempt.cancel_requested()) {
            out.result = ConnectResult {};
            out.cancelled = true;
            out.action = ConnectScreen::Action::back;
        } else if (!out.result.ok) {
            if (test_script_take("connect-error") == std::optional<std::string>("back")) {
                std::cerr << "e2e: connect failed, leaving the error screen by Back\n";
                closed = true;
            }
            ui.clear_trust_prompt();
            ui.set_diagnostics(attempt.log().lines());
            ui.mark_dirty();
            while (!closed && !quit_now() && out.action == ConnectScreen::Action::none) {
                if (ui.dirty())
                    window.blit(ui.render());
                wait_for_events(display, -1, -1);
                while (!closed && XPending(display) > 0) {
                    XEvent event {};
                    XNextEvent(display, &event);
                    handle(event);
                }
                const ConnectScreen::Action action = ui.take_action();
                if (action == ConnectScreen::Action::copy_log) {
                    clipboard.set(attempt.log().text());
                    ui.note_copied();
                } else if (action == ConnectScreen::Action::back
                           || action == ConnectScreen::Action::retry) {
                    out.action = action;
                }
            }
            if (closed || out.action == ConnectScreen::Action::none)
                out.action = ConnectScreen::Action::back;
        }
    }
    XCloseDisplay(display);
    return out;
}

// --- the connected desktop -------------------------------------------------------

bool same_rect(const IntRect& a, const IntRect& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// The X11 front end's toolbar hotkey, Ctrl+Alt+Home: the same chord as the SDL
// front end (and Microsoft's RDP client's connection bar), so it is free of the
// remote's own shortcuts. X's Alt is Haiku's Command (modifier_mask()).
bool is_shell_hotkey(KeySym symbol, unsigned state)
{
    return (symbol == XK_Home || symbol == XK_KP_Home) && (state & ControlMask) != 0
        && (state & Mod1Mask) != 0;
}

// The window's _NET_WM_STATE says it is full screen. Read from the window
// manager's own record rather than remembered, because a window manager may
// refuse the request, or the user may leave full screen through it.
bool is_fullscreen(Display* display, Window window)
{
    const Atom state = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    Atom type = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char* data = nullptr;
    bool found = false;
    if (XGetWindowProperty(display, window, state, 0, 64, False, XA_ATOM, &type, &format,
                           &count, &after, &data)
            == Success
        && data != nullptr) {
        if (type == XA_ATOM && format == 32) {
            const auto* atoms = reinterpret_cast<const unsigned long*>(data);
            for (unsigned long i = 0; i < count; ++i)
                found = found || atoms[i] == fullscreen;
        }
        XFree(data);
    }
    return found;
}

// An EWMH window manager is running: the root names its check window.
bool window_manager_running(Display* display)
{
    const Atom check = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    Atom type = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char* data = nullptr;
    const bool running = XGetWindowProperty(display, DefaultRootWindow(display), check, 0, 1,
                                            False, XA_WINDOW, &type, &format, &count, &after,
                                            &data)
            == Success
        && data != nullptr && count == 1;
    if (data != nullptr)
        XFree(data);
    return running;
}

// What the controller asks of the X window. The desktop is shown the way the
// SDL front end shows it (viewport.hpp): scaled to fit the window, letterboxed,
// with SDL's scaling filter. X has no separate point and pixel sizes -- a window
// is measured in device pixels, as SDL on X11 measures it too (its
// SDL_WINDOW_ALLOW_HIGHDPI changes nothing there) -- so the drawable is the
// window, and Window and Fit ask the remote for exactly that many pixels, shown
// 1:1.
class X11DesktopWindow final : public DesktopWindow {
public:
    explicit X11DesktopWindow(XWindow& window) : window_(window) {}

    void set_desktop_size(DisplaySize) override
    {
        // The letterbox follows at the next present: the image is the window's
        // size, and the shadow takes the new desktop's from the session.
        full_redraw = true;
    }

    DisplaySize drawable_size() override
    {
        return {window_.window_width(), window_.window_height()};
    }

    void fit_window_to(DisplaySize size) override
    {
        Display* display = window_.display();
        if (is_fullscreen(display, window_.id()))
            return;
        const int screen = DefaultScreen(display);
        if (size.width > DisplayWidth(display, screen)
            || size.height > DisplayHeight(display, screen))
            return;
        XResizeWindow(display, window_.id(), static_cast<unsigned>(size.width),
                      static_cast<unsigned>(size.height));
        XFlush(display);
    }

    // EWMH _NET_WM_STATE_FULLSCREEN, asked of the window manager. Like SDL
    // (X11_SetWindowFullscreenViaWM, SDL_x11window.c:1412-1423) the request is
    // an explicit add or remove decided from the window's current state, not a
    // toggle, so it cannot invert a state the window manager changed on its
    // own. With no window manager there is nobody to ask, and nothing happens
    // -- also as with SDL -- but the log says why.
    void toggle_fullscreen() override
    {
        Display* display = window_.display();
        const bool leave = is_fullscreen(display, window_.id());
        if (!window_manager_running(display))
            std::cerr << "full screen: no EWMH window manager is running to ask\n";
        XEvent event {};
        event.xclient.type = ClientMessage;
        event.xclient.window = window_.id();
        event.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
        event.xclient.format = 32;
        event.xclient.data.l[0] = leave ? 0 : 1; // _NET_WM_STATE_REMOVE / _ADD
        event.xclient.data.l[1]
            = static_cast<long>(XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False));
        event.xclient.data.l[3] = 1; // source: a normal application
        XSendEvent(display, DefaultRootWindow(display), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &event);
        XFlush(display);
        std::cerr << "full screen " << (leave ? "off" : "on")
                  << " requested (_NET_WM_STATE_FULLSCREEN)\n";
    }

    void reconnected() override
    {
        // HAIKU_REMOTE_TEST_SCRIPT: a "session:" step queued behind
        // reconnect:resume watches the reconnected session.
        if (!test_wait)
            test_wait = test_script_pixel_wait();
        full_redraw = true;
    }

    std::optional<TestPixelWait> test_wait = test_script_pixel_wait();
    bool full_redraw = true;

private:
    XWindow& window_;
};

// Run a connected desktop over `link`, whose transport is already connected.
// Auto-reconnect follows `reconnect`; a manual Reconnect from the toolbar works
// either way.
DesktopEnd run_desktop(DesktopSize size, SessionLink& link, const std::string& name,
                       const ReconnectConfig& reconnect, bool stats)
{
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr)
        throw std::runtime_error("could not open X display");

    std::unique_ptr<Session> prestarted = link.take_session();
    const int screen = DefaultScreen(display);
    // The Session the connect's handshake ran on is continued at its own size;
    // otherwise default the remote desktop to the display, so the window does
    // not come up larger than the screen. --width/--height still override.
    if (prestarted != nullptr) {
        size.width = prestarted->surface().width();
        size.height = prestarted->surface().height();
    } else if (!size.explicit_size) {
        size.width = std::min(DisplayWidth(display, screen), Surface::max_dimension);
        size.height = std::min(DisplayHeight(display, screen), Surface::max_dimension);
    }

    std::optional<DesktopEnd> end;
    try {
        XWindow window(display, "Haiku Remote", size.width, size.height,
                       ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask
                           | ButtonReleaseMask | PointerMotionMask | FocusChangeMask
                           | StructureNotifyMask,
                       1, 1, true);
        X11DesktopWindow host(window);
        WakePipe wake;
        DesktopController desktop(link, name, size.width, size.height, reconnect, host,
                                  std::move(prestarted), [&wake] { wake.wake(); });
        Session& session = desktop.session();
        DesktopShell& shell = desktop.shell();
        InputMethod input(display, window.id());
        ClickCounter clicks;
        bool closed = false;

        const ::Cursor invisible_cursor = build_invisible_cursor(display, window.id());
        ::Cursor server_cursor = None;
        std::uint64_t applied_cursor_generation = 0;
        std::optional<bool> applied_cursor_shown;
        // The X pointer position and the server's cursor position track each
        // other -- we send RP_MOUSE_MOVED and the desktop answers
        // RP_MOVE_CURSOR_TO with the same point -- so the native cursor needs no
        // per-frame repositioning; only the shape and visibility change.
        const auto sync_cursor = [&]() {
            if (desktop.connecting())
                return;
            const auto& remote = session.cursor();
            if (remote.generation == 0)
                return; // No RP_SET_CURSOR yet: leave the host pointer alone.
            if (remote.generation != applied_cursor_generation) {
                applied_cursor_generation = remote.generation;
                if (server_cursor != None)
                    XFreeCursor(display, server_cursor);
                server_cursor = build_x_cursor(display, window.id(), remote);
                applied_cursor_shown.reset();
            }
            if (applied_cursor_shown.has_value() && *applied_cursor_shown == remote.visible)
                return;
            applied_cursor_shown = remote.visible;
            if (server_cursor == None)
                XUndefineCursor(display, window.id()); // a host arrow beats none
            else
                XDefineCursor(display, window.id(),
                              remote.visible ? server_cursor : invisible_cursor);
            XFlush(display);
        };

        PerformanceStats performance;
        std::optional<Clock::time_point> pending_input;
        auto dirty_since = Clock::now();
        auto last_present = dirty_since - present_interval;
        bool expose_requested = true;
        std::optional<IntRect> bar_shown_at;
        std::optional<IntRect> stats_shown_at;
        const Surface* bar_pixels = nullptr;
        const Surface* stats_pixels = nullptr;

        // The remote screen as last presented, BGRA at the desktop's size: what
        // the damage tracking compares the session against, and what is scaled
        // into the window. Reading it, rather than the session, is what lets a
        // window be repainted while a reconnect's worker owns the session.
        std::vector<std::uint8_t> shadow;
        int shadow_w = 0;
        int shadow_h = 0;
        const auto size_shadow = [&](int width, int height) {
            if (width == shadow_w && height == shadow_h)
                return false;
            shadow_w = width;
            shadow_h = height;
            shadow.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4,
                          0);
            return true;
        };
        size_shadow(size.width, size.height);
        // The scaling filter the SDL front end would use: SDL's default
        // (nearest) unless the environment sets SDL_RENDER_SCALE_QUALITY.
        const ScaleFilter filter = scale_filter_from_hint(std::getenv("SDL_RENDER_SCALE_QUALITY"));
        std::optional<Letterbox> shown_view;
        // What the window shows now, for mapping the pointer back.
        const auto current_view = [&]() {
            return letterbox(window.window_width(), window.window_height(), shadow_w, shadow_h);
        };
        // A non-native visual is scaled into this, then placed pixel by pixel.
        std::vector<std::uint8_t> staging;

        // Present: the session's changed pixels into the shadow, then whatever
        // part of the window they, the bar and the statistics panel reach,
        // scaled from the shadow with the bar and panel composed over it (each
        // from its own surface: neither is ever drawn into the session's
        // surface or the shadow).
        const auto present = [&]() {
            const Surface& surface = session.surface();
            bool full = expose_requested || host.full_redraw;
            if (desktop.surface_changed() || full) {
                if (size_shadow(surface.width(), surface.height()))
                    full = true;
            }
            const int out_w = window.window_width();
            const int out_h = window.window_height();
            if (window.ensure_image(out_w, out_h))
                full = true;
            const Letterbox view = letterbox(out_w, out_h, shadow_w, shadow_h);
            if (!shown_view || !(*shown_view == view))
                full = true;
            shown_view = view;

            const auto copy_start = Clock::now();
            Damage changed;
            if (desktop.surface_changed() || full)
                changed = copy_surface_rows(surface, shadow.data(), shadow_w * 4, shadow_w,
                                            shadow_h, full);
            const auto include = [&changed](const IntRect& r) {
                changed.include(r.left, r.top, r.right, r.bottom);
            };
            // An overlay that went away or moved gives its area back to the
            // remote screen; one that is new, moved or redrawn is composed in.
            const std::optional<IntRect> bar_now
                = shell.visible() ? std::optional<IntRect>(shell.bar_rect()) : std::nullopt;
            const std::optional<IntRect> stats_now = shell.stats_visible()
                ? std::optional<IntRect>(shell.stats_rect())
                : std::nullopt;
            if (bar_shown_at && (!bar_now || !same_rect(*bar_shown_at, *bar_now)))
                include(*bar_shown_at);
            if (stats_shown_at && (!stats_now || !same_rect(*stats_shown_at, *stats_now)))
                include(*stats_shown_at);
            if (bar_now && (shell.dirty() || !bar_shown_at || !same_rect(*bar_shown_at, *bar_now)
                            || bar_pixels == nullptr)) {
                bar_pixels = &shell.render_bar();
                include(*bar_now);
            }
            if (stats_now
                && (shell.dirty() || !stats_shown_at || !same_rect(*stats_shown_at, *stats_now)
                    || stats_pixels == nullptr)) {
                stats_pixels = &shell.render_stats();
                include(*stats_now);
            }
            bar_shown_at = bar_now;
            stats_shown_at = stats_now;

            IntRect area = full ? IntRect {0, 0, out_w - 1, out_h - 1}
                : changed.empty()
                ? IntRect {}
                : output_damage(view, filter,
                                {changed.left, changed.top, changed.right, changed.bottom});
            area = intersect(area, IntRect {0, 0, out_w - 1, out_h - 1});
            if (!area.empty()) {
                // The SDL front end's draw order: the desktop, the statistics
                // panel, then the bar on top.
                std::vector<ScaleLayer> layers;
                layers.push_back({shadow.data(), shadow_w * 4, {0, 0, shadow_w - 1, shadow_h - 1}});
                if (stats_now && stats_pixels != nullptr)
                    layers.push_back({stats_pixels->pixels().data(), stats_pixels->stride(),
                                      *stats_now});
                if (bar_now && bar_pixels != nullptr)
                    layers.push_back({bar_pixels->pixels().data(), bar_pixels->stride(), *bar_now});
                XImage& image = window.image();
                if (native_bgra(image)) {
                    scale_layers(layers, view, filter,
                                 reinterpret_cast<std::uint8_t*>(image.data),
                                 image.bytes_per_line, area);
                } else {
                    staging.resize(static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h) * 4);
                    scale_layers(layers, view, filter, staging.data(), out_w * 4, area);
                    store_pixels(image, staging.data(), out_w * 4, area);
                }
            }
            const auto x11_start = Clock::now();
            if (!area.empty())
                window.put(area.left, area.top, area.right - area.left + 1,
                           area.bottom - area.top + 1);
            XFlush(display);
            if (stats) {
                performance.copy.add(x11_start - copy_start);
                if (!area.empty()) {
                    ++performance.frames;
                    performance.damaged_pixels += static_cast<std::uint64_t>(area.right - area.left + 1)
                        * static_cast<std::uint64_t>(area.bottom - area.top + 1);
                    performance.full_frame_pixels
                        = static_cast<std::uint64_t>(out_w) * static_cast<std::uint64_t>(out_h);
                    performance.x11.add(Clock::now() - x11_start);
                    if (pending_input) {
                        performance.input_present.add(Clock::now() - *pending_input);
                        pending_input.reset();
                    }
                }
            }
            desktop.presented();
            expose_requested = false;
            host.full_redraw = false;
            last_present = Clock::now();
            if (host.test_wait && !desktop.connecting()
                && test_script_check_frame(*host.test_wait, session))
                host.test_wait.reset();
        };

        const auto send_input = [&](const std::vector<std::uint8_t>& message) {
            if (stats && !pending_input)
                pending_input = Clock::now();
            desktop.send(message);
        };

        desktop.start();
        while (desktop.running()) {
            auto now = Clock::now();
            if (quit_now() || closed) {
                desktop.user_quit();
                break;
            }
            if (desktop.advance(now))
                continue;

            if (desktop.live()) {
                const bool was_dirty = desktop.surface_changed();
                if (desktop.receive(2))
                    continue;
                if (!was_dirty && desktop.surface_changed())
                    dirty_since = Clock::now();
            } else {
                // No transport: block on the window, the worker's wake pipe and
                // the quit signal until something happens or the reconnect
                // deadline -- no busy loop, and no sleep the user cannot
                // interrupt.
                const int timeout = desktop.idle_wait_ms(now);
                if (desktop.needs_present())
                    present();
                wait_for_events(display, wake.fd(), timeout);
                wake.drain();
            }

            now = Clock::now();
            while (XPending(display) > 0) {
                XEvent event {};
                XNextEvent(display, &event);
                if ((event.type == KeyPress || event.type == KeyRelease)
                    && XFilterEvent(&event, None) == True)
                    continue; // the input method consumed it
                switch (event.type) {
                case Expose:
                    desktop.mark_dirty();
                    expose_requested = true;
                    break;
                case ConfigureNotify:
                    if (window.configured(event))
                        desktop.window_resized(now);
                    break;
                case ClientMessage:
                    if (window.is_close(event))
                        closed = true;
                    break;
                case MotionNotify: {
                    // Keep ordering relative to buttons and keys, but collapse a
                    // contiguous run of stale pointer positions.
                    while (XPending(display) > 0) {
                        XEvent next {};
                        XPeekEvent(display, &next);
                        if (next.type != MotionNotify)
                            break;
                        XNextEvent(display, &event);
                    }
                    // Window pixels to desktop pixels, as SDL maps them through
                    // the logical size; the toolbar lives in desktop pixels too.
                    const IntPoint at
                        = output_to_logical(current_view(), event.xmotion.x, event.xmotion.y);
                    if (!shell.pointer_move(at.x, at.y, now))
                        send_input(InputEncoder::mouse_moved(static_cast<float>(at.x),
                                                             static_cast<float>(at.y)));
                    break;
                }
                case ButtonPress: {
                    const unsigned button = event.xbutton.button;
                    if (button >= Button4 && button <= 7) {
                        float dx = 0;
                        float dy = 0;
                        if (button == Button4) dy = -1;
                        if (button == Button5) dy = 1;
                        if (button == 6) dx = -1;
                        if (button == 7) dx = 1;
                        send_input(InputEncoder::mouse_wheel(dx, dy));
                        break;
                    }
                    const IntPoint at
                        = output_to_logical(current_view(), event.xbutton.x, event.xbutton.y);
                    if (button == Button1 && shell.pointer_press(at.x, at.y, now))
                        break;
                    const int count = clicks.press(event.xbutton);
                    unsigned state = event.xbutton.state;
                    if (button == Button1) state |= Button1Mask;
                    if (button == Button2) state |= Button2Mask;
                    if (button == Button3) state |= Button3Mask;
                    send_input(InputEncoder::mouse_down(static_cast<float>(at.x),
                                                        static_cast<float>(at.y),
                                                        button_mask(state), count));
                    break;
                }
                case ButtonRelease: {
                    const unsigned button = event.xbutton.button;
                    if (button >= Button4 && button <= 7)
                        break;
                    if (button == Button1 && shell.pointer_release())
                        break;
                    unsigned state = event.xbutton.state;
                    if (button == Button1) state &= ~Button1Mask;
                    if (button == Button2) state &= ~Button2Mask;
                    if (button == Button3) state &= ~Button3Mask;
                    const IntPoint at
                        = output_to_logical(current_view(), event.xbutton.x, event.xbutton.y);
                    send_input(InputEncoder::mouse_up(static_cast<float>(at.x),
                                                      static_cast<float>(at.y),
                                                      button_mask(state)));
                    break;
                }
                case KeyPress:
                case KeyRelease: {
                    const InputMethod::Key typed = input.lookup(event.xkey);
                    if (is_shell_hotkey(typed.symbol, event.xkey.state)) {
                        if (event.type == KeyPress)
                            shell.toggle();
                        break;
                    }
                    std::string text = typed.text;
                    const auto special = haiku_special_text(typed.symbol);
                    if (!special.empty())
                        text.assign(special);
                    desktop.set_modifiers(modifier_mask(event.xkey.state));
                    send_input(InputEncoder::key(event.type == KeyPress, text,
                                                 first_utf8_scalar(text),
                                                 haiku_key(typed.symbol)));
                    break;
                }
                case FocusOut:
                    desktop.focus_lost();
                    break;
                case MappingNotify:
                    XRefreshKeyboardMapping(&event.xmapping);
                    break;
                default:
                    break;
                }
            }

            sync_cursor();
            desktop.after_events(Clock::now());

            // Coalesce a burst of drawing into one present: wait for the batch
            // to go quiet, but never longer than a frame's latency budget. A
            // toolbar change, an expose or a session that is not drawing is
            // presented at once.
            now = Clock::now();
            if (desktop.needs_present()) {
                const bool drawing = desktop.surface_changed() && !expose_requested
                    && !host.full_redraw;
                const bool frame_due = now - last_present >= present_interval;
                const bool batch_settled = !desktop.live() || now - dirty_since >= batch_idle_time;
                const bool latency_reached = now - dirty_since >= maximum_frame_latency;
                if (!drawing || (frame_due && (batch_settled || latency_reached)))
                    present();
            }

            if (stats && now - performance.interval_start >= std::chrono::seconds(1)) {
                const auto details = performance.report(
                    session.message_count(), session.draw_string_replies(), now);
                const auto title = "Haiku Remote | " + details;
                XStoreName(display, window.id(), title.c_str());
                XFlush(display);
                std::cerr << "[perf] " << details << '\n';
            }
        }
        end = desktop.finish();
        if (server_cursor != None)
            XFreeCursor(display, server_cursor);
        XFreeCursor(display, invisible_cursor);
    } catch (...) {
        link.teardown();
        XCloseDisplay(display);
        throw;
    }
    XCloseDisplay(display);
    return *end;
}

// The X11 windows of the shared app flow (desktop_session.hpp).
class X11FrontEnd final : public FrontEnd {
public:
    explicit X11FrontEnd(bool stats) : stats_(stats) {}

    std::optional<ConnectionProfile> run_library(ConnectionLibrary& library) override
    {
        return ::run_library(library, clipboard_);
    }
    ConnectOutcome run_connect(const ConnectionProfile& profile) override
    {
        return ::run_connect(profile, clipboard_);
    }
    DesktopEnd run_desktop(DesktopSize size, SessionLink& link, const std::string& name,
                           const ReconnectConfig& reconnect) override
    {
        return ::run_desktop(size, link, name, reconnect, stats_);
    }
    bool quit_requested() const override { return quit_signalled.load(); }

private:
    bool stats_;
    X11Clipboard clipboard_;
};

int main(int argc, char** argv)
{
    try {
        // The clipboard serves its selection from a thread of its own, on a
        // connection of its own; Xlib must be told before any other call.
        XInitThreads();
        Options options = parse_options(argc, argv);
        install_quit_signals();
        set_test_script_close_window([] {
            if (XWindow* window = XWindow::current())
                window->request_close();
        });
        X11FrontEnd front_end(options.stats);
        const DesktopSize size {options.width, options.height, options.size_explicit};
        // A connection named on the command line connects straight away: the
        // automation/debugging path is unchanged.
        if (options.target == LaunchTarget::endpoint)
            return run_endpoint(front_end, options.transport, size);
        // Otherwise the library, or the one profile --profile / --ssh-host
        // names; a pick runs its progress window and desktop, then the library
        // reopens until it is closed.
        return run_profile_flow(front_end, options.launch, options.target, size);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return exit_status::usage;
    }
}
