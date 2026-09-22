#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/protocol.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"
#include "haiku_remote/transport.hpp"

#include <array>
#include <utility>
#include <limits>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#if defined(HAIKU_REMOTE_HAVE_WSS) && !defined(_WIN32)
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#endif

using namespace haiku_remote;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void append_rect(Writer& writer, Rect rect)
{
    writer.f32(rect.left);
    writer.f32(rect.top);
    writer.f32(rect.right);
    writer.f32(rect.bottom);
}

void append_test_gradient(Writer& writer)
{
    writer.u32(0);
    writer.point({0, 0});
    writer.point({32, 32});
    writer.i32(2);
    writer.u8(255);
    writer.u8(0);
    writer.u8(0);
    writer.u8(255);
    writer.f32(0);
    writer.u8(0);
    writer.u8(0);
    writer.u8(255);
    writer.u8(255);
    writer.f32(255);
}

void test_framer()
{
    Writer writer(Op::update_display_mode);
    writer.i32(1280);
    writer.i32(800);
    const auto bytes = writer.finish();
    Framer framer;
    check(framer.feed(std::span(bytes).first(3)).empty(),
          "partial header does not produce a message");
    const auto messages = framer.feed(std::span(bytes).subspan(3));
    check(messages.size() == 1, "fragmented message is reassembled");
    Reader reader(messages.front().payload);
    check(reader.i32() == 1280 && reader.i32() == 800,
          "message payload is little-endian");

    Writer oversized(Op::update_display_mode);
    auto oversized_bytes = oversized.finish();
    oversized_bytes[2] = 1;
    oversized_bytes[3] = 0;
    oversized_bytes[4] = 0;
    oversized_bytes[5] = 4;
    bool rejected = false;
    try {
        Framer invalid;
        (void)invalid.feed(oversized_bytes);
    } catch (const ProtocolError&) {
        rejected = true;
    }
    check(rejected, "framer rejects declared messages above its safety limit");
}

void test_surface_dimension_validation()
{
    bool negative_rejected = false;
    bool oversized_rejected = false;
    try {
        Surface invalid(-1, 100);
    } catch (const std::invalid_argument&) {
        negative_rejected = true;
    }
    try {
        Surface invalid(Surface::max_dimension + 1, 1);
    } catch (const std::invalid_argument&) {
        oversized_rejected = true;
    }
    check(negative_rejected, "negative surface dimensions fail before allocation");
    check(oversized_rejected, "oversized surface dimensions fail before allocation");
}

void test_inclusive_rect()
{
    Surface surface(8, 8);
    DrawState state;
    state.drawing_mode = DrawingMode::copy;
    state.high = {10, 20, 30, 255};
    surface.fill_rect({2, 3, 4, 5}, state);
    int count = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            if (surface.pixel(x, y) == state.high)
                ++count;
    check(count == 9, "inclusive 3x3 rectangle covers nine pixels");
}

void test_pattern_phase()
{
    Surface surface(8, 1);
    DrawState state;
    state.drawing_mode = DrawingMode::copy;
    state.high = {255, 0, 0, 255};
    state.low = {0, 0, 255, 255};
    state.pattern.fill(0xf0);
    surface.fill_rect({0, 0, 7, 0}, state);
    check(surface.pixel(0, 0) == state.high && surface.pixel(3, 0) == state.high,
          "pattern uses MSB-first high bits");
    check(surface.pixel(4, 0) == state.low && surface.pixel(7, 0) == state.low,
          "vertical stripe pattern is not flattened");
}

void test_drawing_modes()
{
    Surface surface(1, 1);
    DrawState state;
    state.high = {100, 150, 40, 255};
    surface.set_pixel(0, 0, {200, 100, 40, 255});
    state.drawing_mode = DrawingMode::subtract;
    surface.fill_rect({0, 0, 0, 0}, state);
    check(surface.pixel(0, 0) == Color {100, 0, 0, 255},
          "subtract is per-channel and clamps at zero");

    surface.set_pixel(0, 0, {200, 100, 40, 255});
    state.drawing_mode = DrawingMode::blend;
    surface.fill_rect({0, 0, 0, 0}, state);
    check(surface.pixel(0, 0) == Color {150, 125, 40, 255},
          "blend is a 50 percent average");

    surface.set_pixel(0, 0, {10, 200, 10, 255});
    state.high = {100, 100, 100, 255};
    state.drawing_mode = DrawingMode::min;
    surface.fill_rect({0, 0, 0, 0}, state);
    check(surface.pixel(0, 0) == Color {100, 100, 100, 255},
          "minimum compares whole-pixel brightness");
}

void test_affine_transform()
{
    Surface surface(20, 20);
    surface.clear({255, 255, 255, 255});
    DrawState state;
    state.drawing_mode = DrawingMode::copy;
    state.high = {255, 0, 0, 255};
    state.transform.tx = 5;
    state.transform.ty = 3;
    surface.fill_rect({1, 1, 2, 2}, state);
    check(surface.pixel(6, 4) == state.high,
          "affine translation maps filled geometry");
    check(surface.pixel(1, 1) == Color {255, 255, 255, 255},
          "affine geometry does not paint at the untransformed origin");

    Bitmap bitmap;
    bitmap.width = 1;
    bitmap.height = 1;
    bitmap.bgra = {0, 255, 0, 255};
    surface.draw_bitmap(bitmap, {0, 0, 0, 0}, {2, 2, 2, 2}, state);
    check(surface.pixel(7, 5) == Color {0, 255, 0, 255},
          "affine translation maps bitmap destinations");
}

void test_stroke_width_and_pattern()
{
    Surface surface(24, 24);
    surface.clear({255, 255, 255, 255});
    DrawState state;
    state.drawing_mode = DrawingMode::copy;
    state.high = {255, 0, 0, 255};
    state.low = {0, 0, 255, 255};
    state.pen_size = 5;
    state.pattern.fill(0xf0);
    surface.line({4, 12}, {20, 12}, state.high, &state, true);
    check(surface.pixel(8, 14) != Color {255, 255, 255, 255},
          "pen size produces a multi-pixel stroke");
    check(surface.pixel(8, 12) != surface.pixel(12, 12),
          "general strokes preserve the 8x8 pattern");

    surface.clear({255, 255, 255, 255});
    surface.stroke_rect({6, 6, 16, 16}, {0, 0, 0, 255}, &state);
    check(surface.pixel(10, 8) == Color {255, 255, 255, 255},
          "explicit one-pixel rectangle ignores the current pen size");
}

