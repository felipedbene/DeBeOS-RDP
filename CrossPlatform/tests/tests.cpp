#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/protocol.hpp"
#include "haiku_remote/reconnect.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"
#include "haiku_remote/transport.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <limits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

// RP_DRAW_BITMAP's options word carries B_TILE_BITMAP_X/_Y and
// B_FILTER_BITMAP_BILINEAR (RemoteDrawingEngine.cpp:470,
// headers/os/interface/InterfaceDefs.h:306-323). It used to be read and thrown
// away, so a BView::DrawTiledBitmap arrived as one stretched copy. The
// expectations below are geometric, not read back from the renderer: a 2x2
// bitmap tiled over a 4x4 rect repeats, so column 2 restarts at source column 0,
// whereas the stretched copy this client used to draw doubles every source pixel
// and puts source column 1 there.
void append_two_by_two(Writer& writer, bool minimal,
                       const std::array<Color, 4>& pixels)
{
    writer.i32(2);
    writer.i32(2);
    writer.i32(8);
    if (!minimal) {
        writer.u32(0x0008); // B_RGB32
        writer.u32(0);
    }
    writer.u32(16);
    for (const auto pixel : pixels) {
        writer.u8(pixel.b);
        writer.u8(pixel.g);
        writer.u8(pixel.r);
        writer.u8(255);
    }
}

const std::array<Color, 4> quadrants {{
    {255, 0, 0, 255},
    {0, 255, 0, 255},
    {0, 0, 255, 255},
    {255, 255, 0, 255},
}};

void test_draw_bitmap_options_reach_the_renderer()
{
    const auto render = [](std::uint32_t options) {
        Session session(16, 16,
                        [](std::span<const std::uint8_t>) { return true; });
        Writer create(Op::create_state);
        create.i32(9);
        session.ingest(create.finish());

        Writer bitmap(Op::draw_bitmap);
        bitmap.i32(9);
        append_rect(bitmap, {0, 0, 1, 1});
        append_rect(bitmap, {0, 0, 3, 3});
        bitmap.u32(options);
        append_two_by_two(bitmap, false, quadrants);
        session.ingest(bitmap.finish());
        return session.surface();
    };
    const auto tiled = render(tile_bitmap);
    const auto stretched = render(0);
    check(tiled.pixel(0, 0) == quadrants[0] && tiled.pixel(2, 0) == quadrants[0]
              && tiled.pixel(0, 2) == quadrants[0]
              && tiled.pixel(2, 2) == quadrants[0],
          "a tiled RP_DRAW_BITMAP restarts the bitmap at every tile origin");
    check(stretched.pixel(2, 0) == quadrants[1]
              && stretched.pixel(0, 2) == quadrants[2],
          "with an empty options word the same draw is one stretched copy");
    check(tiled.pixel(2, 0) != stretched.pixel(2, 0),
          "the tiled and stretched renders differ, so the word is not ignored");
}

void test_bitmap_rects_filter_but_do_not_tile()
{
    // The tiling bits are deliberately masked off on the RECTS path and the
    // filter bit is deliberately kept. Each rect arrives as pixels the server
    // already extracted for it (RemoteDrawingEngine.cpp:405-423), and the view
    // rect the tile phase would be measured from is not on the wire at all, so
    // wrapping per destination rect would invent a phase. Filtering, by
    // contrast, is ours to do: the server only scales server-side when it
    // minifies (ibid. :1308-1310), so this magnification arrives unfiltered.
    const std::array<Color, 4> checker {{
        {0, 0, 0, 255},
        {255, 255, 255, 255},
        {255, 255, 255, 255},
        {0, 0, 0, 255},
    }};
    Session session(16, 16, [](std::span<const std::uint8_t>) { return true; });
    Writer create(Op::create_state);
    create.i32(11);
    session.ingest(create.finish());

    Writer rects(Op::draw_bitmap_rects);
    rects.i32(11);
    rects.u32(tile_bitmap | filter_bitmap_bilinear);
    rects.u32(0x0008); // B_RGB32
    rects.u32(0);
    rects.i32(1);
    append_rect(rects, {0, 0, 8, 8});
    append_two_by_two(rects, true, checker);
    session.ingest(rects.finish());

    // 126 is app_server's own half-way blend of 0 and 255 on this path
    // (DrawBitmapBilinear.h:104-124: 255-based weights, >> 16). Tiling would put
    // source column 0 at destination column 4 and leave it black; dropping the
    // filter bit would leave it white.
    check(session.surface().pixel(4, 0) == Color {126, 126, 126, 255},
          "RP_DRAW_BITMAP_RECTS honours the filter bit on a magnification");
    check(session.surface().pixel(0, 0) == Color {0, 0, 0, 255}
              && session.surface().pixel(8, 8) == Color {0, 0, 0, 255},
          "the corners of the filtered magnification are the source corners");
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
    // Both bitmap option bits take their own sampling paths, and both derive
    // source pixel indices from these same rects.
    Bitmap bitmap;
    bitmap.width = 3;
    bitmap.height = 3;
    bitmap.bgra.assign(3 * 3 * 4, 0x40);
    const std::array<std::uint32_t, 4> options {{
        0, tile_bitmap, filter_bitmap_bilinear,
        tile_bitmap | filter_bitmap_bilinear,
    }};
    for (const auto rect : hostile) {
        surface.fill_rect(rect, state);
        surface.fill_rect_color(rect, {1, 2, 3, 255}, &state);
        surface.invert_rect(rect, &state);
        surface.fill_ellipse(rect, state);
        surface.copy_rect(rect, std::numeric_limits<int>::max(),
                          std::numeric_limits<int>::min());
        for (const auto option : options) {
            surface.draw_bitmap(bitmap, rect, {0, 0, 31, 31}, state, option);
            surface.draw_bitmap(bitmap, {0, 0, 2, 2}, rect, state, option);
        }
    }
    check(surface.pixels().size()
              == static_cast<std::size_t>(surface.width())
                  * static_cast<std::size_t>(surface.height()) * 4,
          "hostile rects leave the surface allocation intact");
    check(surface.pixel(0, 0).a == 255,
          "hostile rects keep the surface readable");
}

void test_close_connection_is_an_orderly_end()
{
    // RP_CLOSE_CONNECTION was in the do-nothing arm, so nothing downstream
    // could tell an orderly server shutdown from a transport failure, and the
    // capture was discarded even though every pixel had arrived.
    // RemoteHWInterface::_Disconnect() (RemoteHWInterface.cpp:706-717) sends it
    // and then closes the endpoint; the native in-tree client quits on it
    // (RemoteView.cpp:522-526).
    Session session(16, 16, [](std::span<const std::uint8_t>) { return true; });
    check(!session.server_closed(),
          "a fresh session has not seen a server close");

    Writer create(Op::create_state);
    create.i32(4);
    session.ingest(create.finish());
    Writer fill(Op::fill_rect_color);
    fill.i32(4);
    append_rect(fill, {0, 0, 15, 15});
    fill.u8(0);
    fill.u8(255);
    fill.u8(0);
    fill.u8(255);
    session.ingest(fill.finish());
    check(!session.server_closed(),
          "ordinary drawing does not look like a close");

    Writer close(Op::close_connection);
    session.ingest(close.finish());
    check(session.server_closed(),
          "RP_CLOSE_CONNECTION is recorded as an orderly end");
    // The pixels drawn before the close are still there: the whole point is
    // that this outcome keeps the capture.
    check(session.surface().pixel(8, 8) == Color {0, 255, 0, 255},
          "pixels decoded before the close survive it");
    // It is a session-level opcode, so it must not be counted as unhandled --
    // that is what would put "1 unhandled opcodes" in the operator's log for a
    // completely normal shutdown.
    check(session.unhandled().empty(),
          "an orderly close is not reported as an unhandled opcode");
}

