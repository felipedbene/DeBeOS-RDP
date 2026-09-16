#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/protocol.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

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
    test_draw_string_with_offsets_replies();
    test_extended_renderer_opcodes();
    if (failures == 0) {
        std::cout << "PASS - " << checks << " checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " checks failed\n";
    return 1;
}