void test_copy_is_overlap_safe()
{
    Surface surface(1, 4);
    surface.set_pixel(0, 0, {1, 0, 0, 255});
    surface.set_pixel(0, 1, {2, 0, 0, 255});
    surface.set_pixel(0, 2, {3, 0, 0, 255});
    surface.copy_rect({0, 0, 0, 2}, 0, 1);
    check(surface.pixel(0, 1).r == 1 && surface.pixel(0, 2).r == 2
              && surface.pixel(0, 3).r == 3,
          "copyRect snapshots before overlapping writes");
}

void test_text_shapes_and_rasterizes()
{
    Surface surface(160, 40);
    surface.clear({216, 216, 216, 255});
    DrawState state;
    state.drawing_mode = DrawingMode::over;
    state.force_opaque = false;
    state.high = {0, 0, 0, 255};
    state.font.size = 12;
    TextEngine text;
    const float advance = text.draw("Haiku Remote", {4, 20}, state, surface);
    int changed = 0;
    int partially_covered = 0;
    int subpixel_covered = 0;
    for (int y = 0; y < surface.height(); ++y)
        for (int x = 0; x < surface.width(); ++x) {
            const auto pixel = surface.pixel(x, y);
            if (pixel != Color {216, 216, 216, 255})
                ++changed;
            if (pixel != Color {216, 216, 216, 255}
                && pixel != Color {0, 0, 0, 255})
                ++partially_covered;
            if (pixel.r != pixel.g || pixel.g != pixel.b)
                ++subpixel_covered;
        }
    check(advance > 40 && advance < 120, "HarfBuzz returns a plausible advance");
    check(changed > 40, "FreeType rasterizes glyph coverage into the surface");
    check(partially_covered > 20, "text includes antialiased edge coverage");
    check(subpixel_covered > 10, "text includes LCD subpixel coverage");

    Surface terminal_surface(80, 30);
    terminal_surface.clear({255, 255, 255, 255});
    DrawState terminal_state = state;
    terminal_state.font.spacing = 3;
    const Point terminal_baseline {4, 20};
    const float terminal_advance =
        text.draw("65536", terminal_baseline, terminal_state, terminal_surface);
    check(std::abs(terminal_advance - 35.0f) < 0.01f,
          "fixed text uses Haiku-compatible hinted 7px advances");
}

void test_input_messages()
{
    const auto down = InputEncoder::mouse_down(
        12.5f, 8.25f, buttons::primary | buttons::secondary, 2);
    Framer framer;
    const auto messages = framer.feed(down);
    check(messages.size() == 1 && messages.front().op == Op::mouse_down,
          "mouse-down encoder uses the correct opcode");
    Reader mouse(messages.front().payload);
    check(mouse.f32() == 12.5f && mouse.f32() == 8.25f
              && mouse.i32() == 3 && mouse.i32() == 2,
          "mouse-down preserves coordinates, buttons, and clicks");

    const auto key = InputEncoder::key(true, "A", 'a', 0x27);
    Framer key_framer;
    const auto key_messages = key_framer.feed(key);
    Reader key_reader(key_messages.front().payload);
    check(key_messages.front().op == Op::key_down && key_reader.string() == "A"
              && key_reader.i32() == 'a' && key_reader.i32() == 0x27,
          "key carries composed UTF-8 and Haiku key identity");
}

void test_line_array_payload()
{
    Writer writer(Op::stroke_line_array);
    writer.i32(7);
    writer.i32(2);
    writer.point({1, 2});
    writer.point({3, 4});
    writer.u8(10);
    writer.u8(20);
    writer.u8(30);
    writer.u8(255);
    writer.point({5, 6});
    writer.point({7, 8});
    writer.u8(40);
    writer.u8(50);
    writer.u8(60);
    writer.u8(255);
    const auto message = writer.finish();
    Framer framer;
    const auto framed = framer.feed(message);
    Reader reader(framed.front().payload);
    check(reader.i32() == 7 && reader.i32() == 2,
          "line-array payload starts with token and count");
    check(reader.point() == Point {1, 2} && reader.point() == Point {3, 4}
              && reader.color() == Color {10, 20, 30, 255},
          "line-array entry is two points followed by color");
}

void test_session_rejects_unsafe_bitmap()
{
    std::string log;
    Session session(32, 32, [](std::span<const std::uint8_t>) { return true; },
                    [&](std::string_view line) { log = line; });

    Writer create(Op::create_state);
    create.i32(7);
    session.ingest(create.finish());

    Writer bitmap(Op::draw_bitmap);
    bitmap.i32(7);
    bitmap.f32(0);
    bitmap.f32(0);
    bitmap.f32(0);
    bitmap.f32(0);
    bitmap.f32(0);
    bitmap.f32(0);
    bitmap.f32(0);
    bitmap.f32(0);
    bitmap.u32(0);
    bitmap.i32(Surface::max_dimension);
    bitmap.i32(Surface::max_dimension);
    bitmap.i32(1);
    bitmap.u32(0x0008);
    bitmap.u32(0);
    bitmap.u32(1);
    bitmap.u8(0);
    session.ingest(bitmap.finish());

    check(log.find("bitmap row is shorter") != std::string::npos,
          "unsafe bitmap dimensions are rejected without allocating");
}

void test_draw_string_with_offsets_replies()
{
    std::vector<std::uint8_t> reply_bytes;
    Session session(
        80, 30,
        [&](std::span<const std::uint8_t> bytes) {
            reply_bytes.assign(bytes.begin(), bytes.end());
            return true;
        });

    Writer create(Op::create_state);
    create.i32(9);
    session.ingest(create.finish());

    Writer draw(Op::draw_string_with_offsets);
    draw.i32(9);
    draw.string("AB");
    draw.point({2, 20});
    draw.point({12, 20});
    session.ingest(draw.finish());

    Framer framer;
    const auto replies = framer.feed(reply_bytes);
    check(replies.size() == 1 && replies.front().op == Op::draw_string_result,
          "offset text sends the blocking draw-string reply");
    Reader reader(replies.front().payload);
    const auto token = reader.i32();
    const auto pen = reader.point();
    check(token == 9 && pen.x > 12 && pen.y == 20,
          "offset text reply advances from the final codepoint");
}