// Regression guard for a real, live-observed defect: the opcodes that share the
// switch with RP_CLOSE_CONNECTION must not reach its arm. A merge dropped the
// `break` ending the invalidate arm, so RP_INVALIDATE_RECT fell through and set
// the closed flag. app_server sends an invalidate inside the FIRST frame of
// every session, so against a real server the capture ended after ~150 of ~1500
// messages -- and the client blamed the server, reporting "server closed the
// connection". Unit tests passed throughout; only a live server showed it,
// because the repo's mock never sends an invalidate this early.
void test_only_close_connection_ends_the_session()
{
    const Op neighbours[] = {
        Op::invalidate_rect,
        Op::invalidate_region,
        Op::set_cursor_visible,
        Op::move_cursor_to,
        Op::enable_sync_drawing,
        Op::disable_sync_drawing,
    };
    for (const auto op : neighbours) {
        Session session(16, 16, [](std::span<const std::uint8_t>) { return true; });
        Writer message(op);
        // A payload big enough for whichever of these reads one: a BPoint is the
        // largest, and a bool ignores the extra bytes.
        message.f32(1);
        message.f32(2);
        session.ingest(message.finish());
        std::string label = "a session-level ";
        label += op_name(op);
        label += " does not end the session";
        check(!session.server_closed(), label.c_str());
    }
}

// RP_SET_CURSOR: AddCursor() is Add(hotspot) then AddBitmap()
// (RemoteMessage.cpp:190-194), and AddBitmap's non-minimal layout is width,
// height, bytesPerRow, colorSpace, flags, bitsLength, bits
// (RemoteMessage.cpp:124-145).
void append_cursor(Writer& writer, Point hotspot, int width, int height,
                   std::span<const std::uint8_t> bits)
{
    writer.point(hotspot);
    writer.i32(width);
    writer.i32(height);
    writer.i32(width * 4);
    writer.u32(0x2008); // B_RGBA32
    writer.u32(0);
    writer.u32(static_cast<std::uint32_t>(bits.size()));
    writer.raw(bits);
}

void send_cursor(Session& session, Point hotspot, int width, int height,
                 std::span<const std::uint8_t> bits)
{
    Writer writer(Op::set_cursor);
    append_cursor(writer, hotspot, width, height, bits);
    session.ingest(writer.finish());
}

void send_cursor_visible(Session& session, bool visible)
{
    Writer writer(Op::set_cursor_visible);
    writer.u8(visible ? 1 : 0);
    session.ingest(writer.finish());
}

void send_cursor_position(Session& session, float x, float y)
{
    Writer writer(Op::move_cursor_to);
    writer.f32(x);
    writer.f32(y);
    session.ingest(writer.finish());
}

bool rects_equal(IntRect a, IntRect b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right
        && a.bottom == b.bottom;
}

void test_set_cursor_decodes_hotspot_and_bitmap()
{
    Session session(32, 32, [](std::span<const std::uint8_t>) { return true; });
    check(session.cursor().generation == 0
              && session.cursor().bitmap.width == 0,
          "a fresh session holds no cursor");

    // Four distinguishable BGRA pixels, so a transposed, row-swapped or
    // alpha-dropping decode cannot pass.
    const std::array<std::uint8_t, 16> bits {
        0x10, 0x20, 0x30, 0xff, // (0,0)
        0x00, 0x00, 0xff, 0xff, // (1,0) opaque red
        0x00, 0xff, 0x00, 0xff, // (0,1) opaque green
        0x01, 0x02, 0x03, 0x00, // (1,1) fully transparent
    };
    send_cursor(session, {1, 0}, 2, 2, bits);

    const auto& cursor = session.cursor();
    check(cursor.bitmap.width == 2 && cursor.bitmap.height == 2
              && cursor.bitmap.bgra.size() == 16,
          "RP_SET_CURSOR decodes the cursor bitmap dimensions");
    check(cursor.hotspot == Point {1, 0},
          "the hotspot precedes the bitmap and keeps x before y");
    check(std::equal(bits.begin(), bits.end(), cursor.bitmap.bgra.begin(),
                     cursor.bitmap.bgra.end()),
          "a B_RGBA32 cursor keeps every byte, alpha included");
    check(cursor.generation == 1,
          "RP_SET_CURSOR bumps the shape generation a front end watches");
}

void test_set_cursor_visible_toggles_state()
{
    Session session(16, 16, [](std::span<const std::uint8_t>) { return true; });
    check(!session.cursor().visible,
          "a cursor stays hidden until the server says otherwise");
    send_cursor_visible(session, true);
    check(session.cursor().visible, "RP_SET_CURSOR_VISIBLE 1 shows the cursor");
    send_cursor_visible(session, false);
    check(!session.cursor().visible, "RP_SET_CURSOR_VISIBLE 0 hides it again");
}

void test_move_cursor_to_updates_the_position()
{
    Session session(64, 64, [](std::span<const std::uint8_t>) { return true; });
    check(session.cursor().position == Point {0, 0},
          "the cursor position starts at the origin");
    // Distinct, non-integral x and y: a swap or a truncation is visible.
    send_cursor_position(session, 37.5f, 11.25f);
    check(session.cursor().position == Point {37.5f, 11.25f},
          "RP_MOVE_CURSOR_TO reads two floats, x then y");
}

void test_cursor_composites_at_its_hotspot()
{
    Session session(16, 16, [](std::span<const std::uint8_t>) { return true; });
    session.surface().clear({0, 0, 255, 255});

    // Only the left column is drawn, and the hotspot is asymmetric, so a
    // swapped hotspot or a sign error on position - hotspot lands the pixels
    // somewhere the checks below can see.
    const std::array<std::uint8_t, 16> bits {
        0x00, 0xff, 0x00, 0xff, // (0,0) opaque green
        0x00, 0x00, 0x00, 0x00, // (1,0) transparent
        0xff, 0xff, 0xff, 0x80, // (0,1) half-transparent white
        0x00, 0x00, 0x00, 0x00, // (1,1) transparent
    };
    send_cursor(session, {1, 0}, 2, 2, bits);
    send_cursor_visible(session, true);
    send_cursor_position(session, 5, 7);

    // Exactly what main.cpp does: composite onto a copy, so the framebuffer the
    // next frame is drawn against never contains the cursor's own pixels.
    Surface composited = session.surface();
    const auto touched = composite_cursor(session.cursor(), composited);
    check(rects_equal(touched, {4, 7, 5, 8}),
          "the bitmap's top left sits at position - hotspot");
    check(composited.pixel(4, 7) == Color {0, 255, 0, 255},
          "an opaque cursor pixel replaces the framebuffer at the hotspot offset");
    check(composited.pixel(4, 8) == Color {128, 128, 255, 255},
          "a half-transparent cursor pixel blends with the framebuffer");
    check(composited.pixel(5, 7) == Color {0, 0, 255, 255}
              && composited.pixel(3, 7) == Color {0, 0, 255, 255}
              && composited.pixel(4, 6) == Color {0, 0, 255, 255},
          "a transparent cursor pixel and everything outside the cursor are left alone");
}

void test_an_invisible_cursor_composites_nothing()
{
    Session session(16, 16, [](std::span<const std::uint8_t>) { return true; });
    session.surface().clear({0, 0, 255, 255});
    const std::array<std::uint8_t, 4> bits {0x00, 0xff, 0x00, 0xff};
    send_cursor(session, {0, 0}, 1, 1, bits);
    send_cursor_position(session, 5, 7);

    Surface composited = session.surface();
    check(rects_equal(composite_cursor(session.cursor(), composited), {}),
          "a cursor the server has not shown yet composites nothing");
    check(composited.pixel(5, 7) == Color {0, 0, 255, 255},
          "and leaves the framebuffer byte-identical");

    send_cursor_visible(session, true);
    check(rects_equal(composite_cursor(session.cursor(), composited),
                      {5, 7, 5, 7}),
          "the same cursor draws once the server shows it");
    check(composited.pixel(5, 7) == Color {0, 255, 0, 255},
          "which is what makes the hidden case a real check");
}

