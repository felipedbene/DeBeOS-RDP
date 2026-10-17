#include "haiku_remote/session.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>

namespace haiku_remote {
namespace {

constexpr std::uint32_t b_gray1 = 0x0001;
constexpr std::uint32_t b_gray8 = 0x0002;
constexpr std::uint32_t b_rgb24 = 0x0003;
constexpr std::uint32_t b_cmap8 = 0x0004;
constexpr std::uint32_t b_rgb32 = 0x0008;
constexpr std::uint32_t b_rgba32 = 0x2008;
constexpr std::size_t max_decoded_bitmap_size = 256 * 1024 * 1024;
// Floor for the readback limit; the effective limit is derived from the surface
// in read_bitmap_reply(), because a fixed cap is a cap on the display size.
constexpr std::uint64_t min_readback_size = 64 * 1024 * 1024;
// One BPoint on the wire: two 32-bit floats.
constexpr std::size_t point_wire_size = 2 * sizeof(float);

// A UTF-8 continuation byte, i.e. a byte the server's UTF8CountChars() does not
// count as the start of a glyph and therefore sends no offset point for.
bool is_utf8_continuation(char byte)
{
    return (static_cast<unsigned char>(byte) & 0xc0) == 0x80;
}

} // namespace

Session::Session(int width, int height, Send send, Log log)
    : requested_width_(width)
    , requested_height_(height)
    , send_(std::move(send))
    , log_(std::move(log))
    , surface_(width, height)
{
    // Face selection reports a substituted or missing font style through this.
    // It has to reach the same place every other session diagnostic does: with
    // RP_CAP_STRING_WIDTH_REPLY advertised, a width measured with the wrong
    // style is this client silently laying the server's text out wrong.
    text_.set_log(log_);

    set_compression_offered(
        compression_enabled_by_environment(std::getenv("HAIKU_REMOTE_COMPRESSION")));
}

bool compression_enabled_by_environment(const char* value)
{
    if (value == nullptr)
        return true;
    std::string text(value);
    for (auto& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return !(text == "0" || text == "false" || text == "off" || text == "no");
}

void Session::start()
{
    send_message(Writer(Op::init_connection).finish());

    // URP/1 capability handshake, sent before any drawing. Announce our
    // protocol version and the features we implement so the server only
    // drives us with capabilities we actually have. This client shapes text
    // itself and answers RP_STRING_WIDTH, so it advertises
    // RP_CAP_STRING_WIDTH_REPLY. It also understands the reconnect conversation
    // -- the session identity in RP_HELLO_ACK and the RP_RESYNC barrier -- so it
    // advertises RP_CAP_RESYNC; the server's state replay on reconnect happens
    // regardless, but this bit is what lets us tell a reconnected session from a
    // new one and ask for a replay ourselves. A server that predates the
    // handshake simply ignores this message.
    Writer hello(Op::hello);
    hello.u32(protocol_version);
    //
    // RP_CAP_COMPRESS_ZSTD only when this build can decode it: the server
    // switches the stream to compressed segments from the byte after its
    // RP_HELLO_ACK (RemoteWireFormat.h:14-19), so an advertised bit without a
    // decoder aborts the session at the framer (DeBeOS-RDP #23). A browser
    // frame is a raw full-view bitmap -- 2.7 MB for a 1024x663 Ladybird view on
    // every scroll notch or keystroke -- which the server's encoder takes down
    // 40-310x; on a constrained link that is the whole latency budget.
    //
    // RP_CAP_SUBPIXEL_PRECISE always: the Painter port honours the flag
    // (painter.cpp, checked against app_server's own Painter with the flag
    // set and clear), and so does DrawString's baseline rounding below.
    std::uint32_t capabilities = cap_string_width_reply | cap_resync
        | cap_subpixel_precise;
    if (offer_compression_)
        capabilities |= cap_compress_zstd;
    awaiting_hello_ack_ = offer_compression_;
    compressed_ = false;
    hello.u32(capabilities);
    hello.u32(0); // max decode width (no Tier P)
    hello.u32(0); // max decode height
    hello.u32(static_cast<std::uint32_t>(requested_width_));
    hello.u32(static_cast<std::uint32_t>(requested_height_));
    send_message(hello.finish());
}

bool Session::send_client_message(std::span<const std::uint8_t> bytes)
{
    return send_(bytes);
}

bool Session::request_full_repaint()
{
    Writer display(Op::update_display_mode);
    display.i32(requested_width_);
    display.i32(requested_height_);
    return send_message(display.finish());
}

bool Session::set_display_size(int width, int height)
{
    if (!valid_display_size(width, height))
        return false;
    surface_.resize(width, height);
    requested_width_ = width;
    requested_height_ = height;
    // Deliberately nothing else: see the header. The server keeps its shadow of
    // every token's state across the mode change and will not re-send it.
    return true;
}

Session::DisplayModeChange Session::change_display_mode(int width, int height)
{
    if (!valid_display_size(width, height))
        return DisplayModeChange::invalid;
    if (width == requested_width_ && height == requested_height_)
        return DisplayModeChange::unchanged;
    set_display_size(width, height);
    // The canvas is resized before the request goes out, not when the repaint
    // arrives: there is no acknowledgement to wait for. app_server answers
    // RP_UPDATE_DISPLAY_MODE with nothing but drawing (RemoteHWInterface.cpp:
    // 983-998). Anything still in flight for the old frame either lands inside
    // the new one or is clipped by it, and the full-frame repaint that follows
    // (Desktop.cpp:3644-3663) overwrites it either way.
    return request_full_repaint() ? DisplayModeChange::sent
                                  : DisplayModeChange::send_failed;
}

void Session::discard_drawing_state()
{
    // Every per-token drawing state, and with it each token's pattern, high and
    // low colour, pen size, font, transform, offsets and clip. Reusing any of
    // these after a reconnect is the client half of the reconnect black screen:
    // the server re-states everything on the new connection, and a stale local
    // value would win the "unchanged, skip" comparison and never be overwritten.
    states_.clear();
    // The colour-map palette is server-owned and immutable for the life of the
    // session, and the RP_RESYNC replay carries every per-engine state and the
    // cursor but never a palette (RemoteHWInterface.cpp:795-814,
    // RemoteDrawingEngine::ReplayState). Clearing it here dropped it for the
    // rest of the connection and blacked out every B_CMAP8 bitmap in the
    // repaint that followed -- issue #39. The palette is only stale across a
    // fresh connection to a *different* server session, and reset() clears it
    // then.
    // The cursor is re-sent by the server's replay; drop the old shape and
    // position so nothing from the previous session is composited in the gap.
    cursor_ = CursorState {};
}

void Session::reset()
{
    discard_drawing_state();
    // A new connection may reach a different server session with its own
    // colour map, so the palette does not carry across the way the RP_RESYNC
    // barrier lets it carry across a same-session replay. The new connection's
    // Op::init_connection handler will send RP_GET_SYSTEM_PALETTE and repopulate.
    palette_.clear();
    // A fresh byte stream: any half-read frame from the dropped connection must
    // not be prepended to the new one, and a latched framing failure must not
    // outlive the connection that caused it.
    framer_ = Framer {};
    segments_.reset();
    awaiting_hello_ack_ = false;
    compressed_ = false;
    message_count_ = 0;
    negotiated_version_ = 0;
    negotiated_capabilities_ = 0;
    server_closed_ = false;
    unhandled_.clear();
    // session_id_, generation_ and generation_changed_ deliberately survive: the
    // next RP_HELLO_ACK compares against the generation we last saw to recognise
    // this as the same server session at a new generation.
}

bool Session::request_resync()
{
    if ((negotiated_capabilities_ & cap_resync) == 0)
        return false;
    Writer resync(Op::resync);
    resync.u32(generation_);
    return send_message(resync.finish());
}

void Session::observe_generation(std::uint32_t session_id, std::uint32_t generation)
{
    // A higher generation under the same session id is a reconnect to the same
    // server session; anything cached from before it is stale. A different
    // session id is a different session entirely (nothing carried over anyway).
    if (session_id_ != 0 && session_id == session_id_
        && generation > generation_) {
        generation_changed_ = true;
    }
    session_id_ = session_id;
    generation_ = generation;
}

void Session::ingest(std::span<const std::uint8_t> bytes)
{
    // Decoded bytes reach the Framer at most SegmentDecoder::max_chunk at a
    // time, so what is pending stays about one message however far the
    // ratio inflates a read.
    const auto to_framer = [this](std::span<const std::uint8_t> plain) {
        ingest_plain(plain);
    };
    if (compressed_) {
        segments_.feed(bytes, to_framer);
        return;
    }
    if (!awaiting_hello_ack_) {
        ingest_plain(bytes);
        return;
    }

    // Compression was offered and not yet answered. The bytes after
    // RP_HELLO_ACK may already be segments, so frame no further than the ack;
    // what follows it is routed by what the ack negotiated. A server that
    // predates the handshake never acks, and this then frames exactly as the
    // plain path does.
    const auto messages = framer_.feed(bytes, Op::hello_ack, true);
    const bool acked = !messages.empty() && messages.back().op == Op::hello_ack;
    for (const auto& message : messages)
        handle(message);
    if (framer_.segment_start_seen() && !acked) {
        // A server without Haiku-Graviton #625 switched to segments before
        // its acknowledgement reached us (it was queued behind a full send
        // ring, then compressed on the way out). The ack, and whatever plain
        // messages were queued with it, are inside the compressed stream, in
        // order; follow the server's switch and read them there.
        awaiting_hello_ack_ = false;
        compressed_ = true;
        if (log_)
            log_("compressed stream began before RP_HELLO_ACK (server without "
                 "the queued-ack fix); following it");
        segments_.feed(framer_.take_pending(), to_framer);
        return;
    }
    if (!acked)
        return;

    awaiting_hello_ack_ = false;
    compressed_ = (negotiated_capabilities_ & cap_compress_zstd) != 0;
    const auto rest = framer_.take_pending();
    if (compressed_)
        segments_.feed(rest, to_framer);
    else
        ingest_plain(rest);
}

void Session::ingest_plain(std::span<const std::uint8_t> bytes)
{
    for (const auto& message : framer_.feed(bytes))
        handle(message);
}

bool Session::send_message(std::vector<std::uint8_t> bytes)
{
    // Count the replies app_server is synchronously blocked on, at the one point
    // every reply passes through -- including the answer_after_failure() paths,
    // which a per-handler counter would miss and which are exactly the cases
    // where the server stalls longest. The op is the first two bytes of the
    // frame header (op:u16, total_length:u32, LE).
    if (bytes.size() >= 2) {
        const auto op = static_cast<Op>(
            bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
        switch (op) {
        case Op::draw_string_result:
            ++draw_string_replies_;
            ++sync_replies_;
            break;
        case Op::string_width_result:
            ++string_width_replies_;
            ++sync_replies_;
            break;
        case Op::read_bitmap_result:
            ++sync_replies_;
            break;
        default:
            break;
        }
    }
    return send_(bytes);
}

DrawState& Session::state(std::int32_t token)
{
    return states_[token];
}

void Session::handle(const Message& message)
{
    ++message_count_;
    try {
        Reader reader(message.payload);
        if (is_session_level(message.op))
            handle_session(message.op, reader);
        else
            handle_token(message.op, reader.i32(), reader);
    } catch (const std::exception& error) {
        if (log_) {
            std::ostringstream text;
            text << "decode error in " << op_name(message.op) << ": " << error.what();
            log_(text.str());
        }
        // Swallowing the error is right for framing -- one bad message must not
        // kill the session -- but the synchronous opcodes build their reply
        // inside the handler, so a throw halfway through means the reply is
        // never sent and the server's drawing thread blocks until its timeout
        // expires (1 s per string, 10 s per readback, the second holding the
        // desktop drawing engine's exclusive lock). Answer anyway, degraded: a
        // wrong pen position costs one mispainted string, a missing reply costs
        // a visibly frozen desktop.
        answer_after_failure(message);
    }
}

// The reply the protocol owes the server when the request could not be decoded.
// Only the opcodes the server blocks on need one; everything else is fire and
// forget and gets nothing.
void Session::answer_after_failure(const Message& message)
{
    std::int32_t token = 0;
    Point pen {};
    try {
        Reader reader(message.payload);
        token = reader.i32();
        if (message.op == Op::draw_string)
            pen = reader.point();
        else if (message.op == Op::draw_string_with_offsets) {
            // token, string, then one point per glyph: the first point is the
            // closest thing to a real pen position we can still recover.
            (void)reader.string();
            pen = reader.point();
        }
    } catch (const std::exception&) {
        // Keep whatever was recovered before the payload ran out.
    }

    switch (message.op) {
    case Op::draw_string:
    case Op::draw_string_with_offsets: {
        Writer reply(Op::draw_string_result);
        reply.i32(token);
        reply.point(pen);
        send_message(reply.finish());
        break;
    }
    case Op::string_width: {
        Writer reply(Op::string_width_result);
        reply.i32(token);
        reply.f32(0);
        send_message(reply.finish());
        break;
    }
    case Op::read_bitmap:
        send_message(read_bitmap_reply(token, {}));
        break;
    default:
        break;
    }
}

// The cursor as app_server's own DrawingEngine::ReadBitmap puts it into a
// screenshot (DrawingEngine.cpp:1392-1424) -- the path RemoteDrawingEngine
// delegates to the client by forwarding drawCursor (RemoteDrawingEngine.cpp:
// 1190). Every rule below is that function's, including where it looks wrong:
//
//  * It composites whenever a cursor *exists* (`if (!cursor) return`,
//    :1395-1396). There is no IsCursorVisible() test, so a hidden cursor still
//    lands in the picture; so does it here.
//  * The offset is cursorPosition - (bounds.LeftTop() + hotspot), computed as
//    BPoint floats (:1400-1401) and then truncated toward zero by ConvertBits'
//    `(int32)srcOffset.x` (ColorConversion.cpp:744-747) -- not floored, as the
//    on-screen frame is (HWInterface.cpp:576-577).
//  * The blend is `((dst * (255 - a)) >> 8) + src` per colour byte, stored to a
//    uint8 (:1415-1418). That assumes a premultiplied cursor, divides by 256
//    rather than 255 -- so even a fully transparent cursor pixel darkens what
//    is under it, 255 -> 254 -- and wraps modulo 256 when an unassociated
//    cursor's colour exceeds its alpha. All three are reproduced exactly.
//  * Clipping is to the destination bitmap: both ImportBits() calls clip their
//    offsets and widths to the buffers (ColorConversion.cpp:748-781).
//
// The one thing that cannot be matched: the local path blends fCursor
// (HWInterface::Cursor(), HWInterface.cpp:144-151), the plain cursor, while the
// wire only ever carries CursorAndDragBitmap() (RemoteHWInterface.cpp:772,
// :1472). Outside a drag the two are the same object (HWInterface.cpp:1049);
// during a drag this client composites the cursor-plus-drag-image it was sent.
//
// `bits` is the B_RGB24 reply (three bytes per pixel, B,G,R) whose top left is
// the raster point (left, top).
static void composite_readback_cursor(const CursorState& cursor,
                                      std::span<std::uint8_t> bits, int width,
                                      int height, std::size_t bytes_per_row,
                                      int left, int top)
{
    const Bitmap& shape = cursor.bitmap;
    if (shape.width <= 0 || shape.height <= 0
        || shape.bgra.size()
            < static_cast<std::size_t>(shape.width) * shape.height * 4) {
        return;
    }
    // BPoint arithmetic is float; keep it float so the truncation sees the
    // same value the server's would.
    const float offset_x = cursor.position.x
        - (static_cast<float>(left) + cursor.hotspot.x);
    const float offset_y = cursor.position.y
        - (static_cast<float>(top) + cursor.hotspot.y);
    if (!std::isfinite(offset_x) || !std::isfinite(offset_y)
        || std::fabs(offset_x) > 1.0e9f || std::fabs(offset_y) > 1.0e9f) {
        return;
    }
    const auto origin_x = static_cast<std::int64_t>(std::trunc(offset_x));
    const auto origin_y = static_cast<std::int64_t>(std::trunc(offset_y));
    for (int cy = 0; cy < shape.height; ++cy) {
        const std::int64_t y = origin_y + cy;
        if (y < 0 || y >= height)
            continue;
        for (int cx = 0; cx < shape.width; ++cx) {
            const std::int64_t x = origin_x + cx;
            if (x < 0 || x >= width)
                continue;
            const auto source = (static_cast<std::size_t>(cy) * shape.width
                                 + static_cast<std::size_t>(cx)) * 4;
            const unsigned keep = 255u - shape.bgra[source + 3];
            auto* destination = bits.data()
                + static_cast<std::size_t>(y) * bytes_per_row
                + static_cast<std::size_t>(x) * 3;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                destination[channel] = static_cast<std::uint8_t>(
                    ((destination[channel] * keep) >> 8)
                    + shape.bgra[source + channel]);
            }
        }
    }
}

// RP_READ_BITMAP_RESULT for the requested raster rectangle. Pixels outside the
// surface read back black rather than shrinking the answer, because the server
// imports the reply into a bitmap it already sized from its own request: a
// smaller bitmap than asked for is imported as a garbled image.
//
// A request that cannot be honoured at all -- an empty rectangle, or one whose
// readback would exceed the safety limit -- still gets a well-formed one-pixel
// reply. The server can parse that, so its wait ends immediately and the caller
// sees a failed readback instead of a frozen desktop.
//
// With drawCursor set the cursor is blended into the *reply*, never into the
// canvas -- the canvas is the only copy of the screen, and nothing would ever
// repaint the pixels under the pointer. See composite_readback_cursor().
std::vector<std::uint8_t> Session::read_bitmap_reply(std::int32_t token,
                                                     IntRect requested,
                                                     bool draw_cursor)
{
    constexpr std::uint64_t dimension_limit = 1 << 20;
    std::uint64_t width = requested.empty() ? 0
        : static_cast<std::uint64_t>(static_cast<std::int64_t>(requested.right)
                                     - requested.left + 1);
    std::uint64_t height = requested.empty() ? 0
        : static_cast<std::uint64_t>(static_cast<std::int64_t>(requested.bottom)
                                     - requested.top + 1);
    std::uint64_t bytes_per_row = (width * 3 + 3) & ~std::uint64_t {3};
    // The limit has to scale with the surface. A fixed cap is a cap on the
    // display size: a full-screen B_RGB24 readback of anything past roughly
    // 4763x4763 exceeds 64 MiB, and refusing it degrades every screenshot of a
    // large display to a one-pixel reply. Bound by what a readback of our own
    // framebuffer costs, with 64 MiB as a floor for small surfaces.
    const std::uint64_t surface_readback_size =
        ((static_cast<std::uint64_t>(surface_.width()) * 3 + 3) & ~std::uint64_t {3})
        * static_cast<std::uint64_t>(surface_.height());
    const std::uint64_t readback_limit =
        std::max<std::uint64_t>(min_readback_size, surface_readback_size);
    if (width == 0 || height == 0 || width > dimension_limit
        || height > dimension_limit
        || bytes_per_row * height > readback_limit) {
        if (log_) {
            std::ostringstream text;
            text << "bitmap readback of " << width << 'x' << height
                 << " cannot be answered; replying with one pixel";
            log_(text.str());
        }
        requested = {0, 0, 0, 0};
        width = 1;
        height = 1;
        bytes_per_row = 4;
        // A refused readback is a placeholder, not a picture of the screen.
        draw_cursor = false;
    }

    std::vector<std::uint8_t> bits(
        static_cast<std::size_t>(bytes_per_row * height));
    const auto visible = intersect(
        requested, {0, 0, surface_.width() - 1, surface_.height() - 1});
    for (int y = visible.top; y <= visible.bottom; ++y) {
        for (int x = visible.left; x <= visible.right; ++x) {
            const auto color = surface_.pixel(x, y);
            const auto destination = static_cast<std::size_t>(
                static_cast<std::uint64_t>(y - requested.top) * bytes_per_row
                + static_cast<std::uint64_t>(x - requested.left) * 3);
            bits[destination] = color.b;
            bits[destination + 1] = color.g;
            bits[destination + 2] = color.r;
        }
    }

    if (draw_cursor) {
        composite_readback_cursor(cursor_, bits, static_cast<int>(width),
                                  static_cast<int>(height),
                                  static_cast<std::size_t>(bytes_per_row),
                                  requested.left, requested.top);
    }

    Writer reply(Op::read_bitmap_result);
    reply.i32(token);
    reply.i32(static_cast<std::int32_t>(width));
    reply.i32(static_cast<std::int32_t>(height));
    reply.i32(static_cast<std::int32_t>(bytes_per_row));
    reply.u32(b_rgb24);
    reply.u32(0);
    reply.u32(static_cast<std::uint32_t>(bits.size()));
    reply.raw(bits);
    return reply.finish();
}

void Session::handle_session(Op op, Reader& reader)
{
    switch (op) {
    case Op::init_connection: {
        Writer palette(Op::get_system_palette);
        send_message(palette.finish());
        Writer display(Op::update_display_mode);
        display.i32(requested_width_);
        display.i32(requested_height_);
        send_message(display.finish());
        break;
    }
    case Op::hello_ack: {
        // Negotiated protocol version and capability intersection from the
        // server. Nothing is gated on them yet, but they are recorded so a
        // later milestone (or --stats style diagnostics) can inspect them.
        negotiated_version_ = reader.u32();
        negotiated_capabilities_ = reader.u32();
        // When RP_CAP_RESYNC is negotiated the ack carries the session identity
        // (session id, then connection generation). Guarded by remaining() as
        // well as the bit: a server that negotiated it but sent a short ack must
        // not throw here and lose the whole session. See RemoteHWInterface.cpp
        // (RP_HELLO_ACK appends these two only for a resync-capable client).
        if ((negotiated_capabilities_ & cap_resync) != 0
            && reader.remaining() >= 8) {
            const auto session_id = reader.u32();
            const auto generation = reader.u32();
            observe_generation(session_id, generation);
        }
        if (log_) {
            std::ostringstream text;
            text << "hello ack: version " << negotiated_version_
                 << ", capabilities 0x" << std::hex << negotiated_capabilities_;
            text << ((negotiated_capabilities_ & cap_compress_zstd) != 0
                         ? " (zstd stream compression)"
                         : offer_compression_ ? " (zstd offered, not accepted)"
                                              : "");
            if ((negotiated_capabilities_ & cap_resync) != 0) {
                text << std::dec << ", session " << session_id_
                     << " generation " << generation_;
                if (generation_changed_)
                    text << " (reconnected)";
            }
            log_(text.str());
        }
        break;
    }
    case Op::resync: {
        // The server -> client barrier: everything after this belongs to a new
        // connection generation, and a full state replay follows it. Discard
        // what we cached so the replay lands on a clean slate rather than being
        // merged with the previous session's state. It is a barrier, not a
        // request -- the replay is already on its way -- so there is nothing to
        // answer. See RemoteHWInterface::_SendResyncBarrier().
        const auto generation = reader.u32();
        observe_generation(session_id_, generation);
        discard_drawing_state();
        if (log_) {
            std::ostringstream text;
            text << "resync barrier: generation " << std::dec << generation
                 << "; discarded cached drawing state, replay follows";
            log_(text.str());
        }
        break;
    }
    case Op::get_system_palette_result: {
        const auto count = std::min<std::uint32_t>(reader.u32(), 4096);
        palette_.clear();
        palette_.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
            palette_.push_back(reader.color());
        break;
    }
    case Op::create_state:
        states_[reader.i32()] = DrawState {};
        break;
    case Op::delete_state:
        states_.erase(reader.i32());
        break;
    case Op::copy_rect_no_clipping: {
        const int dx = reader.i32();
        const int dy = reader.i32();
        surface_.copy_rect(reader.rect(), dx, dy);
        break;
    }
    case Op::fill_region_color_no_clipping: {
        const auto region = reader.region();
        const auto color = reader.color();
        for (const auto& rect : region)
            surface_.fill_rect_color(rect, color);
        break;
    }
    case Op::set_cursor: {
        // RemoteMessage.cpp:190-194, AddCursor(): Add(hotspot), then
        // AddBitmap(cursor). AddBitmap's non-minimal field order --
        // RemoteMessage.cpp:124-145: width, height, bytesPerRow, colorSpace,
        // flags, bitsLength, bits -- is exactly what read_bitmap() consumes, so
        // a cursor needs no second bitmap decoder.
        const Point hotspot = reader.point();
        // A cursor is composited from its own alpha channel, never through the
        // B_RGB32 "transparent magic" substitution: app_server's cursor blend
        // (HWInterface.cpp:600-606) reads byte 3 as alpha and does no such
        // rewrite. DrawingMode::copy is the mode read_bitmap() treats as
        // "reserved value is an ordinary colour".
        static const DrawState cursor_draw = [] {
            DrawState draw;
            draw.drawing_mode = DrawingMode::copy;
            return draw;
        }();
        // read_bitmap() throws ProtocolError on every degenerate shape (zero or
        // absurd dimensions, a row shorter than its pixels, a bitsLength that
        // disagrees with height * bytesPerRow), and handle() turns that into one
        // logged line. Decoding into a local and committing afterwards is what
        // keeps a bad cursor from also destroying the good one.
        Bitmap bitmap = read_bitmap(reader, cursor_draw);
        cursor_.hotspot = hotspot;
        cursor_.bitmap = std::move(bitmap);
        ++cursor_.generation;
        break;
    }
    case Op::set_cursor_visible:
        // RemoteHWInterface.cpp:872-878 sends Add(bool), and RemoteMessage.h's
        // Add() is a sizeof(T) memcpy, so visibility is a single byte.
        cursor_.visible = reader.boolean();
        break;
    case Op::move_cursor_to:
        // RemoteHWInterface.cpp:881-889 adds x and y as two separate floats,
        // which is byte-for-byte a BPoint.
        cursor_.position = reader.point();
        break;
    case Op::invalidate_rect:
    case Op::invalidate_region:
        // Nothing to do: this client repaints from the ops themselves, and the
        // server follows an invalidate with the drawing for it. The `break` is
        // load-bearing -- without it these fall into the close arm below, and
        // since the server sends an invalidate within the first frame of every
        // session, the capture ends almost immediately.
        break;
    // An orderly teardown, and the only in-band warning we get that the byte
    // stream is about to end. Record it so the read loop can stop for the right
    // reason and still keep what it captured; treating the following EOF as a
    // transport error is how a complete capture gets discarded.
    case Op::close_connection:
        server_closed_ = true;
        if (log_)
            log_("server closed the connection");
        break;
    default:
        note_unhandled(op);
        break;
    }
}

void Session::handle_token(Op op, std::int32_t token, Reader& reader)
{
    auto& draw = state(token);
    switch (op) {
    case Op::set_high_color:
        draw.high = reader.color();
        break;
    case Op::set_low_color:
        draw.low = reader.color();
        break;
    case Op::set_pen_size:
        draw.pen_size = reader.f32();
        break;
    case Op::set_pattern: {
        const auto pattern = reader.raw(8);
        std::copy(pattern.begin(), pattern.end(), draw.pattern.begin());
        break;
    }
    case Op::set_font:
        draw.font = reader.font();
        break;
    case Op::set_transform:
        draw.transform = reader.transform();
        break;
    case Op::set_subpixel_precise:
        // One byte, Add(bool) (RemoteDrawingEngine::SetSubpixelPrecise on
        // DeBeOS feat/remote-subpixel-precise). Any non-zero byte is true, as
        // a C++ bool read back through memcpy would be on the server.
        draw.subpixel_precise = reader.u8() != 0;
        break;
    case Op::set_offsets:
        draw.x_offset = reader.i32();
        draw.y_offset = reader.i32();
        break;
    case Op::set_stroke_mode:
        draw.line_cap = reader.u32();
        draw.line_join = reader.u32();
        draw.miter_limit = reader.f32();
        break;
    case Op::set_blending_mode:
        draw.constant_alpha = reader.u32() == 1;
        // The second word is `alpha_function`. B_ALPHA_OVERLAY (0) and
        // B_ALPHA_COMPOSITE (1) are *not* pixel-identical on an opaque canvas,
        // which this comment used to claim: the shape span functions differ in
        // their arithmetic, BLEND16 on a 16-bit alpha for overlay against
        // BLEND_COMPOSITE16's `alpha / 255` then an 8-bit BLEND for composite
        // (drawing_modes/DrawingModeAlphaCO.h:15-18 versus AlphaCC.h:15-18),
        // and they disagree by one in a channel at many partial coverages and
        // translucent alphas. The shape renderer (src/painter.cpp) honours the
        // word. The Porter-Duff functions after those two
        // (B_ALPHA_COMPOSITE_SOURCE_IN and the eleven that follow,
        // headers/os/interface/GraphicsDefs.h:331-345) are still treated as
        // overlay: they are only used to draw into offscreen BBitmaps, which
        // never cross this wire (issue #22).
        draw.alpha_function = reader.u32();
        if (draw.blend_modes_enabled)
            draw.force_opaque = draw.constant_alpha;
        break;
    case Op::set_drawing_mode: {
        const auto raw = reader.u32();
        draw.drawing_mode = raw <= 10 ? static_cast<DrawingMode>(raw)
                                      : DrawingMode::over;
        draw.force_opaque = draw.drawing_mode == DrawingMode::copy;
        draw.blend_modes_enabled = draw.drawing_mode == DrawingMode::alpha;
        if (draw.blend_modes_enabled)
            draw.force_opaque = draw.constant_alpha;
        break;
    }
    case Op::constrain_clipping_region:
        draw.clip_rects = reader.region();
        draw.clipping_set = true;
        break;
    // Every shape message is one DrawingEngine call, so each goes to the
    // Surface method named after that call, with its arguments in wire order
    // (RemoteDrawingEngine.cpp:594-1072). The surface does what the server's
    // engine and Painter would do with them; nothing is pre-processed here.
    case Op::fill_rect: {
        const auto rect = reader.rect();
        surface_.fill_rect(rect, draw);
        break;
    }
    case Op::fill_rect_color: {
        const auto rect = reader.rect();
        const auto color = reader.color();
        surface_.fill_rect_color(rect, color, &draw);
        break;
    }
    case Op::stroke_rect_1px_color: {
        const auto rect = reader.rect();
        surface_.stroke_rect(rect, reader.color(), &draw);
        break;
    }
    case Op::stroke_point_color: {
        const auto point = reader.point();
        const auto color = reader.color();
        surface_.stroke_point_color(point, color, draw);
        break;
    }
    case Op::stroke_line_1px_color: {
        const auto from = reader.point();
        const auto to = reader.point();
        surface_.stroke_line_color(from, to, reader.color(), draw);
        break;
    }
    case Op::stroke_line: {
        // Read into locals: the evaluation order of function arguments is
        // unspecified in C++, and g++ evaluates them right to left, which
        // would take the endpoints off the wire backwards.
        const auto from = reader.point();
        const auto to = reader.point();
        surface_.stroke_line(from, to, draw);
        break;
    }
    case Op::stroke_line_gradient: {
        const auto from = reader.point();
        const auto to = reader.point();
        const auto gradient = reader.gradient();
        surface_.stroke_line(from, to, draw, &gradient);
        break;
    }
    case Op::stroke_line_array: {
        const auto count = reader.i32();
        if (count < 0 || count > (1 << 20))
            throw ProtocolError("invalid line-array count");
        std::vector<ColoredLine> lines;
        lines.reserve(static_cast<std::size_t>(count));
        for (std::int32_t i = 0; i < count; ++i) {
            const auto from = reader.point();
            const auto to = reader.point();
            const auto color = reader.color();
            lines.push_back({from, to, color});
        }
        surface_.stroke_line_array(lines, draw);
        break;
    }
    case Op::stroke_rect:
        surface_.stroke_rect(reader.rect(), draw);
        break;
    case Op::stroke_rect_gradient: {
        const auto rect = reader.rect();
        const auto gradient = reader.gradient();
        surface_.stroke_rect(rect, draw, &gradient);
        break;
    }
    case Op::fill_rect_gradient: {
        const auto rect = reader.rect();
        const auto gradient = reader.gradient();
        surface_.fill_rect(rect, draw, &gradient);
        break;
    }
    case Op::fill_round_rect:
    case Op::stroke_round_rect:
    case Op::fill_round_rect_gradient:
    case Op::stroke_round_rect_gradient: {
        const auto rect = reader.rect();
        // RemoteDrawingEngine sends the rect, then xRadius, then yRadius
        // (RemoteDrawingEngine.cpp:820-825). Read them into locals: argument
        // evaluation order is unspecified and g++ runs it right to left, which
        // exchanged the two radii.
        const auto x_radius = reader.f32();
        const auto y_radius = reader.f32();
        const bool filled = op == Op::fill_round_rect
            || op == Op::fill_round_rect_gradient;
        std::optional<Gradient> gradient;
        if (op == Op::fill_round_rect_gradient || op == Op::stroke_round_rect_gradient)
            gradient = reader.gradient();
        const Gradient* g = gradient ? &*gradient : nullptr;
        if (filled)
            surface_.fill_round_rect(rect, x_radius, y_radius, draw, g);
        else
            surface_.stroke_round_rect(rect, x_radius, y_radius, draw, g);
        break;
    }
    case Op::fill_ellipse:
        surface_.fill_ellipse(reader.rect(), draw);
        break;
    case Op::stroke_ellipse:
        surface_.stroke_ellipse(reader.rect(), draw);
        break;
    case Op::fill_ellipse_gradient:
    case Op::stroke_ellipse_gradient: {
        const auto rect = reader.rect();
        const auto gradient = reader.gradient();
        if (op == Op::fill_ellipse_gradient)
            surface_.fill_ellipse(rect, draw, &gradient);
        else
            surface_.stroke_ellipse(rect, draw, &gradient);
        break;
    }
    case Op::fill_arc:
    case Op::stroke_arc:
    case Op::fill_arc_gradient:
    case Op::stroke_arc_gradient: {
        const auto rect = reader.rect();
        const auto angle = reader.f32();
        const auto span = reader.f32();
        std::optional<Gradient> gradient;
        if (op == Op::fill_arc_gradient || op == Op::stroke_arc_gradient)
            gradient = reader.gradient();
        const Gradient* g = gradient ? &*gradient : nullptr;
        if (op == Op::fill_arc || op == Op::fill_arc_gradient)
            surface_.fill_arc(rect, angle, span, draw, g);
        else
            surface_.stroke_arc(rect, angle, span, draw, g);
        break;
    }
    case Op::invert_rect:
        surface_.invert_rect(reader.rect(), &draw);
        break;
    case Op::fill_region: {
        const auto region = reader.region();
        surface_.fill_region(region, draw);
        break;
    }
    case Op::fill_region_gradient: {
        const auto region = reader.region();
        const auto gradient = reader.gradient();
        surface_.fill_region(region, draw, &gradient);
        break;
    }
    case Op::fill_polygon:
    case Op::stroke_polygon:
    case Op::fill_polygon_gradient:
    case Op::stroke_polygon_gradient: {
        const auto bounds = reader.rect();
        const bool closed = reader.boolean();
        const auto count = reader.i32();
        if (count < 0 || count > (1 << 20))
            throw ProtocolError("invalid polygon point count");
        std::vector<Point> points;
        points.reserve(static_cast<std::size_t>(count));
        for (std::int32_t i = 0; i < count; ++i)
            points.push_back(reader.point());
        const bool filled = op == Op::fill_polygon
            || op == Op::fill_polygon_gradient;
        std::optional<Gradient> gradient;
        if (op == Op::fill_polygon_gradient || op == Op::stroke_polygon_gradient)
            gradient = reader.gradient();
        surface_.draw_polygon(points, bounds, filled, closed, draw,
                              gradient ? &*gradient : nullptr);
        break;
    }
    case Op::fill_bezier:
    case Op::stroke_bezier:
    case Op::fill_bezier_gradient:
    case Op::stroke_bezier_gradient: {
        std::array<Point, 4> points;
        for (auto& point : points)
            point = reader.point();
        const bool filled = op == Op::fill_bezier
            || op == Op::fill_bezier_gradient;
        std::optional<Gradient> gradient;
        if (op == Op::fill_bezier_gradient || op == Op::stroke_bezier_gradient)
            gradient = reader.gradient();
        surface_.draw_bezier(points, filled, draw, gradient ? &*gradient : nullptr);
        break;
    }
    case Op::fill_triangle:
    case Op::stroke_triangle:
    case Op::fill_triangle_gradient:
    case Op::stroke_triangle_gradient: {
        std::array<Point, 3> points;
        for (auto& point : points)
            point = reader.point();
        const auto bounds = reader.rect();
        const bool filled = op == Op::fill_triangle
            || op == Op::fill_triangle_gradient;
        std::optional<Gradient> gradient;
        if (op == Op::fill_triangle_gradient || op == Op::stroke_triangle_gradient)
            gradient = reader.gradient();
        surface_.draw_triangle(points, bounds, filled, draw,
                               gradient ? &*gradient : nullptr);
        break;
    }
    case Op::fill_shape:
    case Op::stroke_shape:
    case Op::fill_shape_gradient:
    case Op::stroke_shape_gradient: {
        (void)reader.rect();
        const auto op_count = reader.i32();
        if (op_count < 0 || op_count > (1 << 20))
            throw ProtocolError("invalid shape op count");
        std::vector<std::uint32_t> operations;
        operations.reserve(static_cast<std::size_t>(op_count));
        for (std::int32_t i = 0; i < op_count; ++i)
            operations.push_back(reader.u32());
        const auto point_count = reader.i32();
        if (point_count < 0 || point_count > (1 << 20))
            throw ProtocolError("invalid shape point count");
        std::vector<Point> points;
        points.reserve(static_cast<std::size_t>(point_count));
        for (std::int32_t i = 0; i < point_count; ++i)
            points.push_back(reader.point());
        const auto offset = reader.point();
        const auto scale = reader.f32();
        const bool filled = op == Op::fill_shape
            || op == Op::fill_shape_gradient;
        std::optional<Gradient> gradient;
        if (op == Op::fill_shape_gradient || op == Op::stroke_shape_gradient)
            gradient = reader.gradient();
        surface_.draw_shape(operations, points, offset, scale, filled, draw,
                            gradient ? &*gradient : nullptr);
        break;
    }
    case Op::draw_bitmap: {
        const auto source = reader.rect();
        const auto destination = reader.rect();
        // The options word: B_TILE_BITMAP_X/_Y and B_FILTER_BITMAP_BILINEAR,
        // straight off BView::DrawBitmap (RemoteDrawingEngine.cpp:470). It used
        // to be dropped here, which turned every BView::DrawTiledBitmap into one
        // stretched copy.
        const auto options = reader.u32();
        const auto bitmap = read_bitmap(reader, draw);
        surface_.draw_bitmap(bitmap, source, destination, draw, options);
        break;
    }
    case Op::draw_bitmap_rects: {
        const auto options = reader.u32();
        const auto color_space = reader.u32();
        (void)reader.u32();
        const auto count = reader.i32();
        if (count < 0 || count > (1 << 16))
            throw ProtocolError("invalid bitmap rectangle count");
        // The tiling bits cannot be honoured on this path and must not be
        // guessed at. Each rect arrives as pixels the server already extracted
        // for that rect (RemoteDrawingEngine.cpp:405-423, _ExtractBitmapRegions),
        // and the view rect the tile phase is measured from is not on the wire at
        // all -- so wrapping per destination rect would invent a phase rather
        // than reproduce one. The filter bit, by contrast, is ours to honour and
        // is worth honouring: the server only scales server-side when it
        // *minifies* (ibid. :1308-1310), so a clipped magnification arrives
        // unscaled and unfiltered, and filtering it here is what the local
        // desktop does.
        const auto rect_options = options & ~tile_bitmap;
        for (std::int32_t i = 0; i < count; ++i) {
            const auto destination = reader.rect();
            const auto bitmap = read_bitmap(reader, draw, true, color_space);
            surface_.draw_bitmap(
                bitmap, {0, 0, static_cast<float>(bitmap.width - 1),
                         static_cast<float>(bitmap.height - 1)},
                destination, draw, rect_options);
        }
        break;
    }
    case Op::draw_string: {
        const auto where = reader.point();
        const auto text = reader.string();
        // After the string the server writes Add(delta != NULL) -- a one-byte
        // bool -- and, only when that is set, one escapement_delta
        // { float nonspace; float space; }
        // (RemoteDrawingEngine::DrawString, RemoteDrawingEngine.cpp:985-995).
        // The two floats go into named locals: as two arguments of one call the
        // evaluation order is unspecified and g++ evaluates right to left, which
        // would silently exchange nonspace and space.
        //
        // A short payload costs only the delta, not the whole advance: throwing
        // here would drop us into answer_after_failure(), which can only reply
        // with the bare `where`, losing the glyph advance as well. Degrading by
        // the smaller amount is the better trade.
        EscapementDelta delta;
        bool has_delta = false;
        if (reader.remaining() >= 1) {
            has_delta = reader.boolean();
            if (has_delta && reader.remaining() >= 8) {
                const float nonspace = reader.f32();
                const float space = reader.f32();
                delta = {nonspace, space};
            } else {
                has_delta = false;
            }
        }
        // Painter::DrawString rounds the baseline to whole pixels unless the
        // view is B_SUBPIXEL_PRECISE (Painter.cpp:1485-1488); only the glyphs
        // move, the reply below still answers from `where`.
        Point baseline = where;
        if (!draw.subpixel_precise) {
            baseline.x = std::round(where.x);
            baseline.y = std::round(where.y);
        }
        const float advance = text_.draw(text, baseline, draw, surface_,
                                         has_delta ? &delta : nullptr);
        if (std::getenv("HAIKU_REMOTE_TRACE_TEXT") != nullptr && log_) {
            std::ostringstream trace;
            trace << "text #" << message_count_ << " token=" << token
                  << " at=" << where.x << ',' << where.y
                  << " advance=" << advance
                  << " font=" << draw.font.size
                  << " offset=" << draw.x_offset << ',' << draw.y_offset
                  << " delta=";
            if (has_delta)
                trace << delta.nonspace << '/' << delta.space;
            else
                trace << "none";
            // Leftover payload is reported rather than rejected: the decoder
            // cannot tell a trailing field it does not know about from a
            // corrupt message, and refusing the message would cost the server
            // its mandatory reply and a 1 s stall per string.
            trace << " left=" << reader.remaining()
                  << " clip=";
            if (draw.clip_rects.empty()) {
                trace << "none";
            } else {
                float left = draw.clip_rects.front().left;
                float top = draw.clip_rects.front().top;
                float right = draw.clip_rects.front().right;
                float bottom = draw.clip_rects.front().bottom;
                for (const auto& rect : draw.clip_rects) {
                    left = std::min(left, rect.left);
                    top = std::min(top, rect.top);
                    right = std::max(right, rect.right);
                    bottom = std::max(bottom, rect.bottom);
                }
                trace << left << ',' << top << '-' << right << ',' << bottom
                      << " rects=[";
                for (std::size_t i = 0; i < draw.clip_rects.size(); ++i) {
                    if (i != 0)
                        trace << ';';
                    const auto& rect = draw.clip_rects[i];
                    trace << rect.left << ',' << rect.top << '-'
                          << rect.right << ',' << rect.bottom;
                }
                trace << ']';
            }
            trace << " value=\"";
            for (const unsigned char character : text) {
                if (character >= 0x20 && character < 0x7f
                    && character != '\\' && character != '"') {
                    trace << static_cast<char>(character);
                } else {
                    trace << "\\x";
                    constexpr char hex[] = "0123456789abcdef";
                    trace << hex[character >> 4] << hex[character & 0x0f];
                }
            }
            trace << '"';
            log_(trace.str());
        }
        Writer reply(Op::draw_string_result);
        reply.i32(token);
        reply.point({where.x + advance, where.y});
        send_message(reply.finish());
        break;
    }
    case Op::draw_string_with_offsets: {
        const std::string text = reader.string();
        const std::string_view view(text);
        Point last {};
        std::string_view last_scalar;
        // The server sends exactly one point per glyph *as it counts glyphs*:
        // UTF8CountChars(), which counts non-continuation bytes and stops at an
        // embedded NUL. Walking the string by decoded sequence length instead
        // disagrees on anything that is not well-formed UTF-8 -- a stray
        // continuation byte (Latin-1 "(c)", "+/-", "deg") or a NUL inside the
        // length -- and then the loop reads more points than were sent. That
        // read throws, the throw skips the reply below, and the server's
        // drawing thread blocks on its 1 s RP_DRAW_STRING_RESULT timeout. Count
        // glyphs the way the sender does.
        for (std::size_t offset = 0; offset < view.size();) {
            if (view[offset] == '\0')
                break;
            if (is_utf8_continuation(view[offset])) {
                // Not a glyph start to the server, so no point was sent for it.
                ++offset;
                continue;
            }
            std::size_t length = 1;
            while (offset + length < view.size() && view[offset + length] != '\0'
                   && is_utf8_continuation(view[offset + length])) {
                ++length;
            }
            if (reader.remaining() < point_wire_size)
                break;
            last = reader.point();
            last_scalar = view.substr(offset, length);
            text_.draw(last_scalar, last, draw, surface_);
            offset += length;
        }
        Writer reply(Op::draw_string_result);
        reply.i32(token);
        if (!last_scalar.empty())
            last.x += text_.width(last_scalar, draw.font);
        reply.point(last);
        send_message(reply.finish());
        break;
    }
    case Op::string_width: {
        const auto text = reader.string();
        Writer reply(Op::string_width_result);
        reply.i32(token);
        reply.f32(text_.width(text, draw.font));
        send_message(reply.finish());
        break;
    }
    case Op::read_bitmap: {
        const auto bounds = reader.rect();
        // RemoteDrawingEngine.cpp:1190 sends the caller's drawCursor verbatim
        // (BScreen::GetBitmap's includeCursor, ServerApp.cpp:3393-3405).
        const bool draw_cursor = reader.boolean();
        // RP_READ_BITMAP is synchronous and expensive to ignore: the server
        // blocks a drawing thread for up to 10 s waiting for the result, and on
        // the screenshot path it holds the desktop drawing engine's exclusive
        // lock while it waits, so the whole desktop stops painting. Every exit
        // from this case must therefore answer -- including the degenerate
        // requests (an empty rect, a rect that misses the surface entirely, a
        // rect so large the readback would blow the safety limit), which
        // previously returned or threw with no reply at all.
        const auto requested = raster_bounds(bounds);
        send_message(read_bitmap_reply(token, requested, draw_cursor));
        break;
    }
    case Op::enable_sync_drawing:
    case Op::disable_sync_drawing:
        break;
    default:
        note_unhandled(op);
        break;
    }
}

Bitmap Session::read_bitmap(Reader& reader, const DrawState& draw,
                            bool minimal, std::uint32_t inherited_color_space)
{
    // BitmapPainter.cpp:265-278: B_OP_COPY keeps the reserved value as an
    // ordinary colour, and B_OP_ALPHA treats a B_RGB32 bitmap as B_RGBA32
    // (BeOS compatibility), so neither mode substitutes transparency.
    const bool honour_transparent_magic =
        draw.drawing_mode != DrawingMode::copy
        && draw.drawing_mode != DrawingMode::alpha;

    Bitmap result;
    result.width = reader.i32();
    result.height = reader.i32();
    const int bytes_per_row = reader.i32();
    const std::uint32_t color_space = minimal ? inherited_color_space : reader.u32();
    if (!minimal)
        (void)reader.u32();
    const auto bits_size = static_cast<std::size_t>(reader.u32());
    if (result.width <= 0 || result.height <= 0 || bytes_per_row <= 0
        || result.width > Surface::max_dimension
        || result.height > Surface::max_dimension) {
        throw ProtocolError("invalid bitmap dimensions");
    }

    std::size_t minimum_row_size = 0;
    switch (color_space) {
    case b_rgb32:
    case b_rgba32:
        minimum_row_size = static_cast<std::size_t>(result.width) * 4;
        break;
    case b_rgb24:
        minimum_row_size = static_cast<std::size_t>(result.width) * 3;
        break;
    case b_gray8:
    case b_cmap8:
        minimum_row_size = static_cast<std::size_t>(result.width);
        break;
    case b_gray1:
        minimum_row_size = (static_cast<std::size_t>(result.width) + 7) / 8;
        break;
    default:
        throw ProtocolError("unsupported bitmap color space");
    }
    if (static_cast<std::size_t>(bytes_per_row) < minimum_row_size)
        throw ProtocolError("bitmap row is shorter than its pixel data");
    const auto required_bits = static_cast<std::size_t>(bytes_per_row)
        * static_cast<std::size_t>(result.height);
    if (bits_size < required_bits || bits_size > reader.remaining())
        throw ProtocolError("bitmap data is truncated");
    const auto decoded_size = static_cast<std::size_t>(result.width)
        * static_cast<std::size_t>(result.height) * 4;
    if (decoded_size > max_decoded_bitmap_size)
        throw ProtocolError("decoded bitmap exceeds the 256 MiB safety limit");

    const auto bits = reader.raw(bits_size);
    result.bgra.resize(decoded_size);
    for (int y = 0; y < result.height; ++y) {
        const auto row = static_cast<std::size_t>(y)
            * static_cast<std::size_t>(bytes_per_row);
        for (int x = 0; x < result.width; ++x) {
            Color color;
            switch (color_space) {
            case b_rgb32:
            case b_rgba32: {
                const auto source = row + static_cast<std::size_t>(x * 4);
                color = {bits.at(source + 2), bits.at(source + 1),
                         bits.at(source), color_space == b_rgba32
                             ? bits.at(source + 3) : std::uint8_t {255}};
                // B_RGB32 has no alpha channel, so BeOS/Haiku carry
                // "see-through" in a reserved pixel value. app_server's own
                // painter rewrites it to alpha 0 before blending in every mode
                // except B_OP_COPY and B_OP_ALPHA:
                // BitmapPainter.cpp:262-307 (_ConvertColorSpace ->
                // _TransparentMagicToAlpha, B_TRANSPARENT_MAGIC_RGBA32).
                if (color_space == b_rgb32 && honour_transparent_magic
                    && color.b == 0x77 && color.g == 0x74 && color.r == 0x77) {
                    color.a = 0;
                }
                break;
            }
            case b_rgb24: {
                const auto source = row + static_cast<std::size_t>(x * 3);
                color = {bits.at(source + 2), bits.at(source + 1), bits.at(source), 255};
                break;
            }
            case b_gray8: {
                const auto value = bits.at(row + static_cast<std::size_t>(x));
                color = {value, value, value, 255};
                break;
            }
            case b_gray1: {
                // Haiku's own reader is MSB-first and treats a *set* bit as
                // black: ColorConversion.cpp:556-567
                //   shift = 7 - (index % 8);
                //   result = ((**source >> shift) & 0x01) ? 0x00 : 0xFF;
                // The HTML5 reference client reads bit (index % 8) and maps a
                // set bit to white, which is mirrored and inverted; it is not
                // an oracle.
                const auto byte = bits.at(row + static_cast<std::size_t>(x / 8));
                const auto shift = 7 - (x & 7);
                const auto value = ((byte >> shift) & 0x01) != 0 ? 0 : 255;
                color = {static_cast<std::uint8_t>(value),
                         static_cast<std::uint8_t>(value),
                         static_cast<std::uint8_t>(value), 255};
                break;
            }
            case b_cmap8: {
                const auto index = bits.at(row + static_cast<std::size_t>(x));
                color = index < palette_.size() ? palette_[index] : Color {};
                break;
            }
            }
            const auto destination = (
                static_cast<std::size_t>(y)
                    * static_cast<std::size_t>(result.width)
                + static_cast<std::size_t>(x)) * 4;
            result.bgra[destination] = color.b;
            result.bgra[destination + 1] = color.g;
            result.bgra[destination + 2] = color.r;
            result.bgra[destination + 3] = color.a;
        }
    }
    return result;
}

IntRect composite_cursor(const CursorState& cursor, std::span<std::uint8_t> bgra,
                         int width, int height, std::size_t stride)
{
    if (!cursor.drawable() || width <= 0 || height <= 0
        || stride < static_cast<std::size_t>(width) * 4) {
        return {};
    }
    const auto required = stride * static_cast<std::size_t>(height - 1)
        + static_cast<std::size_t>(width) * 4;
    if (bgra.size() < required)
        return {};

    const IntRect frame = cursor.bounds();
    const IntRect clipped = intersect(frame, {0, 0, width - 1, height - 1});
    if (clipped.empty())
        return {};
    const auto source_stride = static_cast<std::size_t>(cursor.bitmap.width) * 4;
    for (int y = clipped.top; y <= clipped.bottom; ++y) {
        const auto source_row =
            static_cast<std::size_t>(y - frame.top) * source_stride;
        auto* destination_row = bgra.data()
            + static_cast<std::size_t>(y) * stride;
        for (int x = clipped.left; x <= clipped.right; ++x) {
            const auto source =
                source_row + static_cast<std::size_t>(x - frame.left) * 4;
            const unsigned alpha = cursor.bitmap.bgra[source + 3];
            if (alpha == 0)
                continue;
            auto* destination = destination_row + static_cast<std::size_t>(x) * 4;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const unsigned over = cursor.bitmap.bgra[source + channel];
                destination[channel] = alpha == 255
                    ? static_cast<std::uint8_t>(over)
                    : static_cast<std::uint8_t>(
                        (over * alpha + destination[channel] * (255 - alpha) + 127)
                            / 255);
            }
            // The framebuffer is opaque; a cursor never makes it see-through.
            destination[3] = 255;
        }
    }
    return clipped;
}


IntRect composite_cursor(const CursorState& cursor, Surface& target)
{
    return composite_cursor(cursor, target.pixels(), target.width(),
                            target.height(),
                            static_cast<std::size_t>(target.stride()));
}


void Session::note_unhandled(Op op)
{
    auto& count = unhandled_[static_cast<std::uint16_t>(op)];
    ++count;
    if (count == 1 && log_) {
        std::ostringstream text;
        text << "unhandled op " << op_name(op)
             << " (" << static_cast<std::uint16_t>(op) << ')';
        log_(text.str());
    }
}

} // namespace haiku_remote