// RP_DRAW_STRING carries an escapement_delta after the string: a one-byte bool
// and, when it is set, { float nonspace; float space; }
// (RemoteDrawingEngine.cpp:985-995). The delta is charged to every character's
// advance -- `space` for the scalars Haiku calls whitespace, `nonspace` for the
// rest (GlyphLayoutEngine.h:349-352) -- and the last character's share is folded
// into the pen position the server gets back (GlyphLayoutEngine.h:367-369).
// Ignoring the field painted justified and letter-spaced text at the wrong
// spacing and replied with a pen position short by the whole delta, so every
// later layout decision drifted with it.
//
// FreeType and HarfBuzz versions disagree on absolute pixel counts, so these
// checks compare a string against *itself* with and without a delta: the
// differences are the delta's own arithmetic and are font-independent.
struct DeltaTextResult {
    std::size_t painted = 0;
    int right = -1;
    float advance = 0;
    std::size_t replies = 0;
};

// Feeds one RP_DRAW_STRING exactly as RemoteDrawingEngine::DrawString writes it.
// `delta_bytes` chooses how much of the trailing delta reaches the client: 0 =
// no bool at all, 1 = bool only, 5 = bool and half a delta, 9 = the full field.
DeltaTextResult draw_string_with_delta(std::string_view text, bool has_delta,
                                       float nonspace, float space,
                                       int delta_bytes = 9)
{
    DeltaTextResult result;
    std::vector<std::uint8_t> reply_bytes;
    Session session(320, 80, [&](std::span<const std::uint8_t> bytes) {
        reply_bytes.insert(reply_bytes.end(), bytes.begin(), bytes.end());
        return true;
    });

    Writer create(Op::create_state);
    create.i32(7);
    session.ingest(create.finish());

    Writer color(Op::set_high_color);
    color.i32(7);
    color.u8(255);
    color.u8(255);
    color.u8(255);
    color.u8(255);
    session.ingest(color.finish());

    Writer draw(Op::draw_string);
    draw.i32(7);
    draw.point({10, 40});
    draw.string(text);
    if (delta_bytes >= 1)
        draw.boolean(has_delta);
    if (has_delta && delta_bytes >= 5)
        draw.f32(nonspace);
    if (has_delta && delta_bytes >= 9)
        draw.f32(space);
    session.ingest(draw.finish());

    const auto& surface = session.surface();
    for (int y = 0; y < surface.height(); ++y)
        for (int x = 0; x < surface.width(); ++x) {
            const auto pixel = surface.pixel(x, y);
            if (pixel.r != 0 || pixel.g != 0 || pixel.b != 0) {
                ++result.painted;
                result.right = std::max(result.right, x);
            }
        }

    Framer framer;
    for (const auto& message : framer.feed(reply_bytes)) {
        if (message.op != Op::draw_string_result)
            continue;
        ++result.replies;
        Reader reader(message.payload);
        (void)reader.i32();
        result.advance = reader.point().x - 10.0f;
    }
    return result;
}

void test_draw_string_applies_escapement_delta()
{
    // "Wide Open": eight non-space characters and one space, so a delta of
    // { nonspace = 12, space = 24 } owes 8 * 12 + 24 = 120 extra advance, and
    // moves the final glyph's ink by the 7 * 12 + 24 = 108 charged before it.
    const auto plain = draw_string_with_delta("Wide Open", false, 0, 0);
    const auto zero = draw_string_with_delta("Wide Open", true, 0, 0);
    const auto justified = draw_string_with_delta("Wide Open", true, 12, 24);

    check(plain.replies == 1 && zero.replies == 1 && justified.replies == 1,
          "every draw-string variant sends exactly one blocking reply");
    check(plain.advance > 0 && plain.right > 10,
          "the no-delta case paints and advances at all");
    check(std::abs(justified.advance - plain.advance - 120.0f) < 0.01f,
          "escapement delta lengthens the replied pen position by its own sum");
    check(std::abs(justified.right - plain.right - 108) <= 2,
          "escapement delta moves the last glyph's ink, not just the reply");

    // A delta that is present but zero has to be indistinguishable from none,
    // and a hasDelta=0 message has to behave exactly as it did before the field
    // was read at all.
    check(zero.advance == plain.advance && zero.right == plain.right
              && zero.painted == plain.painted,
          "a zero escapement delta changes nothing");

    TextEngine text;
    DrawState state;
    state.font.size = 12;
    check(std::abs(plain.advance - text.width("Wide Open", state.font)) < 0.01f,
          "a hasDelta=0 message still replies the plain string width");
}

void test_escapement_delta_distinguishes_space_from_nonspace()
{
    // "AB C": three non-space characters and one space. Exchanging the two
    // components changes both the total (3 * 4 + 40 = 52 against
    // 3 * 40 + 4 = 124) and the ink shift of the final glyph (4 + 4 + 40 = 48
    // against 40 + 40 + 4 = 84), so a swap cannot hide in either number.
    const auto plain = draw_string_with_delta("AB C", false, 0, 0);
    const auto forward = draw_string_with_delta("AB C", true, 4, 40);
    const auto swapped = draw_string_with_delta("AB C", true, 40, 4);

    check(std::abs(forward.advance - plain.advance - 52.0f) < 0.01f,
          "nonspace is charged to non-space characters only");
    check(std::abs(swapped.advance - plain.advance - 124.0f) < 0.01f,
          "space is charged to space characters only");
    check(std::abs(forward.right - plain.right - 48) <= 2,
          "the painted ink follows the nonspace component");
    check(std::abs(swapped.right - plain.right - 84) <= 2,
          "the painted ink follows the space component");
}

void test_escapement_delta_whitespace_set_matches_haiku()
{
    // GlyphLayoutEngine::IsWhiteSpace() is wider than U+0020: tab, the vertical
    // controls, and U+00A0 all take the `space` component. With nonspace = 0
    // the whole difference is the one whitespace character's share, whatever
    // the font does with it.
    struct Case {
        std::string text;
        float expected;
        const char* what;
    };
    const Case cases[] = {
        {"A B", 50.0f, "U+0020 space"},
        {"A\tB", 50.0f, "U+0009 tab"},
        {std::string("A\xc2\xa0" "B"), 50.0f, "U+00A0 non-breaking space"},
        {"AB", 0.0f, "no whitespace at all"},
    };

    for (const auto& item : cases) {
        const auto plain = draw_string_with_delta(item.text, false, 0, 0);
        const auto spaced = draw_string_with_delta(item.text, true, 0, 50);
        check(std::abs(spaced.advance - plain.advance - item.expected) < 0.01f,
              std::string("space component is charged for ") + item.what);
    }
}