void test_cursor_is_clipped_to_the_surface()
{
    Session session(8, 8, [](std::span<const std::uint8_t>) { return true; });
    session.surface().clear({0, 0, 0, 255});
    const std::array<std::uint8_t, 16> bits {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    send_cursor(session, {0, 0}, 2, 2, bits);
    send_cursor_visible(session, true);
    send_cursor_position(session, 7, 7);

    Surface composited = session.surface();
    check(rects_equal(composite_cursor(session.cursor(), composited),
                      {7, 7, 7, 7}),
          "a cursor straddling the edge is clipped to the surface");
    check(composited.pixel(7, 7) == Color {255, 255, 255, 255},
          "and its on-surface pixel is still drawn");

    send_cursor_position(session, 100, 100);
    check(rects_equal(composite_cursor(session.cursor(), composited), {}),
          "a cursor entirely off the surface composites nothing");
}

void test_a_malformed_cursor_keeps_the_last_good_one()
{
    int errors = 0;
    Session session(16, 16, [](std::span<const std::uint8_t>) { return true; },
                    [&](std::string_view) { ++errors; });
    const std::array<std::uint8_t, 16> good {
        0x10, 0x20, 0x30, 0xff, 0x00, 0x00, 0xff, 0xff,
        0x00, 0xff, 0x00, 0xff, 0x01, 0x02, 0x03, 0x00,
    };
    send_cursor(session, {1, 0}, 2, 2, good);

    // Zero-sized, absurdly large, and a bitsLength that disagrees with
    // height * bytesPerRow. read_bitmap() rejects all three.
    send_cursor(session, {0, 0}, 0, 0, std::span<const std::uint8_t> {});
    send_cursor(session, {0, 0}, 100000, 100000, good);
    send_cursor(session, {0, 0}, 2, 2, std::span(good).first(8));

    check(errors == 3, "each malformed cursor is reported once");
    const auto& cursor = session.cursor();
    check(cursor.generation == 1 && cursor.bitmap.width == 2
              && cursor.hotspot == Point {1, 0},
          "a malformed cursor does not replace the last good one");

    // The session must still be decoding, not wedged on the bad payloads.
    send_cursor_position(session, 3, 4);
    check(session.cursor().position == Point {3, 4},
          "and the session keeps handling later cursor messages");
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
              && feed("--port", "1234") && feed("--cookie", "c00kie"),
          "transport arguments are consumed");
    check(parsed.url == "wss://h/" && parsed.token == "tok"
              && parsed.pin_sha256 == "sha256//xyz" && parsed.ca_file == "ca.pem"
              && parsed.insecure && parsed.host == "h2" && parsed.port == 1234
              && parsed.cookie == "c00kie",
          "transport arguments are stored");
    check(!feed("--width", "10"), "unrelated arguments are left to the caller");
    bool rejected = false;
    try {
        feed("--port", "70000");
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "out-of-range port is rejected");

    // A cookie longer than the wire allows is refused where it is given, not
    // where the gate would drop it.
    rejected = false;
    try {
        feed("--cookie", std::string(session_cookie_max_length + 1, 'a'));
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "an over-long cookie is rejected at the argument");

    // --cookie-file is the form to prefer, and app_server writes the cookie
    // with a trailing newline: a cookie that keeps it matches nothing.
    // Written beside wherever the test binary runs, so this does not depend on
    // the working directory the build system happens to use.
    const std::string cookie_path = "haiku-remote-test-session-cookie.tmp";
    {
        std::ofstream file(cookie_path, std::ios::binary);
        file << std::string(64, 'a') << "\n";
    }
    TransportOptions from_file;
    bool read_cookie = false;
    try {
        read_cookie = parse_transport_argument(from_file, "--cookie-file",
                                              [&] { return cookie_path; });
    } catch (const std::exception&) {
        read_cookie = false;
    }
    check(read_cookie && from_file.cookie == std::string(64, 'a'),
          "--cookie-file reads the cookie and strips the trailing newline");
    std::remove(cookie_path.c_str());

    // And a file that is not there is an error, not an empty cookie that would
    // be refused later with a different complaint.
    bool missing_rejected = false;
    try {
        (void)parse_transport_argument(from_file, "--cookie-file",
                                       [&] { return cookie_path; });
    } catch (const std::exception&) {
        missing_rejected = true;
    }
    check(missing_rejected, "an unreadable --cookie-file is reported");

    // The cookie and the broker token belong to two different hops, and exactly
    // one of them is this client's to send. A cookie on a broker URL is refused
    // rather than silently dropped.
    TransportOptions broker;
    broker.url = "wss://broker.example/";
    broker.token = "tok";
    broker.cookie = std::string(64, 'a');
    error.clear();
    check(make_transport(broker, error) == nullptr
              && error.find("broker") != std::string::npos,
          "a cookie on a ws/wss URL is refused, naming the broker");
}

// ---------------------------------------------------------------------------
// The opening frames of a session (src/transport.cpp, src/session.cpp).
//
// app_server's candidate gate (NetReceiver::_ReceiveCandidateData) reads the
// first six bytes of a fresh connection and requires RP_SESSION_COOKIE; a
// connection that opens with a bare RP_INIT_CONNECTION is dropped, by name, and
// never becomes the session. So the ORDER of the first frames is a correctness
// property of this client, and these tests pin it against hand-written golden
// bytes -- not against the Writer that produces them, which would agree with
// itself no matter which opcode or method it spelled.
// ---------------------------------------------------------------------------

// Frame codes of `stream`, in order, or an empty vector when it is not a
// well-formed sequence of whole frames.
std::vector<std::uint16_t> frame_codes(const std::vector<std::uint8_t>& stream)
{
    std::vector<std::uint16_t> codes;
    std::size_t offset = 0;
    while (offset + message_header_size <= stream.size()) {
        const auto code = static_cast<std::uint16_t>(
            stream[offset] | stream[offset + 1] << 8);
        std::uint32_t total = 0;
        for (int i = 3; i >= 0; --i)
            total = (total << 8) | stream[offset + 2 + static_cast<std::size_t>(i)];
        if (total < message_header_size || offset + total > stream.size())
            return {};
        codes.push_back(code);
        offset += total;
    }
    return offset == stream.size() ? codes : std::vector<std::uint16_t> {};
}

// app_server's gate decision, transcribed from
// NetReceiver::_ReceiveCandidateData() at the offsets it reads: the six byte
// header, the method at +6, the cookie length at +10 and the cookie at +14. Two
// implementations checked against each other, rather than each against its
// author's memory -- a disagreement here is every direct connection refused on
// hardware, which is the expensive place to find out.
std::string gate_verdict(const std::vector<std::uint8_t>& frame,
                         const std::string& expected_cookie)
{
    constexpr std::size_t header = 6;
    constexpr std::size_t body = 8; // method + cookie length
    if (frame.size() < header)
        return "short header";
    const auto u32_at = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (int i = 3; i >= 0; --i)
            value = (value << 8) | frame[offset + static_cast<std::size_t>(i)];
        return value;
    };
    const auto code = static_cast<std::uint16_t>(frame[0] | frame[1] << 8);
    const std::uint32_t length = u32_at(2);
    if (code != static_cast<std::uint16_t>(Op::session_cookie))
        return "first frame is not a session cookie";
    if (length < header + body
        || length > header + body + session_cookie_max_length)
        return "implausible length";
    if (frame.size() < length)
        return "incomplete frame";
    if (u32_at(header) != cookie_method_per_boot)
        return "malformed session cookie";
    const std::uint32_t cookie_length = u32_at(header + 4);
    if (header + body + cookie_length != length)
        return "malformed session cookie";
    const std::string got(frame.begin() + header + body,
                          frame.begin() + header + body + cookie_length);
    return got == expected_cookie ? "accepted" : "wrong session cookie";
}

// RP_INIT_CONNECTION then RP_HELLO for a 64x48 surface, spelled out byte by
// byte. The RP_HELLO body is {protocol version, capability bitmap, max decode
// width, max decode height, width, height}, and the bitmap is pinned here
// deliberately: the shipping arm64 server has zstd compiled in, so advertising
// RP_CAP_COMPRESS_ZSTD (1 << 1) without a decoder would make it switch to
// compressed segments after the ack and every byte after that would trip
// Framer's size guard. Any future edit to the bitmap has to edit this vector.
const std::vector<std::uint8_t> golden_session_opening = {
    1, 0, 6, 0, 0, 0,                                  // RP_INIT_CONNECTION
    6, 0, 30, 0, 0, 0,                                 // RP_HELLO, 6 + 24 bytes
    1, 0, 0, 0,                                        // protocol version 1
    5, 0, 0, 0,                                        // RP_CAP_STRING_WIDTH_REPLY | RP_CAP_RESYNC
    0, 0, 0, 0,                                        // max decode width
    0, 0, 0, 0,                                        // max decode height
    64, 0, 0, 0,                                       // requested width
    48, 0, 0, 0,                                       // requested height
};

void test_session_start_opens_with_init_then_hello()
{
    std::vector<std::uint8_t> stream;
    Session session(64, 48, [&](std::span<const std::uint8_t> bytes) {
        stream.insert(stream.end(), bytes.begin(), bytes.end());
        return true;
    });
    session.start();

    check(stream == golden_session_opening,
          "Session::start() sends exactly RP_INIT_CONNECTION then RP_HELLO,"
          " advertising RP_CAP_STRING_WIDTH_REPLY | RP_CAP_RESYNC (0x5)");
    check(frame_codes(stream)
              == std::vector<std::uint16_t> {
                  static_cast<std::uint16_t>(Op::init_connection),
                  static_cast<std::uint16_t>(Op::hello)},
          "the session stream itself carries no cookie frame -- on ws/wss the"
          " broker presents its own");
}

#ifndef _WIN32

// A listening loopback socket on an ephemeral port, for driving a real
// TcpTransport with no app_server in the loop.
struct LoopbackListener {
    int listener = -1;
    std::uint16_t port = 0;

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
        return true;
    }

    ~LoopbackListener()
    {
        if (listener >= 0)
            ::close(listener);
    }
};

void test_direct_transport_presents_the_cookie_before_anything_else()
{
    const std::string cookie(64, 'a');

    LoopbackListener server;
    check(server.start(), "test listener starts");

    TransportOptions options;
    options.host = "127.0.0.1";
    options.port = server.port;
    options.cookie = cookie;
    std::string error;
    const auto transport = make_transport(options, error);
    check(transport != nullptr && transport->connect(error),
          "a direct transport with a cookie connects: " + error);

    const int accepted = ::accept(server.listener, nullptr, nullptr);
    check(accepted >= 0, "the connection is accepted");

    Session session(64, 48, [&](std::span<const std::uint8_t> bytes) {
        std::string send_error;
        return transport->send_all(bytes, send_error);
    });
    session.start();
    transport->close();

    std::vector<std::uint8_t> stream;
    if (accepted >= 0) {
        std::array<std::uint8_t, 1024> buffer {};
        for (;;) {
            const ssize_t count = ::recv(accepted, buffer.data(), buffer.size(), 0);
            if (count <= 0)
                break;
            stream.insert(stream.end(), buffer.begin(),
                          buffer.begin() + count);
        }
        ::close(accepted);
    }

    // The cookie frame, byte for byte as the gate reads it: code 12, total
    // length 6 + 8 + 64, method 1, cookie length 64, then the cookie.
    std::vector<std::uint8_t> golden_cookie_frame = {
        12, 0, 78, 0, 0, 0, 1, 0, 0, 0, 64, 0, 0, 0,
    };
    golden_cookie_frame.insert(golden_cookie_frame.end(), cookie.begin(),
                               cookie.end());

    const bool cookie_first = stream.size() >= golden_cookie_frame.size()
        && std::equal(golden_cookie_frame.begin(), golden_cookie_frame.end(),
                      stream.begin());
    check(cookie_first,
          "a direct connection opens with the RP_SESSION_COOKIE frame, byte"
          " for byte");
    check(gate_verdict(golden_cookie_frame, cookie) == "accepted",
          "and app_server's own gate decision accepts that frame");
    check(gate_verdict(golden_cookie_frame, std::string(64, 'b'))
              == "wrong session cookie",
          "the gate refuses a wrong cookie of the same length");
    check(gate_verdict(golden_session_opening, cookie)
              == "first frame is not a session cookie",
          "and refuses the pre-cookie opening this client used to send");

    // Everything after the cookie frame is the session stream, unchanged: the
    // gate consumes the cookie and never forwards it, so RP_INIT_CONNECTION is
    // still the first frame app_server's parser sees.
    std::vector<std::uint8_t> after;
    if (cookie_first) {
        after.assign(stream.begin()
                         + static_cast<std::ptrdiff_t>(
                             golden_cookie_frame.size()),
                     stream.end());
    }
    check(after == golden_session_opening,
          "the session stream follows the cookie frame, RP_INIT_CONNECTION"
          " first and RP_HELLO second");
}

void test_direct_transport_refuses_a_connection_with_no_cookie()
{
    LoopbackListener server;
    check(server.start(), "second test listener starts");

    TransportOptions options;
    options.host = "127.0.0.1";
    options.port = server.port;
    std::string error;
    const auto transport = make_transport(options, error);
    check(transport != nullptr, "a cookie-less direct transport is created");
    check(transport != nullptr && !transport->connect(error),
          "connect() refuses a direct connection with no cookie");
    check(error.find("session_cookie.") != std::string::npos,
          "and names the file to read");
    check(error.find("--cookie-file") != std::string::npos,
          "and the option that reads it");
    // The two refusals a harness has to tell apart. This one is local: no
    // credential was supplied and no socket was opened, so it carries its own
    // exit code rather than the generic failure.
    check(transport != nullptr
              && transport->connect_failure()
                  == ConnectFailure::missing_credential,
          "a missing cookie is reported as a missing credential");
    check(transport != nullptr
              && connect_exit_status(*transport) == exit_status::no_credential
              && exit_status::no_credential != exit_status::failed
              && exit_status::no_credential != exit_status::usage,
          "which exits with a code of its own, distinct from failure and usage");

    // Refused before the socket is opened: nothing was accepted.
    const auto refused_cookie = std::string(session_cookie_max_length + 1, 'a');
    TransportOptions too_long = options;
    too_long.cookie = refused_cookie;
    const auto long_transport = make_transport(too_long, error);
    check(long_transport != nullptr && !long_transport->connect(error)
              && error.find("longer than") != std::string::npos,
          "an over-long cookie is refused rather than truncated onto the wire");
    // A cookie that was supplied and is unusable is NOT the missing-credential
    // case: the exit code must not widen to mean "something about a cookie".
    check(long_transport != nullptr
              && long_transport->connect_failure() == ConnectFailure::other
              && connect_exit_status(*long_transport) == exit_status::failed,
          "a cookie that was supplied keeps the generic failure exit code");
}

// The refusal above has exactly one actionable word in it -- a filename -- and
// app_server names that file after the port *it* listens on. The port this client
// dialled is the same number only when nothing forwards it, and a loopback-bound
// listener is normally reached through a forward whose local port was picked from
// whatever was free. Naming the file after the dialled port therefore sent a
// first-time reader to a path that exists nowhere, and the natural conclusion was
// that the server had published no cookie (issue #20: session_cookie.19900
// reported for a listener on 10900).
//
// The numbers below are deliberately different from each other and neither is
// the transport's default, so no arm of this test can pass by coincidence, and
// the expected text is spelled out rather than built from the code under test.
void test_no_cookie_hint_names_the_listener_port_not_the_local_one()
{
    constexpr std::uint16_t local_forward_port = 19900; // this end of a tunnel
    constexpr std::uint16_t listener_port = 10900;      // app_server's own

    TransportOptions tunnelled;
    tunnelled.host = "127.0.0.1";
    tunnelled.port = local_forward_port;
    std::string error;
    const auto through_a_tunnel = make_transport(tunnelled, error);
    check(through_a_tunnel != nullptr && !through_a_tunnel->connect(error),
          "a cookie-less connection to a forwarded port is refused");
    check(error.find("session_cookie." + std::to_string(local_forward_port))
              == std::string::npos,
          "the refusal does not name the cookie file after the local port");
    check(error.find(std::to_string(local_forward_port)) == std::string::npos,
          "and does not put the local port in the message at all, so it cannot"
          " be read as part of the path");
    check(error.find("session_cookie." + std::to_string(listener_port))
              != std::string::npos,
          "it gives the shape of the name, with the default listener as the"
          " example");
    check(error.find("session_cookie.<") != std::string::npos,
          "and says the number is the listener's, not a literal");
    check(error.find("tunnel") != std::string::npos,
          "and says why the port used here is probably not that number");
    check(error.find("haiku-remote-desktop") != std::string::npos,
          "and points at the tooling that fetches the cookie for you");

    // A destination that is not this machine: the port dialled *is* the port
    // something listens on over there, so the exact path can be given. No
    // socket is opened for this -- the cookie is checked first -- so the
    // unroutable documentation address is never contacted.
    TransportOptions direct;
    direct.host = "198.51.100.7";
    direct.port = local_forward_port;
    const auto to_a_server = make_transport(direct, error);
    check(to_a_server != nullptr && !to_a_server->connect(error),
          "a cookie-less connection to a remote host is refused too");
    check(error.find("session_cookie." + std::to_string(local_forward_port))
              != std::string::npos,
          "and there the port addressed is the listener's, so the refusal names"
          " the file exactly");
}