// A delta the server truncated must cost only the delta. Throwing out of the
// handler would land in answer_after_failure(), whose best reply is the bare
// starting point -- losing the glyph advance as well, which is the larger error.
void test_draw_string_replies_when_the_delta_is_short()
{
    const auto plain = draw_string_with_delta("Wide Open", false, 0, 0);
    const struct {
        int delta_bytes;
        const char* what;
    } cases[] = {
        {0, "no delta bool at all"},
        {5, "a delta cut in half"},
    };

    for (const auto& item : cases) {
        const auto degraded =
            draw_string_with_delta("Wide Open", item.delta_bytes != 0, 12, 24,
                                   item.delta_bytes);
        check(degraded.replies == 1 && degraded.advance == plain.advance,
              std::string("a short payload keeps the plain advance with ")
                  + item.what);
    }
}

// The server counts glyphs with UTF8CountChars(): one offset point per
// non-continuation byte, stopping at an embedded NUL. A client that walks the
// string any other way reads past the payload on text that is not well-formed
// UTF-8, and the throw takes the mandatory reply with it -- 1 s of blocked
// server drawing thread per string.
void test_offset_text_replies_on_malformed_utf8()
{
    struct Case {
        std::string text;
        std::size_t server_points;
        const char* what;
    };
    const Case cases[] = {
        {std::string("\x80" "A", 2), 1, "a leading continuation byte"},
        {std::string("\xa9 2026", 6), 5, "Latin-1 (c) (0xa9)"},
        {std::string("35\xb1\xb0" "C", 5), 3, "Latin-1 +/- and degree"},
        {std::string("A\0B", 3), 1, "a NUL inside the string length"},
        {std::string("\xc3\xa9\xa9", 3), 1, "an over-long continuation run"},
    };

    for (const auto& item : cases) {
        std::vector<std::uint8_t> reply_bytes;
        Session session(80, 30, [&](std::span<const std::uint8_t> bytes) {
            reply_bytes.insert(reply_bytes.end(), bytes.begin(), bytes.end());
            return true;
        });

        Writer draw(Op::draw_string_with_offsets);
        draw.i32(5);
        draw.string(item.text);
        for (std::size_t i = 0; i < item.server_points; ++i)
            draw.point({2.0f + 8 * static_cast<float>(i), 20});
        session.ingest(draw.finish());

        Framer framer;
        const auto replies = framer.feed(reply_bytes);
        check(replies.size() == 1
                  && replies.front().op == Op::draw_string_result,
              std::string("offset text still replies with ") + item.what);
    }
}

// RP_READ_BITMAP is synchronous and the server holds the desktop drawing
// engine's exclusive lock for the whole 10 s wait, so no request may go
// unanswered -- not an empty rectangle, not one that misses the surface.
void test_read_bitmap_always_replies()
{
    struct Case {
        Rect bounds;
        int width;
        int height;
        const char* what;
    };
    const Case cases[] = {
        {{0, 0, 9, 9}, 10, 10, "an in-bounds rectangle"},
        {{500, 500, 540, 540}, 41, 41, "a rectangle that misses the surface"},
        {{50, 50, 89, 89}, 40, 40, "a rectangle that straddles the edge"},
        {{0, 0, -1, -1}, 1, 1, "an empty rectangle"},
    };

    for (const auto& item : cases) {
        std::vector<std::uint8_t> reply_bytes;
        Session session(60, 60, [&](std::span<const std::uint8_t> bytes) {
            reply_bytes.insert(reply_bytes.end(), bytes.begin(), bytes.end());
            return true;
        });

        Writer request(Op::read_bitmap);
        request.i32(1);
        append_rect(request, item.bounds);
        request.boolean(false);
        session.ingest(request.finish());

        Framer framer;
        const auto replies = framer.feed(reply_bytes);
        const bool answered = replies.size() == 1
            && replies.front().op == Op::read_bitmap_result;
        check(answered,
              std::string("read bitmap replies for ") + item.what);
        if (!answered)
            continue;
        Reader reader(replies.front().payload);
        const auto token = reader.i32();
        const auto width = reader.i32();
        const auto height = reader.i32();
        const auto bytes_per_row = reader.i32();
        reader.u32();
        reader.u32();
        const auto bits_size = reader.u32();
        check(token == 1 && width == item.width && height == item.height
                  && bits_size == static_cast<std::uint32_t>(bytes_per_row)
                      * static_cast<std::uint32_t>(height),
              std::string("read bitmap answers the requested geometry for ")
                  + item.what);
    }
}

// A decode failure must not swallow the reply the server is blocking on: a
// truncated RP_READ_BITMAP still has to release the drawing thread.
void test_truncated_sync_request_still_replies()
{
    std::vector<std::uint8_t> reply_bytes;
    Session session(60, 60, [&](std::span<const std::uint8_t> bytes) {
        reply_bytes.insert(reply_bytes.end(), bytes.begin(), bytes.end());
        return true;
    });

    Writer request(Op::read_bitmap);
    request.i32(3);
    request.f32(0);
    request.f32(0);
        // Rectangle cut short: the rest of the payload never arrives.
    session.ingest(request.finish());

    Framer framer;
    const auto replies = framer.feed(reply_bytes);
    check(replies.size() == 1 && replies.front().op == Op::read_bitmap_result,
          "a truncated read-bitmap request is still answered");

    reply_bytes.clear();
    Writer width(Op::string_width);
    width.i32(4);
        // No string follows.
    session.ingest(width.finish());
    Framer width_framer;
    const auto width_replies = width_framer.feed(reply_bytes);
    check(width_replies.size() == 1
              && width_replies.front().op == Op::string_width_result,
          "a truncated string-width request is still answered");
}