#endif // !_WIN32

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
    // When non-negative, the RP_AUTH_RESULT status to answer with regardless of
    // the token -- the broker's failures that are not about the token at all.
    int forced_status = -1;
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
        const std::uint32_t status = forced_status >= 0
            ? static_cast<std::uint32_t>(forced_status)
            : (auth_token == std::vector<std::uint8_t>({'s', 'e', 'c', 'r',
                                                        'e', 't'})
                   ? 0u : 1u);
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

    // Join the server thread so its writes to request/client_payload/auth_*
    // are visible to the main thread before it asserts on them. run() reads a
    // fixed number of frames and closes its client, so it always terminates on
    // its own; the caller must transport->close() first only to unblock a frame
    // the client never sent. Without this happens-before edge the assertions
    // race the server thread and client_payload loses ~1 run in 10 (#7).
    void join()
    {
        if (thread.joinable())
            thread.join();
    }

    ~WsTestServer()
    {
        join();
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
    // Synchronize with the server thread before reading any of its members: the
    // close() above unblocks a frame the client never sent, join() then
    // establishes the happens-before edge the assertions below rely on (#7).
    server.join();

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

    // The broker's other refusals are not about the token, and saying "status 2"
    // or "status 3" sends the operator to look at the one thing that is fine.
    // Status 3 (kAuthResultNoCookie) in particular means the token was ACCEPTED
    // and the broker could not read app_server's session cookie -- a remedy
    // entirely on the server.
    const auto expect_status = [&](int status, std::string_view needle,
                                   std::string_view forbidden,
                                   std::string_view message) {
        WsTestServer broker;
        broker.forced_status = status;
        if (!broker.start()) {
            check(false, "test WebSocket server starts");
            return;
        }
        TransportOptions options_for_status;
        options_for_status.url
            = "ws://127.0.0.1:" + std::to_string(broker.port) + "/";
        options_for_status.token = "secret";
        std::string status_error;
        const auto transport_for_status
            = make_transport(options_for_status, status_error);
        const bool refused = transport_for_status != nullptr
            && !transport_for_status->connect(status_error);
        check(refused
                  && status_error.find(needle) != std::string::npos
                  && (forbidden.empty()
                      || status_error.find(forbidden) == std::string::npos),
              std::string(message) + " (got: " + status_error + ")");
    };
    expect_status(2, "no session to attach", "",
                  "status 2 is reported as the session being unreachable");
    expect_status(3, "session cookie", "denied",
                  "status 3 blames the broker's unreadable session cookie, not"
                  " the token");
    expect_status(3, "remedy is on the server", "",
                  "status 3 says where the fix belongs");
    // A code this client has never heard of still has to be reported, with its
    // number, rather than swallowed.
    expect_status(77, "status 77", "",
                  "an unknown authentication status keeps its number");
}

#endif // HAIKU_REMOTE_HAVE_WSS && !_WIN32

// ---------------------------------------------------------------------------
// Opcode naming and frame-length validation (src/protocol.cpp).
// ---------------------------------------------------------------------------

bool protocol_text_contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

void set_declared_frame_size(std::vector<std::uint8_t>& frame,
                             std::uint32_t size)
{
    for (std::size_t i = 0; i < 4; ++i)
        frame[2 + i] = static_cast<std::uint8_t>(size >> (i * 8));
}

// Feeds `frame` to a fresh Framer and returns the diagnostic it died with, or
// an empty string if it did not throw.
std::string framing_failure(const std::vector<std::uint8_t>& frame)
{
    Framer framer;
    try {
        (void)framer.feed(frame);
    } catch (const ProtocolError& error) {
        return error.what();
    }
    return {};
}

void test_op_names_cover_the_whole_protocol()
{
    // A sample from every range op_name() used to answer "RP_UNKNOWN" for: the
    // RP_SET_* state block, the clipping/copy block, the gradient strokes and
    // fills, the cursor ops, and the synchronous *_RESULT replies.
    check(op_name(Op::set_font) == "RP_SET_FONT", "RP_SET_FONT is named");
    check(op_name(Op::set_transform) == "RP_SET_TRANSFORM",
          "RP_SET_TRANSFORM is named");
    check(op_name(Op::constrain_clipping_region)
              == "RP_CONSTRAIN_CLIPPING_REGION",
          "RP_CONSTRAIN_CLIPPING_REGION is named");
    check(op_name(Op::copy_rect_no_clipping) == "RP_COPY_RECT_NO_CLIPPING",
          "RP_COPY_RECT_NO_CLIPPING is named");
    check(op_name(Op::draw_bitmap_rects) == "RP_DRAW_BITMAP_RECTS",
          "RP_DRAW_BITMAP_RECTS is named");
    check(op_name(Op::stroke_line_gradient) == "RP_STROKE_LINE_GRADIENT",
          "RP_STROKE_LINE_GRADIENT is named");
    check(op_name(Op::fill_polygon_gradient) == "RP_FILL_POLYGON_GRADIENT",
          "RP_FILL_POLYGON_GRADIENT is named");
    check(op_name(Op::set_cursor_visible) == "RP_SET_CURSOR_VISIBLE",
          "RP_SET_CURSOR_VISIBLE is named");
    check(op_name(Op::move_cursor_to) == "RP_MOVE_CURSOR_TO",
          "RP_MOVE_CURSOR_TO is named");
    check(op_name(Op::string_width_result) == "RP_STRING_WIDTH_RESULT",
          "RP_STRING_WIDTH_RESULT is named");
    check(op_name(Op::read_bitmap_result) == "RP_READ_BITMAP_RESULT",
          "RP_READ_BITMAP_RESULT is named");
    check(op_name(Op::draw_string_result) == "RP_DRAW_STRING_RESULT",
          "RP_DRAW_STRING_RESULT is named");
    // The names that were already right must stay right.
    check(op_name(Op::fill_rect) == "RP_FILL_RECT", "RP_FILL_RECT is named");

    // An opcode the protocol does not define is still worth logging, but only
    // with its value: "RP_UNKNOWN" alone identifies nothing.
    check(op_name(static_cast<Op>(0x1234)) == "RP_UNKNOWN(4660)",
          "an undefined opcode is reported with its numeric value");
    check(protocol_text_contains(op_name(static_cast<Op>(0)), "0"),
          "opcode 0 is reported with its numeric value");
}

void test_op_names_do_not_drift_from_the_op_enum()
{
    // The drift guard. Op and op_name() are generated from one table, and
    // op_name()'s switch has no default arm so -Wswitch catches an enumerator
    // added by hand; this is the runtime half of the same invariant.
    std::size_t unnamed = 0;
    std::size_t misprefixed = 0;
    std::vector<std::string> names;
    std::vector<std::uint16_t> values;
    for (const Op op : all_ops()) {
        const auto name = op_name(op);
        if (name.empty() || protocol_text_contains(name, "RP_UNKNOWN"))
            ++unnamed;
        if (!name.starts_with("RP_"))
            ++misprefixed;
        names.push_back(name);
        values.push_back(static_cast<std::uint16_t>(op));
    }
    check(all_ops().size() >= 96,
          "the opcode table covers the server's whole opcode enum");
    check(unnamed == 0, "every opcode the client defines has a wire name");
    check(misprefixed == 0, "every wire name carries the RP_ prefix");

    // A copy-pasted table row is the other way these two lists go wrong.
    std::size_t duplicate_names = 0;
    std::size_t duplicate_values = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            if (names[i] == names[j])
                ++duplicate_names;
            if (values[i] == values[j])
                ++duplicate_values;
        }
    }
    check(duplicate_names == 0, "no two opcodes share a wire name");
    check(duplicate_values == 0, "no two opcodes share a value");
}

void test_framer_rejects_a_declared_length_below_the_header()
{
    Writer writer(Op::update_display_mode);
    writer.i32(1280);
    writer.i32(800);
    auto frame = writer.finish();
    set_declared_frame_size(frame, 5);
    const auto failure = framing_failure(frame);
    check(protocol_text_contains(failure, "framing desync in "
                                          "RP_UPDATE_DISPLAY_MODE"),
          "a short declared length is reported against its opcode by name");
    check(protocol_text_contains(failure, "declared frame size 5")
              && protocol_text_contains(failure,
                                        "smaller than the 6 byte frame header"),
          "a short declared length is reported with the size and the bound");
    check(protocol_text_contains(failure, "at stream offset 0"),
          "a short declared length is reported with its stream offset");
}

void test_framer_rejects_an_absurd_declared_length()
{
    Writer writer(Op::fill_polygon);
    writer.i32(7);
    auto frame = writer.finish();
    set_declared_frame_size(frame, 0xffffffffu);
    const auto failure = framing_failure(frame);
    check(protocol_text_contains(failure, "framing desync in RP_FILL_POLYGON"),
          "an oversized declared length is reported against its opcode");
    check(protocol_text_contains(failure, "declared frame size 4294967295")
              && protocol_text_contains(failure,
                                        "beyond the 67108864 byte limit"),
          "an oversized declared length is reported with the size and the bound");
}

void test_framer_holds_a_payload_truncated_mid_frame()
{
    // A frame split across two reads is the normal TCP case, not an error: it
    // must be held, not reported and not lost.
    Writer writer(Op::invalidate_rect);
    writer.i32(4);
    writer.f32(1);
    writer.f32(2);
    writer.f32(3);
    writer.f32(4);
    const auto frame = writer.finish();
    Framer framer;
    const auto prefix = frame.size() - 4;
    check(framer.feed(std::span(frame).first(prefix)).empty(),
          "a payload truncated mid-frame produces no message");
    check(!framer.failed() && framer.pending_bytes() == prefix,
          "a payload truncated mid-frame is held, not discarded");
    const auto messages = framer.feed(std::span(frame).subspan(prefix));
    check(messages.size() == 1 && messages.front().op == Op::invalidate_rect
              && messages.front().payload.size() == frame.size() - 6,
          "the held payload completes the frame when the rest arrives");
    check(framer.pending_bytes() == 0 && framer.stream_offset() == frame.size(),
          "a completed frame advances the stream offset and empties the buffer");
}

void test_framer_latches_a_framing_failure()
{
    Writer good(Op::update_display_mode);
    good.i32(640);
    good.i32(480);
    const auto good_frame = good.finish();
    Writer bad(Op::fill_rect);
    auto bad_frame = bad.finish();
    set_declared_frame_size(bad_frame, 1);

    std::vector<std::uint8_t> stream(good_frame.begin(), good_frame.end());
    stream.insert(stream.end(), bad_frame.begin(), bad_frame.end());

    Framer framer;
    std::string first;
    try {
        (void)framer.feed(stream);
        check(false, "a bad declared length after a good frame is rejected");
    } catch (const ProtocolError& error) {
        first = error.what();
        check(true, "a bad declared length after a good frame is rejected");
    }
    check(protocol_text_contains(
              first, "at stream offset " + std::to_string(good_frame.size())),
          "the diagnostic names the stream offset of the bad frame, not zero");
    check(framer.failed() && framer.pending_bytes() == 0,
          "a framing failure latches and releases the buffered bytes");

    // No fake recovery: a perfectly good frame after the desync is refused with
    // the same diagnostic, because its position in the stream is unknowable.
    std::string second;
    try {
        (void)framer.feed(good_frame);
    } catch (const ProtocolError& error) {
        second = error.what();
    }
    check(second == first,
          "later reads repeat the original diagnostic instead of resyncing");
    check(framer.pending_bytes() == 0,
          "a latched framer buffers nothing more from a hostile peer");
}

// ---------------------------------------------------------------------------
// Reconnect (issue #24). The policy the maintainer approved: reconnect is
// opt-in, bounded with backoff, scoped to a transport failure, and NEVER
// attempted after RP_CLOSE_CONNECTION or after an eviction. These exercise the
// pure policy and classification directly; the socket test below proves the
// classification is wired to the transport's own reset/FIN signals.
// ---------------------------------------------------------------------------

void test_reconnect_is_opt_in_and_off_by_default()
{
    // The default config does not reconnect: a build or a front end that sets
    // nothing behaves exactly as before -- one connection, then done.
    ReconnectPolicy off {ReconnectConfig {}};
    check(!off.config().enabled, "reconnect is disabled by default");
    check(!off.should_retry(ConnectionOutcome::transport_dropped, 0),
          "a default policy does not retry even a plain transport drop");

    // Turning it on is a single explicit flag.
    ReconnectConfig on;
    on.enabled = true;
    check(ReconnectPolicy {on}.should_retry(ConnectionOutcome::transport_dropped, 0),
          "an explicitly enabled policy retries a transport drop");
}

void test_reconnect_is_bounded()
{
    ReconnectConfig config;
    config.enabled = true;
    config.max_attempts = 3;
    ReconnectPolicy policy {config};

    check(policy.should_retry(ConnectionOutcome::transport_dropped, 0),
          "first reconnect is allowed");
    check(policy.should_retry(ConnectionOutcome::transport_dropped, 2),
          "the third reconnect is allowed");
    check(!policy.should_retry(ConnectionOutcome::transport_dropped, 3),
          "the fourth gives up: bounded, does not spin");
    check(!policy.should_retry(ConnectionOutcome::transport_dropped, 99),
          "and stays given up past the bound");

    // A zero bound is a legal way to keep the machinery present but retry never.
    ReconnectConfig none = config;
    none.max_attempts = 0;
    check(!ReconnectPolicy {none}.should_retry(ConnectionOutcome::transport_dropped, 0),
          "max_attempts 0 never retries");
}

void test_reconnect_backoff_is_exponential_and_capped()
{
    ReconnectConfig config;
    config.enabled = true;
    config.base_backoff = std::chrono::milliseconds(100);
    config.max_backoff = std::chrono::milliseconds(1000);
    ReconnectPolicy policy {config};

    check(policy.backoff_for(0) == std::chrono::milliseconds(100),
          "first backoff is the base");
    check(policy.backoff_for(1) == std::chrono::milliseconds(200),
          "backoff doubles");
    check(policy.backoff_for(2) == std::chrono::milliseconds(400),
          "and doubles again");
    check(policy.backoff_for(3) == std::chrono::milliseconds(800),
          "and again");
    check(policy.backoff_for(4) == std::chrono::milliseconds(1000),
          "then saturates at the cap rather than overshooting");
    check(policy.backoff_for(40) == std::chrono::milliseconds(1000),
          "a large attempt count cannot overflow past the cap");
}

void test_classify_connection_reads_more_than_socket_closed()
{
    // connect() never succeeded.
    ConnectionResult missing;
    missing.connected = false;
    missing.connect_failure = ConnectFailure::missing_credential;
    check(classify_connection(missing) == ConnectionOutcome::missing_credential,
          "a missing credential is its own outcome, never retried");

    ConnectionResult unreachable;
    unreachable.connected = false;
    unreachable.connect_failure = ConnectFailure::other;
    check(classify_connection(unreachable) == ConnectionOutcome::connect_failed,
          "a listener that is not up is a retriable connect failure");

    // A clean end at the deadline is not a drop.
    ConnectionResult done;
    done.connected = true;
    done.message_count = 500;
    done.reached_deadline = true;
    check(classify_connection(done) == ConnectionOutcome::completed,
          "reaching the capture deadline is a completed session, not a drop");

    // Connected, clean FIN, but nothing decoded: refused at the gate.
    ConnectionResult refused;
    refused.connected = true;
    refused.peer_closed = true;
    refused.message_count = 0;
    check(classify_connection(refused) == ConnectionOutcome::refused,
          "a clean close with no messages is a gate refusal, not a drop");

    // The two that share a closed socket but must part ways: a mid-session FIN
    // (a tunnel torn down) is a retriable drop; a reset (RST) is the server
    // refusing/evicting us and must not be retried.
    ConnectionResult drop;
    drop.connected = true;
    drop.message_count = 500;
    drop.peer_closed = true;
    check(classify_connection(drop) == ConnectionOutcome::transport_dropped,
          "a mid-session clean close with a live session is a transport drop");

    ConnectionResult reset;
    reset.connected = true;
    reset.message_count = 500;
    reset.connection_reset = true;
    check(classify_connection(reset) == ConnectionOutcome::evicted,
          "a reset -- pipelined bytes unread -- is an eviction, not a drop");
}