void test_extended_renderer_opcodes()
{
    Session session(64, 64, [](std::span<const std::uint8_t>) { return true; });
    Writer create(Op::create_state);
    create.i32(11);
    session.ingest(create.finish());

    const auto send_bezier = [&](Op op, bool gradient) {
        Writer writer(op);
        writer.i32(11);
        writer.point({4, 24});
        writer.point({12, 4});
        writer.point({20, 44});
        writer.point({28, 24});
        if (gradient)
            append_test_gradient(writer);
        session.ingest(writer.finish());
    };
    send_bezier(Op::fill_bezier, false);
    send_bezier(Op::stroke_bezier, false);
    send_bezier(Op::fill_bezier_gradient, true);
    send_bezier(Op::stroke_bezier_gradient, true);

    const auto send_rect_gradient = [&](Op op, bool radii) {
        Writer writer(op);
        writer.i32(11);
        append_rect(writer, {4, 4, 24, 20});
        if (radii) {
            writer.f32(4);
            writer.f32(4);
        }
        append_test_gradient(writer);
        session.ingest(writer.finish());
    };
    send_rect_gradient(Op::stroke_rect_gradient, false);
    send_rect_gradient(Op::fill_ellipse_gradient, false);
    send_rect_gradient(Op::stroke_ellipse_gradient, false);
    send_rect_gradient(Op::fill_round_rect_gradient, true);
    send_rect_gradient(Op::stroke_round_rect_gradient, true);

    const auto send_arc_gradient = [&](Op op) {
        Writer writer(op);
        writer.i32(11);
        append_rect(writer, {8, 8, 40, 40});
        writer.f32(0);
        writer.f32(180);
        append_test_gradient(writer);
        session.ingest(writer.finish());
    };
    send_arc_gradient(Op::fill_arc_gradient);
    send_arc_gradient(Op::stroke_arc_gradient);

    const auto send_polygon = [&](Op op, bool gradient) {
        Writer writer(op);
        writer.i32(11);
        append_rect(writer, {4, 4, 28, 28});
        writer.boolean(true);
        writer.i32(3);
        writer.point({4, 28});
        writer.point({16, 4});
        writer.point({28, 28});
        if (gradient)
            append_test_gradient(writer);
        session.ingest(writer.finish());
    };
    send_polygon(Op::stroke_polygon, false);
    send_polygon(Op::fill_polygon_gradient, true);
    send_polygon(Op::stroke_polygon_gradient, true);

    const auto send_triangle_gradient = [&](Op op) {
        Writer writer(op);
        writer.i32(11);
        writer.point({32, 4});
        writer.point({48, 28});
        writer.point({32, 28});
        append_rect(writer, {32, 4, 48, 28});
        append_test_gradient(writer);
        session.ingest(writer.finish());
    };
    send_triangle_gradient(Op::fill_triangle_gradient);
    send_triangle_gradient(Op::stroke_triangle_gradient);

    Writer region(Op::fill_region_gradient);
    region.i32(11);
    region.i32(1);
    append_rect(region, {36, 32, 52, 48});
    append_test_gradient(region);
    session.ingest(region.finish());

    Writer line(Op::stroke_line_gradient);
    line.i32(11);
    line.point({0, 0});
    line.point({63, 63});
    append_test_gradient(line);
    session.ingest(line.finish());

    const auto send_shape_gradient = [&](Op op) {
        Writer writer(op);
        writer.i32(11);
        append_rect(writer, {8, 32, 28, 52});
        writer.i32(3);
        writer.u32(0x80000000);
        writer.u32(0x10000003);
        writer.u32(0x40000000);
        writer.i32(4);
        writer.point({8, 52});
        writer.point({8, 32});
        writer.point({28, 32});
        writer.point({28, 52});
        writer.point({0, 0});
        writer.f32(1);
        append_test_gradient(writer);
        session.ingest(writer.finish());
    };
    send_shape_gradient(Op::fill_shape_gradient);
    send_shape_gradient(Op::stroke_shape_gradient);

    check(session.unhandled().empty(),
          "extended renderer opcode families are all handled");
    check(session.surface().pixel(16, 16) != Color {0, 0, 0, 255},
          "extended renderer operations paint the surface");
}


// ---------------------------------------------------------------------------
// Raster and geometry regressions found auditing surface.cpp against
// app_server's own rasterizer (see ~/Projects/Haiku-Graviton).
// ---------------------------------------------------------------------------

void test_empty_clipping_region_clips_everything()
{
    // RP_CONSTRAIN_CLIPPING_REGION carries a rect count, and zero is a legal
    // value meaning "nothing may be drawn" -- app_server reaches it on the
    // AS_VIEW_END_LAYER path (ServerWindow.cpp:2511-2533). Treating an empty
    // region as "unclipped" painted a fully obscured view over the screen.
    Session session(16, 16, [](std::span<const std::uint8_t>) { return true; });
    Writer create(Op::create_state);
    create.i32(7);
    session.ingest(create.finish());

    Writer clip(Op::constrain_clipping_region);
    clip.i32(7);
    clip.i32(0);
    session.ingest(clip.finish());

    Writer fill(Op::fill_rect_color);
    fill.i32(7);
    append_rect(fill, {0, 0, 15, 15});
    fill.u8(255);
    fill.u8(0);
    fill.u8(0);
    fill.u8(255);
    session.ingest(fill.finish());
    check(session.surface().pixel(8, 8) == Color {0, 0, 0, 255},
          "an empty clipping region clips the whole fill away");

    // ...and a state that has never been constrained still draws.
    Session open(16, 16, [](std::span<const std::uint8_t>) { return true; });
    Writer create_open(Op::create_state);
    create_open.i32(7);
    open.ingest(create_open.finish());
    Writer fill_open(Op::fill_rect_color);
    fill_open.i32(7);
    append_rect(fill_open, {0, 0, 15, 15});
    fill_open.u8(255);
    fill_open.u8(0);
    fill_open.u8(0);
    fill_open.u8(255);
    open.ingest(fill_open.finish());
    check(open.surface().pixel(8, 8) == Color {255, 0, 0, 255},
          "no clipping message at all still means unclipped");
}

void test_round_rect_radii_are_not_exchanged()
{
    // RemoteDrawingEngine.cpp:820-825 sends rect, xRadius, yRadius. Reading the
    // two radii as function arguments left the order to the compiler, and g++
    // evaluates right to left.
    const auto corners = [](float x_radius, float y_radius) {
        Session session(40, 40,
                        [](std::span<const std::uint8_t>) { return true; });
        Writer create(Op::create_state);
        create.i32(3);
        session.ingest(create.finish());
        Writer high(Op::set_high_color);
        high.i32(3);
        high.u8(255);
        high.u8(255);
        high.u8(255);
        high.u8(255);
        session.ingest(high.finish());
        Writer round(Op::fill_round_rect);
        round.i32(3);
        append_rect(round, {0, 0, 39, 39});
        round.f32(x_radius);
        round.f32(y_radius);
        session.ingest(round.finish());
        int top = 0;
        int left = 0;
        for (int x = 0; x < 40; ++x)
            if (session.surface().pixel(x, 0).r != 0)
                ++top;
        for (int y = 0; y < 40; ++y)
            if (session.surface().pixel(0, y).r != 0)
                ++left;
        return std::pair<int, int> {top, left};
    };
    const auto wide = corners(18, 4);   // rounded mostly in x
    const auto tall = corners(4, 18);   // rounded mostly in y
    check(wide.first == 21 && wide.second == 33,
          "xRadius 18 / yRadius 4 keeps the left edge long");
    check(tall.first == 33 && tall.second == 21,
          "xRadius 4 / yRadius 18 keeps the top edge long");
}