void test_no_retry_after_eviction()
{
    // THE load-bearing arm. The candidate gate is newest-wins, so an
    // auto-reconnecting evicted client would evict its successor and be evicted
    // back, forever. An eviction must be final. Even fully enabled with attempts
    // to spare, the policy must refuse to retry it.
    ConnectionResult reset;
    reset.connected = true;
    reset.message_count = 1000; // a full, healthy session preceded the reset
    reset.connection_reset = true;
    const auto outcome = classify_connection(reset);
    check(outcome == ConnectionOutcome::evicted,
          "a peer reset after a healthy session classifies as an eviction");

    ReconnectConfig config;
    config.enabled = true;
    config.max_attempts = 10;
    ReconnectPolicy policy {config};
    check(!policy.should_retry(outcome, 0),
          "an evicted client does NOT reconnect, even with attempts to spare");
    check(!outcome_is_retriable(outcome),
          "eviction is not a retriable outcome at all");
}

void test_no_retry_after_close_connection()
{
    // RP_CLOSE_CONNECTION is the server saying "go away" in band. It wins even
    // if a reset chased the FIN, and it is never retried.
    ConnectionResult closed;
    closed.connected = true;
    closed.message_count = 1000;
    closed.server_closed = true;
    closed.connection_reset = true; // a reset behind the close changes nothing
    const auto outcome = classify_connection(closed);
    check(outcome == ConnectionOutcome::server_closed,
          "RP_CLOSE_CONNECTION classifies as an orderly server close");

    ReconnectConfig config;
    config.enabled = true;
    ReconnectPolicy policy {config};
    check(!policy.should_retry(outcome, 0),
          "an orderly server close is never retried");
}

void test_session_reset_discards_stale_drawing_state()
{
    // The client half of the reconnect black screen: a pattern (and colours)
    // cached on a token before the drop must not survive into the reconnect.
    Session session(8, 1, [](std::span<const std::uint8_t>) { return true; });

    Writer create(Op::create_state);
    create.i32(7);
    session.ingest(create.finish());

    Writer low(Op::set_low_color);
    low.i32(7);
    low.u8(0); low.u8(0); low.u8(255); low.u8(255); // blue
    session.ingest(low.finish());
    Writer high(Op::set_high_color);
    high.i32(7);
    high.u8(255); high.u8(0); high.u8(0); high.u8(255); // red
    session.ingest(high.finish());
    Writer pattern(Op::set_pattern);
    pattern.i32(7);
    // 0xf0 == 11110000 MSB-first: x 0..3 high, x 4..7 low across the 8px row.
    const std::array<std::uint8_t, 8> stripe {0xf0, 0xf0, 0xf0, 0xf0,
                                              0xf0, 0xf0, 0xf0, 0xf0};
    pattern.raw(stripe);
    session.ingest(pattern.finish());
    Writer fill(Op::fill_rect);
    fill.i32(7);
    append_rect(fill, {0, 0, 7, 0});
    session.ingest(fill.finish());
    check(session.surface().pixel(0, 0) == Color {255, 0, 0, 255}
              && session.surface().pixel(7, 0) == Color {0, 0, 255, 255},
          "sanity: the striped pattern paints red and blue before the drop");

    // The drop, then the reconnect's fresh start.
    session.reset();

    // On the new connection the token is used again. It must be a clean slate:
    // set only the high colour, then fill. With the stale state discarded the
    // whole row is the new high colour (default pattern is solid high); if the
    // stripe pattern or the blue low colour had survived, the right half would
    // still be blue.
    Writer high2(Op::set_high_color);
    high2.i32(7);
    high2.u8(0); high2.u8(255); high2.u8(0); high2.u8(255); // green
    session.ingest(high2.finish());
    Writer fill2(Op::fill_rect);
    fill2.i32(7);
    append_rect(fill2, {0, 0, 7, 0});
    session.ingest(fill2.finish());

    bool all_green = true;
    for (int x = 0; x < 8; ++x) {
        if (session.surface().pixel(x, 0) != Color {0, 255, 0, 255})
            all_green = false;
    }
    check(all_green,
          "after reset(), a reused token carries no stale pattern or colour --"
          " the whole fill is the new high colour");
}

void test_session_reset_rearms_the_connection_state()
{
    Session session(8, 8, [](std::span<const std::uint8_t>) { return true; });
    Writer close(Op::close_connection);
    session.ingest(close.finish());
    check(session.server_closed() && session.message_count() == 1,
          "sanity: a close is recorded and counted");

    session.reset();
    check(!session.server_closed(),
          "reset() clears the server-closed flag so the new connection is live");
    check(session.message_count() == 0,
          "reset() zeroes the message count so 'produced nothing' is per-connection");
    check(session.negotiated_capabilities() == 0,
          "reset() drops the previous connection's negotiated capabilities");
}

void test_hello_ack_records_the_session_identity_when_resync_negotiated()
{
    Session session(16, 16, [](std::span<const std::uint8_t>) { return true; });
    check(session.session_id() == 0 && session.generation() == 0,
          "a fresh session has no identity yet");

    // RP_HELLO_ACK carrying version, caps (with RP_CAP_RESYNC), then session id
    // and generation -- the layout the server appends only for a resync-capable
    // client (RemoteHWInterface.cpp).
    Writer ack(Op::hello_ack);
    ack.u32(protocol_version);
    ack.u32(cap_string_width_reply | cap_resync);
    ack.u32(0xABCD1234); // session id
    ack.u32(1);          // generation
    session.ingest(ack.finish());
    check(session.session_id() == 0xABCD1234 && session.generation() == 1,
          "the ack's session id and generation are recorded");
    check(!session.generation_changed(),
          "the first ack is not a reconnect");

    // A second ack: same session, higher generation == a reconnect.
    Writer ack2(Op::hello_ack);
    ack2.u32(protocol_version);
    ack2.u32(cap_string_width_reply | cap_resync);
    ack2.u32(0xABCD1234);
    ack2.u32(2);
    session.ingest(ack2.finish());
    check(session.generation() == 2 && session.generation_changed(),
          "a higher generation under the same session id is a reconnect");
}

// PROTOCOL.md 4.1: RP_HELLO_ACK's payload length is not fixed, and the session
// identity is present *only* when RP_CAP_RESYNC was negotiated. So the read must
// be gated on the bit and not on the length -- a client that takes the two extra
// uint32s whenever there happen to be eight bytes left will misread whatever a
// later milestone appends, and will invent a session identity out of it. The
// fixture is therefore an ack that did NOT negotiate resync but does carry a
// plausible-looking tail; a length-only gate reads 0xABCD1234/5 as identity.
void test_hello_ack_without_resync_ignores_a_trailing_identity()
{
    Session session(16, 16, [](std::span<const std::uint8_t>) { return true; });

    Writer ack(Op::hello_ack);
    ack.u32(protocol_version);
    ack.u32(cap_string_width_reply); // no cap_resync
    ack.u32(0xABCD1234);             // not a session id: unnegotiated tail
    ack.u32(5);                      // not a generation
    session.ingest(ack.finish());

    check(session.negotiated_capabilities() == cap_string_width_reply,
          "the ack's negotiated capability set is recorded as sent");
    check(session.session_id() == 0,
          "no session id is read from an ack that did not negotiate resync");
    check(session.generation() == 0,
          "no generation is read from an ack that did not negotiate resync");
    check(!session.generation_changed(),
          "an unnegotiated tail cannot be mistaken for a reconnect");
    check(session.unhandled().empty(),
          "the trailing bytes are skipped by declared length, not rejected");
    check(!session.request_resync(),
          "a client that did not negotiate resync must not send RP_RESYNC");
}