void test_gray1_is_msb_first_and_set_bit_is_black()
{
    // ColorConversion.cpp:556-567: shift = 7 - (index % 8), and a set bit is
    // black. The HTML5 reference client reads bit (index % 8) and maps a set
    // bit to white; it is mirrored and inverted, and is not an oracle.
    Session session(16, 16, [](std::span<const std::uint8_t>) { return true; });
    Writer create(Op::create_state);
    create.i32(4);
    session.ingest(create.finish());

    Writer bitmap(Op::draw_bitmap);
    bitmap.i32(4);
    append_rect(bitmap, {0, 0, 7, 0});
    append_rect(bitmap, {0, 0, 7, 0});
    bitmap.u32(0);
    bitmap.i32(8);
    bitmap.i32(1);
    bitmap.i32(4);
    bitmap.u32(0x0001);
    bitmap.u32(0);
    bitmap.u32(4);
    // 0b11000000: bits 7 and 6 set, so the *first two* pixels are black and
    // the remaining six are white. An LSB-first reader blackens pixels 0-5
    // instead, which is what the asymmetric fixture catches.
    bitmap.u8(0xc0);
    bitmap.u8(0);
    bitmap.u8(0);
    bitmap.u8(0);
    session.ingest(bitmap.finish());
    check(session.surface().pixel(0, 0) == Color {0, 0, 0, 255}
              && session.surface().pixel(1, 0) == Color {0, 0, 0, 255},
          "the two most significant bits are the leftmost pixels, set is black");
    check(session.surface().pixel(2, 0) == Color {255, 255, 255, 255}
              && session.surface().pixel(7, 0) == Color {255, 255, 255, 255},
          "a clear bit is white, counting down from bit 7");
}

void test_rgb32_transparent_magic_is_see_through()
{
    // B_RGB32 has no alpha channel, so BeOS/Haiku reserve 0xff777477 for
    // "transparent"; app_server rewrites it to alpha 0 before blending in every
    // mode except B_OP_COPY and B_OP_ALPHA (BitmapPainter.cpp:262-307).
    const auto draw_magic_over = [](std::uint32_t drawing_mode) {
        Session session(4, 4,
                        [](std::span<const std::uint8_t>) { return true; });
        Writer create(Op::create_state);
        create.i32(5);
        session.ingest(create.finish());
        Writer mode(Op::set_drawing_mode);
        mode.i32(5);
        mode.u32(drawing_mode);
        session.ingest(mode.finish());
        Writer background(Op::fill_rect_color);
        background.i32(5);
        append_rect(background, {0, 0, 3, 3});
        background.u8(0);
        background.u8(255);
        background.u8(0);
        background.u8(255);
        session.ingest(background.finish());

        Writer bitmap(Op::draw_bitmap);
        bitmap.i32(5);
        append_rect(bitmap, {0, 0, 0, 0});
        append_rect(bitmap, {0, 0, 0, 0});
        bitmap.u32(0);
        bitmap.i32(1);
        bitmap.i32(1);
        bitmap.i32(4);
        bitmap.u32(0x0008);
        bitmap.u32(0);
        bitmap.u32(4);
        bitmap.u8(0x77);
        bitmap.u8(0x74);
        bitmap.u8(0x77);
        bitmap.u8(0xff);
        session.ingest(bitmap.finish());
        return session.surface().pixel(0, 0);
    };
    check(draw_magic_over(1) == Color {0, 255, 0, 255},
          "under B_OP_OVER the reserved value leaves the background alone");
    check(draw_magic_over(0) == Color {119, 116, 119, 255},
          "under B_OP_COPY it is an ordinary colour");
}

void test_rect_fill_truncates_fractional_edges()
{
    // Painter::FillRect aligns both corners with _Align(round=true), i.e.
    // (int32)coord (Painter.cpp:970-978, :1648-1652), so a right edge of 5.5
    // covers through column 5 and no further.
    Surface surface(8, 8);
    DrawState state;
    state.drawing_mode = DrawingMode::copy;
    state.high = {255, 0, 0, 255};
    surface.fill_rect({2, 2, 5.5f, 5.5f}, state);
    check(surface.pixel(5, 5) == Color {255, 0, 0, 255},
          "the truncated edge pixel is filled");
    check(surface.pixel(6, 2) == Color {0, 0, 0, 255}
              && surface.pixel(2, 6) == Color {0, 0, 0, 255},
          "a fractional edge does not spill into the next column or row");
}

void test_stroke_cost_is_bounded_by_the_surface()
{
    // RemoteDrawingEngine forwards the app's own endpoints and pen size
    // unclipped, so the rasterizer has to bound its own work: a wide pen swept
    // a box sized by the wire (quadratic in the coordinates), and the 1 px
    // Bresenham walk overflowed its int error term and never terminated.
    Surface surface(64, 64);
    DrawState state;
    state.drawing_mode = DrawingMode::copy;
    state.high = {255, 0, 0, 255};
    const auto begin = std::chrono::steady_clock::now();
    state.pen_size = 2;
    surface.line({0, 0}, {40000, 40000}, state.high, &state, true);
    surface.line({-1.0e30f, 0}, {1.0e30f, 63}, state.high, &state, true);
    // 1 px, and past int range: this one used to loop forever, because
    // `2 * error` overflowed and x stopped advancing towards its target.
    state.pen_size = 1;
    surface.line({0, 0}, {3.0e9f, 1}, state.high, &state);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    check(elapsed < std::chrono::milliseconds(500),
          "off-surface stroke geometry costs surface-bounded time");
    check(surface.pixel(0, 0) == Color {255, 0, 0, 255},
          "the on-surface part of the stroke is still drawn");
}

void test_readback_covers_a_large_surface()
{
    // A fixed 64 MiB cap on RP_READ_BITMAP_RESULT threw for any surface past
    // roughly 4763x4763, and because a decode error sends no reply at all,
    // RemoteDrawingEngine::ReadBitmap then waited out its 10 s semaphore
    // timeout (RemoteDrawingEngine.cpp:1145-1152) and the screenshot failed.
    int replies = 0;
    Session session(5000, 5000, [&](std::span<const std::uint8_t> bytes) {
        if (bytes.size() >= 2
            && static_cast<Op>(bytes[0] | (bytes[1] << 8))
                == Op::read_bitmap_result) {
            ++replies;
        }
        return true;
    });
    Writer create(Op::create_state);
    create.i32(9);
    session.ingest(create.finish());
    Writer read(Op::read_bitmap);
    read.i32(9);
    append_rect(read, {0, 0, 4999, 4999});
    read.boolean(false);
    session.ingest(read.finish());
    check(replies == 1, "a 5000x5000 readback is answered");
}

void test_hostile_rects_do_not_escape_the_surface()
{
    // Every rect is unvalidated wire data. None of these may write outside the
    // surface, hang, or convert a float that is NaN or out of int range.
    Surface surface(32, 32);
    DrawState state;
    state.drawing_mode = DrawingMode::copy;
    state.high = {255, 0, 0, 255};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const std::array<Rect, 6> hostile {{
        {nan, nan, nan, nan},
        {-infinity, -infinity, infinity, infinity},
        {-1.0e30f, -1.0e30f, 1.0e30f, 1.0e30f},
        {1.0e30f, 1.0e30f, -1.0e30f, -1.0e30f},
        {0, 0, nan, 31},
        {-3.0e9f, -3.0e9f, 3.0e9f, 3.0e9f},
    }};
    for (const auto rect : hostile) {
        surface.fill_rect(rect, state);
        surface.fill_rect_color(rect, {1, 2, 3, 255}, &state);
        surface.invert_rect(rect, &state);
        surface.fill_ellipse(rect, state);
        surface.copy_rect(rect, std::numeric_limits<int>::max(),
                          std::numeric_limits<int>::min());
    }
    check(surface.pixels().size()
              == static_cast<std::size_t>(surface.width())
                  * static_cast<std::size_t>(surface.height()) * 4,
          "hostile rects leave the surface allocation intact");
    check(surface.pixel(0, 0).a == 255,
          "hostile rects keep the surface readable");
}

void test_transport_factory()
{
    std::string error;
    TransportOptions options;
    auto transport = make_transport(options, error);
    check(transport != nullptr && transport->describe() == "127.0.0.1:10900",
          "empty URL selects the classic raw TCP transport");

    options.url = "tcp://198.51.100.7:19000";
    transport = make_transport(options, error);
    check(transport != nullptr && transport->describe() == "198.51.100.7:19000",
          "tcp:// URL selects raw TCP with the URL's endpoint");

    options.url = "gopher://example/";
    check(make_transport(options, error) == nullptr && !error.empty(),
          "unsupported scheme is rejected");
    options.url = "no-scheme";
    check(make_transport(options, error) == nullptr,
          "URL without a scheme is rejected");
    options.url = "ws://:1/";
    check(make_transport(options, error) == nullptr,
          "URL without a host is rejected");

#ifdef HAIKU_REMOTE_HAVE_WSS
    options.url = "wss://broker.example/session/42?token=abc";
    transport = make_transport(options, error);
    check(transport != nullptr
              && transport->describe()
                  == "wss://broker.example:443/session/42?token=abc",
          "wss:// URL defaults to port 443 and keeps path and query");
    options.url = "ws://[::1]:8080";
    transport = make_transport(options, error);
    check(transport != nullptr && transport->describe() == "ws://::1:8080/",
          "IPv6 literal host and explicit port parse");
#endif

    TransportOptions parsed;
    const auto feed = [&](std::string_view name, std::string value) {
        return parse_transport_argument(parsed, name, [value] { return value; });
    };
    check(feed("--url", "wss://h/") && feed("--token", "tok")
              && feed("--pin-sha256", "sha256//xyz") && feed("--ca-file", "ca.pem")
              && feed("--insecure", "") && feed("--host", "h2")
              && feed("--port", "1234"),
          "transport arguments are consumed");
    check(parsed.url == "wss://h/" && parsed.token == "tok"
              && parsed.pin_sha256 == "sha256//xyz" && parsed.ca_file == "ca.pem"
              && parsed.insecure && parsed.host == "h2" && parsed.port == 1234,
          "transport arguments are stored");
    check(!feed("--width", "10"), "unrelated arguments are left to the caller");
    bool rejected = false;
    try {
        feed("--port", "70000");
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "out-of-range port is rejected");
}

#if defined(HAIKU_REMOTE_HAVE_WSS) && !defined(_WIN32)

// A minimal single-connection RFC 6455 server: accepts one client, answers the
// upgrade, records the request, echoes what protocol the client spoke.
struct WsTestServer {
    int listener = -1;
    std::uint16_t port = 0;
    std::string request;
    std::vector<std::uint8_t> client_payload;
    std::vector<std::uint8_t> auth_token;
    std::uint32_t auth_method = 0;
    std::thread thread;