void test_resync_barrier_discards_cached_state()
{
    Session session(8, 1, [](std::span<const std::uint8_t>) { return true; });
    // Prime a palette entry so a stale-palette regression has something to
    // catch: RP_GET_SYSTEM_PALETTE_RESULT with a single red entry.
    Writer palette(Op::get_system_palette_result);
    palette.u32(1);
    palette.u8(255); palette.u8(0); palette.u8(0); palette.u8(255);
    session.ingest(palette.finish());

    // Establish the resync capability so request_resync() is not a no-op and so
    // the barrier's generation is interpreted.
    Writer ack(Op::hello_ack);
    ack.u32(protocol_version);
    ack.u32(cap_resync);
    ack.u32(7);  // session id
    ack.u32(1);  // generation
    session.ingest(ack.finish());

    // The server -> client barrier: generation bumps, cached state is dropped.
    Writer barrier(Op::resync);
    barrier.u32(2);
    session.ingest(barrier.finish());
    check(session.generation() == 2 && session.generation_changed(),
          "the RP_RESYNC barrier advances the generation");
    check(session.unhandled().empty(),
          "RP_RESYNC is handled, not logged as an unknown opcode");

    // request_resync() is gated on the negotiated capability.
    bool sent = false;
    Session capable(16, 16, [&](std::span<const std::uint8_t> bytes) {
        // Confirm the client emits RP_RESYNC{generation} when it asks.
        Reader reader(bytes);
        check(reader.u16() == static_cast<std::uint16_t>(Op::resync),
              "request_resync() sends an RP_RESYNC frame");
        sent = true;
        return true;
    });
    Writer cap_ack(Op::hello_ack);
    cap_ack.u32(protocol_version);
    cap_ack.u32(cap_resync);
    cap_ack.u32(9);
    cap_ack.u32(1);
    capable.ingest(cap_ack.finish());
    check(capable.request_resync() && sent,
          "request_resync() emits a frame once the capability is negotiated");

    Session incapable(16, 16, [](std::span<const std::uint8_t>) {
        check(false, "a client that did not negotiate resync must not send it");
        return true;
    });
    check(!incapable.request_resync(),
          "request_resync() is a no-op without the negotiated capability");
}

#ifndef _WIN32

// Prove the classification is wired to the transport's own signals: a reset
// (RST) and a clean shutdown (FIN) reach classify_connection() as an eviction
// and a transport drop respectively, over a real loopback socket -- the offline
// stand-in for app_server dropping us. This is the mock-driven half of the
// #513 + #25 hardware round-trip that is deferred.
ConnectionResult drive_until_close(bool reset_the_connection,
                                   bool send_a_frame_first)
{
    ConnectionResult result;
    LoopbackListener server;
    if (!server.start()) {
        check(false, "reset/close test listener starts");
        return result;
    }

    TransportOptions options;
    options.host = "127.0.0.1";
    options.port = server.port;
    options.cookie = std::string(64, 'a');
    std::string error;
    auto transport = make_transport(options, error);
    if (transport == nullptr || !transport->connect(error)) {
        check(false, "reset/close test transport connects");
        return result;
    }
    result.connected = true;

    const int accepted = ::accept(server.listener, nullptr, nullptr);
    if (accepted < 0) {
        check(false, "reset/close test connection is accepted");
        return result;
    }

    Session session(32, 32, [&](std::span<const std::uint8_t> bytes) {
        std::string send_error;
        return transport->send_all(bytes, send_error);
    });
    session.start();

    // Drain the cookie + handshake the client just sent, so a reset does not
    // race unread client bytes in a way that hides the frame we mean to send.
    std::array<std::uint8_t, 4096> scratch {};
    ::recv(accepted, scratch.data(), scratch.size(), MSG_DONTWAIT);

    if (send_a_frame_first) {
        // A bare RP_INVALIDATE_RECT: a valid, session-level frame that the
        // client counts and then does nothing with. Enough to make
        // message_count > 0 so a clean close is a drop, not a gate refusal.
        const std::array<std::uint8_t, 6> frame {24, 0, 6, 0, 0, 0};
        ::send(accepted, frame.data(), frame.size(), 0);
    }

    if (reset_the_connection) {
        // SO_LINGER with a zero timeout turns close() into an RST, discarding
        // any unread inbound bytes -- exactly the shape app_server leaves for a
        // refused or evicted candidate.
        linger no_linger {1, 0};
        ::setsockopt(accepted, SOL_SOCKET, SO_LINGER, &no_linger,
                     sizeof(no_linger));
    }
    ::close(accepted);

    std::array<std::uint8_t, 4096> buffer {};
    for (;;) {
        const int count = transport->receive(buffer, 500, error);
        if (count < 0) {
            result.peer_closed = transport->peer_closed();
            result.connection_reset = transport->connection_reset();
            break;
        }
        if (count > 0)
            session.ingest(std::span(buffer.data(),
                                     static_cast<std::size_t>(count)));
        if (session.server_closed()) {
            result.server_closed = true;
            break;
        }
    }
    result.message_count = session.message_count();
    transport->close();
    return result;
}

void test_transport_reset_and_clean_close_are_distinguished()
{
    const auto reset = drive_until_close(true, true);
    check(reset.connection_reset && !reset.peer_closed,
          "a peer RST surfaces as connection_reset(), not peer_closed()");
    check(classify_connection(reset) == ConnectionOutcome::evicted,
          "and classifies as an eviction -- not retried");

    const auto clean = drive_until_close(false, true);
    check(clean.peer_closed && !clean.connection_reset,
          "a clean FIN surfaces as peer_closed(), not connection_reset()");
    check(classify_connection(clean) == ConnectionOutcome::transport_dropped,
          "a mid-session clean FIN classifies as a retriable transport drop");

    const auto empty = drive_until_close(false, false);
    check(classify_connection(empty) == ConnectionOutcome::refused,
          "a clean FIN before any message is a refusal, not a drop");

    // The end-to-end policy conclusion the whole issue turns on.
    ReconnectConfig config;
    config.enabled = true;
    ReconnectPolicy policy {config};
    check(!policy.should_retry(classify_connection(reset), 0),
          "an enabled client still does NOT reconnect after a reset (eviction)");
    check(policy.should_retry(classify_connection(clean), 0),
          "an enabled client DOES reconnect after a clean transport drop");
}

#endif // _WIN32

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
    test_draw_bitmap_options_reach_the_renderer();
    test_bitmap_rects_filter_but_do_not_tile();
    test_rect_fill_truncates_fractional_edges();
    test_stroke_cost_is_bounded_by_the_surface();
    test_readback_covers_a_large_surface();
    test_hostile_rects_do_not_escape_the_surface();
    test_close_connection_is_an_orderly_end();
    test_only_close_connection_ends_the_session();
    test_set_cursor_decodes_hotspot_and_bitmap();
    test_set_cursor_visible_toggles_state();
    test_move_cursor_to_updates_the_position();
    test_cursor_composites_at_its_hotspot();
    test_an_invisible_cursor_composites_nothing();
    test_cursor_is_clipped_to_the_surface();
    test_a_malformed_cursor_keeps_the_last_good_one();
    test_transport_factory();
    test_session_start_opens_with_init_then_hello();
#ifndef _WIN32
    test_direct_transport_presents_the_cookie_before_anything_else();
    test_direct_transport_refuses_a_connection_with_no_cookie();
    test_no_cookie_hint_names_the_listener_port_not_the_local_one();
#endif
#if defined(HAIKU_REMOTE_HAVE_WSS) && !defined(_WIN32)
    test_websocket_roundtrip();
#endif
    test_op_names_cover_the_whole_protocol();
    test_op_names_do_not_drift_from_the_op_enum();
    test_framer_rejects_a_declared_length_below_the_header();
    test_framer_rejects_an_absurd_declared_length();
    test_framer_holds_a_payload_truncated_mid_frame();
    test_framer_latches_a_framing_failure();
    test_reconnect_is_opt_in_and_off_by_default();
    test_reconnect_is_bounded();
    test_reconnect_backoff_is_exponential_and_capped();
    test_classify_connection_reads_more_than_socket_closed();
    test_no_retry_after_eviction();
    test_no_retry_after_close_connection();
    test_session_reset_discards_stale_drawing_state();
    test_session_reset_rearms_the_connection_state();
    test_hello_ack_records_the_session_identity_when_resync_negotiated();
    test_hello_ack_without_resync_ignores_a_trailing_identity();
    test_resync_barrier_discards_cached_state();
#ifndef _WIN32
    test_transport_reset_and_clean_close_are_distinguished();
#endif
    if (failures == 0) {
        std::cout << "PASS - " << checks << " checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " checks failed\n";
    return 1;
}