    bool start()
    {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0)
            return false;
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) != 0
            || ::listen(listener, 1) != 0)
            return false;
        socklen_t length = sizeof(address);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                          &length) != 0)
            return false;
        port = ntohs(address.sin_port);
        thread = std::thread([this] { run(); });
        return true;
    }

    void run()
    {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0)
            return;

        std::string headers;
        char byte = 0;
        while (headers.find("\r\n\r\n") == std::string::npos
               && ::recv(client, &byte, 1, 0) == 1)
            headers.push_back(byte);
        request = headers;

        const std::string key_name = "Sec-WebSocket-Key: ";
        const auto key_start = headers.find(key_name);
        const auto key_end = headers.find("\r\n", key_start);
        const std::string key = headers.substr(key_start + key_name.size(),
                                               key_end - key_start - key_name.size());
        const std::string accept_source
            = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char digest[SHA_DIGEST_LENGTH];
        SHA1(reinterpret_cast<const unsigned char*>(accept_source.data()),
             accept_source.size(), digest);
        char accept[64] = {};
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(accept), digest,
                        SHA_DIGEST_LENGTH);

        std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Accept: " + std::string(accept)
            + "\r\nSec-WebSocket-Protocol: binary\r\n\r\n";
        (void)::send(client, response.data(), response.size(), 0);

        auto read_frame = [&](std::vector<std::uint8_t>& payload) -> std::uint8_t {
            std::uint8_t header[2];
            if (::recv(client, header, 2, MSG_WAITALL) != 2)
                return 0xff;
            const std::size_t length = header[1] & 0x7f;
            std::uint8_t mask[4] = {};
            if ((header[1] & 0x80) != 0
                && ::recv(client, mask, 4, MSG_WAITALL) != 4)
                return 0xff;
            payload.resize(length);
            if (length > 0
                && ::recv(client, payload.data(), length, MSG_WAITALL)
                    != static_cast<ssize_t>(length))
                return 0xff;
            for (std::size_t i = 0; i < payload.size(); ++i)
                payload[i] ^= mask[i % 4];
            return header[0] & 0x0f;
        };
        // Broker preamble: the first binary message must be RP_AUTHENTICATE
        // with the shared token; only then answer RP_AUTH_RESULT and start
        // the session traffic.
        std::vector<std::uint8_t> auth;
        if (read_frame(auth) == 0x2 && auth.size() >= 14
            && auth[0] == 10 && auth[1] == 0) {
            auth_token.assign(auth.begin() + 14, auth.end());
            auth_method = static_cast<std::uint32_t>(
                auth[6] | auth[7] << 8 | auth[8] << 16 | auth[9] << 24);
        }
        const std::uint32_t status
            = auth_token == std::vector<std::uint8_t>({'s', 'e', 'c', 'r',
                                                       'e', 't'})
            ? 0u : 1u;
        const std::uint8_t result[] = {0x82, 10, 11, 0, 10, 0, 0, 0,
                                       static_cast<std::uint8_t>(status),
                                       0, 0, 0};
        (void)::send(client, result, sizeof(result), 0);
        if (status != 0) {
            ::close(client);
            return;
        }

        // A ping the client must answer, then application bytes fragmented
        // across two frames to prove reassembly into one byte stream.
        const std::uint8_t ping[] = {0x89, 0x02, 'h', 'i'};
        (void)::send(client, ping, sizeof(ping), 0);
        const std::uint8_t first[] = {0x02, 0x03, 0x01, 0x02, 0x03};
        const std::uint8_t final_frame[] = {0x80, 0x02, 0x04, 0x05};
        (void)::send(client, first, sizeof(first), 0);
        (void)::send(client, final_frame, sizeof(final_frame), 0);

        // The client may interleave its data frame and the pong in either
        // order; collect both.
        bool pong_seen = false;
        for (int i = 0; i < 2; ++i) {
            std::vector<std::uint8_t> payload;
            const std::uint8_t opcode = read_frame(payload);
            if (opcode == 0xa && payload == std::vector<std::uint8_t> {'h', 'i'})
                pong_seen = true;
            else if (opcode == 0x2)
                client_payload = payload;
        }
        if (!pong_seen)
            client_payload.clear();
        ::close(client);
    }

    ~WsTestServer()
    {
        if (thread.joinable())
            thread.join();
        if (listener >= 0)
            ::close(listener);
    }
};

void test_websocket_roundtrip()
{
    WsTestServer server;
    check(server.start(), "test WebSocket server starts");

    TransportOptions options;
    options.url = "ws://127.0.0.1:" + std::to_string(server.port) + "/session";
    options.token = "secret";
    std::string error;
    const auto transport = make_transport(options, error);
    check(transport != nullptr, "ws:// transport is created");
    check(transport->connect(error), "WebSocket upgrade succeeds: " + error);

    const std::uint8_t outgoing[] = {0x10, 0x20, 0x30};
    check(transport->send_all(outgoing, error), "client frame is sent");

    std::vector<std::uint8_t> received;
    std::array<std::uint8_t, 16> buffer {};
    for (int i = 0; i < 100 && received.size() < 5; ++i) {
        const int count = transport->receive(buffer, 100, error);
        if (count < 0)
            break;
        received.insert(received.end(), buffer.begin(), buffer.begin() + count);
    }
    check(received == std::vector<std::uint8_t>({1, 2, 3, 4, 5}),
          "fragmented server frames reassemble into the byte stream");
    transport->close();

    check(server.request.find("GET /session HTTP/1.1") != std::string::npos,
          "the token never appears in the request target");
    check(server.auth_method == 1
              && server.auth_token == std::vector<std::uint8_t>(
                     {'s', 'e', 'c', 'r', 'e', 't'}),
          "RP_AUTHENTICATE carries method 1 and the shared token");
    check(server.request.find("Sec-WebSocket-Protocol: binary")
              != std::string::npos,
          "client offers the binary subprotocol");
    check(server.client_payload
              == std::vector<std::uint8_t>({0x10, 0x20, 0x30}),
          "client frame arrives masked and intact");

    // A wrong token is answered with RP_AUTH_RESULT status 1 (denied) and the
    // connection never opens for the session.
    WsTestServer denying;
    check(denying.start(), "second test WebSocket server starts");
    TransportOptions denied_options;
    denied_options.url = "ws://127.0.0.1:" + std::to_string(denying.port) + "/";
    denied_options.token = "wrong";
    const auto denied = make_transport(denied_options, error);
    check(denied != nullptr && !denied->connect(error)
              && error.find("denied") != std::string::npos,
          "a denied token fails connect() with a denial message");
}

#endif // HAIKU_REMOTE_HAVE_WSS && !_WIN32

} // namespace

int main()
{
    test_framer();
    test_surface_dimension_validation();
    test_inclusive_rect();
    test_pattern_phase();
    test_drawing_modes();
    test_affine_transform();
    test_stroke_width_and_pattern();
    test_copy_is_overlap_safe();
    test_text_shapes_and_rasterizes();
    test_input_messages();
    test_line_array_payload();
    test_session_rejects_unsafe_bitmap();
    test_draw_string_applies_escapement_delta();
    test_escapement_delta_distinguishes_space_from_nonspace();
    test_escapement_delta_whitespace_set_matches_haiku();
    test_draw_string_replies_when_the_delta_is_short();
    test_draw_string_with_offsets_replies();
    test_offset_text_replies_on_malformed_utf8();
    test_read_bitmap_always_replies();
    test_truncated_sync_request_still_replies();
    test_extended_renderer_opcodes();
    test_empty_clipping_region_clips_everything();
    test_round_rect_radii_are_not_exchanged();
    test_gray1_is_msb_first_and_set_bit_is_black();
    test_rgb32_transparent_magic_is_see_through();
    test_rect_fill_truncates_fractional_edges();
    test_stroke_cost_is_bounded_by_the_surface();
    test_readback_covers_a_large_surface();
    test_hostile_rects_do_not_escape_the_surface();
    test_transport_factory();
#if defined(HAIKU_REMOTE_HAVE_WSS) && !defined(_WIN32)
    test_websocket_roundtrip();
#endif
    if (failures == 0) {
        std::cout << "PASS - " << checks << " checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " checks failed\n";
    return 1;
}
