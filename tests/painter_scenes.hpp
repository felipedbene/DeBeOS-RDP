#pragma once

// The scene list shared by the Painter oracle (tools/painter_oracle/) and the
// golden tests in tests/render_tests.cpp.
//
// Each scene is one DrawingEngine call with an explicit drawing state, on a
// small canvas with a known, deliberately non-uniform background. The oracle
// replays it through app_server's own Painter, DrawingEngine, PixelFormat and
// AGG sources, compiled on the host, and records every pixel; the test replays
// it through the client and compares every pixel for equality. Nothing in here
// is rendering logic -- it is data, plus the background function both sides
// paint before drawing.
//
// Plain C++17 with no dependencies beyond the standard library, because the oracle is built against Haiku
// shim headers and the test against the client's, and both include this.
//
// Fixtures are asymmetric on purpose (CLAUDE.md): canvases are wider than tall,
// shapes are off-centre, polygons are concave and non-convex, and the
// background differs in every pixel, so a mirrored, transposed or
// destination-ignoring render cannot match by accident.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace painter_scenes {

enum class Op {
    fill_rect,          // DrawingEngine::FillRect(BRect)
    fill_rect_color,    // DrawingEngine::FillRect(BRect, rgb_color)
    stroke_rect,        // DrawingEngine::StrokeRect(BRect)
    stroke_rect_color,  // DrawingEngine::StrokeRect(BRect, rgb_color)
    stroke_line,        // DrawingEngine::StrokeLine(BPoint, BPoint)
    stroke_line_color,  // DrawingEngine::StrokeLine(BPoint, BPoint, rgb_color)
    stroke_point_color, // DrawingEngine::StrokePoint(BPoint, rgb_color)
    stroke_line_array,  // DrawingEngine::StrokeLineArray
    fill_ellipse,       // DrawingEngine::DrawEllipse(r, true)
    stroke_ellipse,     // DrawingEngine::DrawEllipse(r, false)
    fill_arc,           // DrawingEngine::DrawArc(r, angle, span, true)
    stroke_arc,
    fill_round_rect,    // DrawingEngine::DrawRoundRect(r, xr, yr, true)
    stroke_round_rect,
    fill_polygon,       // DrawingEngine::DrawPolygon(..., true, closed)
    stroke_polygon,
    fill_bezier,        // DrawingEngine::DrawBezier(pts, true)
    stroke_bezier,
    fill_triangle,      // DrawingEngine::DrawTriangle(pts, bounds, true)
    stroke_triangle,
    fill_shape,         // DrawingEngine::DrawShape(..., true, offset, scale)
    stroke_shape,
    fill_region,        // DrawingEngine::FillRegion(BRegion&)
    draw_bitmap,        // DrawingEngine::DrawBitmap(bitmap, source, rect, options)
};

struct P {
    float x;
    float y;
};

struct IRect {
    int left;
    int top;
    int right;
    int bottom;
};

// One element of a StrokeLineArray.
struct ArrayLine {
    P from;
    P to;
    std::uint8_t color[4];
};

// BShape op words, verbatim from headers/private/interface/ShapePrivate.h:21-28.
constexpr std::uint32_t op_line_to = 0x10000000;
constexpr std::uint32_t op_bezier_to = 0x20000000;
constexpr std::uint32_t op_close = 0x40000000;
constexpr std::uint32_t op_move_to = 0x80000000;
constexpr std::uint32_t op_large_arc_to_cw = 0x01000000;
constexpr std::uint32_t op_large_arc_to_ccw = 0x02000000;
constexpr std::uint32_t op_small_arc_to_cw = 0x04000000;
constexpr std::uint32_t op_small_arc_to_ccw = 0x08000000;

// Haiku enum values (headers/os/interface/GraphicsDefs.h:309-346,
// InterfaceDefs.h:280-292).
enum Mode : std::uint32_t {
    copy = 0, over, erase, invert, add, subtract, blend, min, max, select, alpha
};
constexpr std::uint32_t round_cap = 0, butt_cap = 3, square_cap = 4;
constexpr std::uint32_t round_join = 0, miter_join = 1, bevel_join = 2,
                        butt_join = 3, square_join = 4;
constexpr std::uint32_t pixel_alpha = 0, constant_alpha = 1;
// DrawBitmap options, InterfaceDefs.h:306-323.
constexpr std::uint32_t tile_bitmap = 0x3, filter_bilinear = 0x100;
// color_space, headers/os/interface/GraphicsDefs.h:176-177.
constexpr std::uint32_t rgb32 = 0x0008, rgba32 = 0x2008;
constexpr std::uint32_t alpha_overlay = 0, alpha_composite = 1;

// BGradient::Type, headers/os/interface/Gradient.h:27-34.
constexpr std::uint32_t gradient_linear = 0, gradient_radial = 1,
                        gradient_radial_focus = 2, gradient_diamond = 3,
                        gradient_conic = 4;

// One BGradient::ColorStop: an RGBA colour at an offset in [0, 255].
struct GradientStop {
    std::uint8_t color[4];
    float offset;
};

struct Scene {
    const char* name = "";
    int width = 40;
    int height = 28;

    // Drawing state.
    std::uint8_t high[4] {20, 40, 200, 255};
    std::uint8_t low[4] {250, 240, 90, 255};
    float pen = 1;
    std::uint32_t mode = copy;
    std::uint32_t source_alpha = pixel_alpha;
    std::uint32_t alpha_function = alpha_overlay;
    std::uint8_t pattern[8] {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    std::uint32_t cap = butt_cap;
    std::uint32_t join = miter_join;
    float miter = 10;
    // BAffineTransform sx, shy, shx, sy, tx, ty.
    double transform[6] {1, 0, 0, 1, 0, 0};
    int x_offset = 0;
    int y_offset = 0;
    // The clipping region; empty means the whole canvas.
    std::vector<IRect> clip;

    // The call.
    Op op = Op::fill_rect;
    float rect[4] {0, 0, -1, -1};
    float a = 0; // angle, or x radius
    float b = 0; // span, or y radius
    std::uint8_t color[4] {0, 0, 0, 255}; // for the *_color calls
    std::vector<P> points;
    bool closed = true;
    std::vector<std::uint32_t> shape_ops;
    P shape_offset {0, 0};
    float shape_scale = 1;
    std::vector<IRect> region; // fill_region
    std::vector<ArrayLine> lines;
    // A gradient variant. With no gradient_stops it is a linear gradient
    // (0,0)-(30,10) whose two stops are both gradient_color, so the pixels
    // exercise coverage and the blend_color_hspan arithmetic alone. With
    // gradient_stops it is a real gradient of gradient_kind -- BGradient::Type,
    // headers/os/interface/Gradient.h:27-34 -- built from the geometry below
    // that its kind reads, and the pixels pin span_gradient's colours too.
    bool gradient = false;
    std::uint8_t gradient_color[4] {200, 30, 60, 255};
    std::uint32_t gradient_kind = gradient_linear;
    P gradient_start {0, 0};
    P gradient_end {30, 10};
    P gradient_center {0, 0};
    P gradient_focal {0, 0};
    float gradient_radius = 0;
    float gradient_angle = 0;
    std::vector<GradientStop> gradient_stops;

    // draw_bitmap: a bitmap of this size and colour space (rgba32 or rgb32),
    // whose pixels are bitmap_pixel() below, drawn from `source` (bitmapRect)
    // into `rect` (viewRect) with these DrawBitmap options. The client side
    // decodes it from the bytes RemoteMessage::AddBitmap would send.
    int bitmap_width = 7;
    int bitmap_height = 5;
    std::uint32_t bitmap_color_space = rgba32;
    float source[4] {0, 0, 6, 4};
    std::uint32_t bitmap_options = 0;

    // The view's B_SUBPIXEL_PRECISE flag, which Painter reads as
    // fSubpixelPrecise (Painter.cpp:311) and RP_SET_SUBPIXEL_PRECISE carries.
    bool subpixel_precise = false;
};

// The bitmap a draw_bitmap scene draws, as straight (unpremultiplied) RGBA:
// for B_RGB32, rgba[3] is the fourth byte, which that colour space calls
// unused. Every pixel differs from its neighbours in every channel and the
// image has no symmetry, so a mirrored, transposed, shifted or rescaled copy
// cannot match by accident; the fourth byte mixes 255, 0 and partial values,
// so each path's alpha handling is visible too.
//
// A B_RGB32 bitmap also carries B_TRANSPARENT_MAGIC_RGBA32's colour,
// 0x77 0x74 0x77, three times: at (1, 3) with fourth byte 0 -- the magic
// value itself, 0x00777477 (GraphicsDefs.cpp:33) -- and at (0, 0) and (4, 0)
// with 255 and 152, which as whole uint32s are not the magic value.
inline void bitmap_pixel(int x, int y, std::uint8_t rgba[4],
                         std::uint32_t color_space = rgba32)
{
    rgba[0] = static_cast<std::uint8_t>(25 + 33 * x);
    rgba[1] = static_cast<std::uint8_t>(230 - 41 * y);
    rgba[2] = static_cast<std::uint8_t>((17 + 29 * x * y + 7 * x) % 256);
    if ((x + 2 * y) % 5 == 0)
        rgba[3] = 255;
    else if ((x * y) % 7 == 3)
        rgba[3] = 0;
    else
        rgba[3] = static_cast<std::uint8_t>((60 + 23 * x + 31 * y) % 256);
    if (color_space == rgb32
        && ((x == 1 && y == 3) || (x == 0 && y == 0) || (x == 4 && y == 0))) {
        rgba[0] = 0x77;
        rgba[1] = 0x74;
        rgba[2] = 0x77;
    }
}

// The background both sides paint before drawing. Every pixel differs from its
// neighbours in all three channels, and a sparse lattice of pixels equals the
// low or the high colour so that B_OP_SELECT has something to swap.
inline void background(const Scene& scene, int x, int y, std::uint8_t rgb[3])
{
    if ((x * 3 + y) % 7 == 0) {
        rgb[0] = scene.low[0];
        rgb[1] = scene.low[1];
        rgb[2] = scene.low[2];
        return;
    }
    if ((x + 2 * y) % 11 == 0) {
        rgb[0] = scene.high[0];
        rgb[1] = scene.high[1];
        rgb[2] = scene.high[2];
        return;
    }
    rgb[0] = static_cast<std::uint8_t>(40 + x * 5);
    rgb[1] = static_cast<std::uint8_t>(60 + y * 6);
    rgb[2] = static_cast<std::uint8_t>(200 - x * 3 - y);
}

inline std::vector<Scene> scenes()
{
    std::vector<Scene> all;
    const auto push = [&all](const Scene& scene) { all.push_back(scene); };
    const std::uint8_t stipple[8] {0xaa, 0x55, 0xcc, 0x33, 0xf0, 0x0f, 0x81, 0x7e};
    const std::uint8_t solid_low[8] {0, 0, 0, 0, 0, 0, 0, 0};

    const auto set_pattern = [](Scene& s, const std::uint8_t (&p)[8]) {
        for (int i = 0; i < 8; ++i)
            s.pattern[i] = p[i];
    };
    const auto set_rect = [](Scene& s, float l, float t, float r, float b) {
        s.rect[0] = l;
        s.rect[1] = t;
        s.rect[2] = r;
        s.rect[3] = b;
    };

    // -- rects ------------------------------------------------------------
    {
        Scene s;
        s.name = "fill-rect-fractional";
        s.op = Op::fill_rect;
        set_rect(s, 2.3f, 3.7f, 17.6f, 12.2f);
        push(s);
        s.name = "fill-rect-negative-fractional";
        set_rect(s, -0.5f, -1.5f, 6.5f, 4.25f);
        push(s);
        s.name = "fill-rect-stipple-copy";
        set_pattern(s, stipple);
        set_rect(s, 3, 2, 30, 20);
        push(s);
        s.name = "fill-rect-transformed";
        set_pattern(s, stipple);
        s.pattern[0] = 0xff; // keep it non-solid
        s.transform[0] = 1.25;
        s.transform[1] = 0.35;
        s.transform[2] = -0.2;
        s.transform[3] = 0.9;
        s.transform[4] = 3.5;
        s.transform[5] = 1.25;
        set_rect(s, 2, 1, 22.5f, 14.25f);
        push(s);
    }
    {
        Scene s;
        s.name = "fill-rect-alpha-blendrect";
        s.mode = alpha;
        s.high[3] = 100;
        set_rect(s, 1.5f, 2.5f, 25.5f, 18.75f);
        push(s);
        s.name = "fill-rect-alpha-constant-transformed";
        s.source_alpha = constant_alpha;
        s.transform[0] = 0.8;
        s.transform[2] = 0.3;
        push(s);
    }
    {
        Scene s;
        s.name = "fill-rect-color-fractional";
        s.op = Op::fill_rect_color;
        s.color[0] = 255;
        s.color[1] = 128;
        s.color[2] = 0;
        set_rect(s, 0.5f, 0.5f, 9.5f, 9.5f);
        push(s);
        s.name = "fill-rect-color-ignores-mode";
        s.mode = invert;
        set_rect(s, 4.25f, 2.75f, 21.5f, 13.0f);
        push(s);
    }
    {
        Scene s;
        s.name = "fill-region";
        s.op = Op::fill_region;
        set_pattern(s, stipple);
        s.mode = over;
        s.region = {{2, 2, 13, 9}, {14, 2, 20, 17}, {2, 18, 35, 24}};
        push(s);
    }
    // -- strokes of rects ---------------------------------------------------
    {
        Scene s;
        s.name = "stroke-rect-1px-fast";
        s.op = Op::stroke_rect;
        set_rect(s, 3.6f, 2.2f, 30.4f, 20.9f);
        push(s);
        s.name = "stroke-rect-pen3";
        s.pen = 3;
        push(s);
        s.name = "stroke-rect-pen4-round-join";
        s.pen = 4;
        s.join = round_join;
        push(s);
        s.name = "stroke-rect-1px-stipple-agg";
        s.pen = 1;
        s.join = miter_join;
        set_pattern(s, stipple);
        push(s);
    }
    {
        Scene s;
        s.name = "stroke-rect-color";
        s.op = Op::stroke_rect_color;
        s.color[0] = 10;
        s.color[1] = 250;
        s.color[2] = 10;
        set_rect(s, 2.7f, 3.2f, 29.5f, 19.8f);
        push(s);
        s.name = "stroke-rect-color-one-row";
        set_rect(s, 4, 6, 25, 6);
        push(s);
    }
    // -- lines --------------------------------------------------------------
    {
        Scene s;
        s.name = "stroke-line-diagonal-1px";
        s.op = Op::stroke_line;
        s.points = {{2.5f, 3.25f}, {33.75f, 21.5f}};
        push(s);
        s.name = "stroke-line-horizontal-fast";
        s.points = {{3.6f, 7.2f}, {28.9f, 7.7f}};
        s.points[1].y = 7.2f;
        push(s);
        s.name = "stroke-line-dot";
        s.points = {{9.7f, 5.2f}, {9.7f, 5.2f}};
        push(s);
        s.name = "stroke-line-dot-pen3";
        s.pen = 3;
        push(s);
        for (const auto cap : {butt_cap, round_cap, square_cap}) {
            Scene t = s;
            t.pen = 5;
            t.cap = cap;
            t.points = {{6.0f, 18.5f}, {31.25f, 6.75f}};
            t.name = cap == butt_cap ? "stroke-line-pen5-butt"
                : cap == round_cap ? "stroke-line-pen5-round"
                : "stroke-line-pen5-square";
            push(t);
        }
        s.name = "stroke-line-pen2-even";
        s.pen = 2;
        s.cap = butt_cap;
        s.points = {{4, 4}, {34, 15}};
        push(s);
        s.name = "stroke-line-transformed";
        s.pen = 2.5f;
        s.transform[0] = 1.1;
        s.transform[1] = -0.4;
        s.transform[2] = 0.25;
        s.transform[3] = 1.3;
        s.transform[4] = 1;
        s.transform[5] = 6;
        s.x_offset = 2;
        s.y_offset = 1;
        push(s);
    }
    {
        Scene s;
        s.name = "stroke-line-color-diagonal";
        s.op = Op::stroke_line_color;
        s.mode = copy;
        s.color[0] = 230;
        s.color[1] = 20;
        s.color[2] = 120;
        s.points = {{30.5f, 2.5f}, {3.25f, 24.0f}};
        push(s);
        s.name = "stroke-line-color-vertical-fast";
        s.points = {{12.5f, 2.5f}, {12.5f, 20.0f}};
        push(s);
        s.name = "stroke-line-color-solid-low-draws-nothing";
        set_pattern(s, solid_low);
        s.points = {{30.5f, 2.5f}, {3.25f, 24.0f}};
        push(s);
    }
    {
        Scene s;
        s.name = "stroke-point-color";
        s.op = Op::stroke_point_color;
        s.color[0] = 255;
        s.color[1] = 255;
        s.color[2] = 255;
        s.points = {{7.8f, 5.4f}};
        push(s);
    }
    {
        Scene s;
        s.name = "stroke-line-array";
        s.op = Op::stroke_line_array;
        s.pen = 1.5f;
        set_pattern(s, stipple);
        s.lines = {
            {{2, 2}, {30, 9}, {255, 0, 0, 255}},
            {{3.5f, 20}, {35, 12.5f}, {0, 255, 40, 255}},
            {{20, 1}, {20, 25}, {250, 250, 250, 255}},
        };
        push(s);
    }
    // -- ellipses -----------------------------------------------------------
    {
        Scene s;
        s.name = "fill-ellipse";
        s.op = Op::fill_ellipse;
        set_rect(s, 3.2f, 2.6f, 30.7f, 20.4f);
        push(s);
        s.name = "stroke-ellipse-1px";
        s.op = Op::stroke_ellipse;
        push(s);
        s.name = "stroke-ellipse-pen3";
        s.pen = 3;
        push(s);
        s.name = "stroke-ellipse-pen2";
        s.pen = 2;
        push(s);
        s.name = "fill-ellipse-tiny";
        s.op = Op::fill_ellipse;
        s.pen = 1;
        set_rect(s, 5, 5, 7, 6);
        push(s);
        s.name = "fill-ellipse-transformed";
        set_rect(s, 3, 2, 25, 15);
        s.transform[0] = 1.2;
        s.transform[1] = 0.3;
        s.transform[2] = -0.35;
        s.transform[3] = 0.95;
        s.transform[4] = 4;
        s.transform[5] = 2;
        push(s);
    }
    // -- arcs ---------------------------------------------------------------
    {
        Scene s;
        s.name = "fill-arc";
        s.op = Op::fill_arc;
        set_rect(s, 3.3f, 2.5f, 33.6f, 24.2f);
        s.a = 30;
        s.b = 250;
        push(s);
        s.name = "stroke-arc-pen2-round";
        s.op = Op::stroke_arc;
        s.pen = 2;
        s.cap = round_cap;
        push(s);
        s.name = "stroke-arc-1px-negative-span";
        s.pen = 1;
        s.cap = butt_cap;
        s.a = 200;
        s.b = -135;
        push(s);
    }
    // -- round rects --------------------------------------------------------
    {
        Scene s;
        s.name = "fill-round-rect";
        s.op = Op::fill_round_rect;
        set_rect(s, 2.4f, 3.1f, 33.2f, 24.6f);
        s.a = 7;
        s.b = 4.5f;
        push(s);
        s.name = "stroke-round-rect-pen2";
        s.op = Op::stroke_round_rect;
        s.pen = 2;
        push(s);
        s.name = "stroke-round-rect-pen1";
        s.pen = 1;
        push(s);
        s.name = "fill-round-rect-oversized-radius";
        s.op = Op::fill_round_rect;
        s.a = 30;
        s.b = 3;
        push(s);
    }
    // -- polygons and joins -------------------------------------------------
    {
        Scene s;
        s.name = "fill-polygon-concave";
        s.op = Op::fill_polygon;
        s.points = {{3.5f, 2.25f}, {30.75f, 4.5f}, {18.2f, 11.6f},
                    {34.0f, 24.5f}, {6.3f, 20.1f}, {11.0f, 12.0f}};
        push(s);
        s.name = "fill-polygon-self-intersecting";
        s.points = {{2, 2}, {36, 22}, {36, 3}, {3, 24.5f}};
        push(s);
        s.name = "stroke-polygon-open-miter";
        s.op = Op::stroke_polygon;
        s.closed = false;
        s.pen = 3;
        s.points = {{3.5f, 22.0f}, {10.25f, 3.5f}, {17.0f, 21.0f},
                    {26.5f, 6.0f}, {36.0f, 20.5f}};
        push(s);
        s.name = "stroke-polygon-open-miter-limit";
        s.miter = 1.5f;
        push(s);
        s.name = "stroke-polygon-open-bevel-square";
        s.miter = 10;
        s.join = bevel_join;
        s.cap = square_cap;
        push(s);
        s.name = "stroke-polygon-open-round-round";
        s.join = round_join;
        s.cap = round_cap;
        s.pen = 4;
        push(s);
        s.name = "stroke-polygon-closed-pen1";
        s.closed = true;
        s.pen = 1;
        s.join = miter_join;
        s.cap = butt_cap;
        s.points = {{3.5f, 2.25f}, {30.75f, 4.5f}, {18.2f, 11.6f},
                    {34.0f, 24.5f}, {6.3f, 20.1f}, {11.0f, 12.0f}};
        push(s);
        s.name = "stroke-polygon-closed-pen5";
        s.pen = 5;
        push(s);
        s.name = "stroke-polygon-transformed";
        s.pen = 2;
        s.transform[0] = 0.9;
        s.transform[1] = 0.2;
        s.transform[2] = -0.15;
        s.transform[3] = 0.8;
        s.transform[4] = 3;
        s.transform[5] = 2.5;
        push(s);
    }
    // -- beziers and triangles ----------------------------------------------
    {
        Scene s;
        s.name = "fill-bezier";
        s.op = Op::fill_bezier;
        s.points = {{2.5f, 24.0f}, {8.0f, -6.0f}, {36.0f, 30.0f}, {35.0f, 3.5f}};
        push(s);
        s.name = "stroke-bezier-pen2";
        s.op = Op::stroke_bezier;
        s.pen = 2;
        push(s);
        s.name = "stroke-bezier-1px";
        s.pen = 1;
        push(s);
    }
    {
        Scene s;
        s.name = "fill-triangle";
        s.op = Op::fill_triangle;
        s.points = {{4.7f, 3.2f}, {35.1f, 11.9f}, {12.4f, 25.6f}};
        set_rect(s, 4, 3, 36, 26);
        push(s);
        s.name = "stroke-triangle-pen2";
        s.op = Op::stroke_triangle;
        s.pen = 2;
        push(s);
    }
    // -- shapes -------------------------------------------------------------
    {
        Scene s;
        s.name = "fill-shape";
        s.op = Op::fill_shape;
        // MoveTo, LineTo x2, BezierTo x1, SmallArc CCW, Close; then a second
        // closed sub-path that overlaps the first.
        s.shape_ops = {
            op_move_to | op_line_to | 2,
            op_bezier_to | 3,
            op_small_arc_to_ccw | 3,
            op_close,
            op_move_to | op_line_to | 3,
            op_close,
        };
        s.points = {
            {2.5f, 3.0f}, {20.0f, 2.5f}, {24.0f, 9.0f},
            {30.0f, 14.0f}, {34.0f, 20.0f}, {26.0f, 24.0f},
            {6.0f, 5.0f}, {0.4f, 0}, {8.0f, 23.0f},
            {14.0f, 10.0f}, {36.0f, 8.0f}, {30.0f, 18.0f}, {16.0f, 22.0f},
        };
        s.shape_offset = {0.5f, 0.25f};
        s.shape_scale = 1;
        push(s);
        s.name = "stroke-shape-pen2";
        s.op = Op::stroke_shape;
        s.pen = 2;
        s.join = round_join;
        push(s);
        s.name = "fill-shape-scaled";
        s.op = Op::fill_shape;
        s.pen = 1;
        s.join = miter_join;
        s.shape_offset = {1.0f, 0.5f};
        s.shape_scale = 0.75f;
        push(s);
        s.name = "fill-shape-large-arc-cw";
        s.shape_ops = {op_move_to | op_line_to | 1, op_large_arc_to_cw | 3,
                       op_close};
        s.points = {{6, 20}, {14, 20}, {7, 6}, {0.3f, 0}, {28, 16}};
        s.shape_offset = {0, 0};
        s.shape_scale = 1;
        push(s);
    }
    // -- clipping -----------------------------------------------------------
    {
        Scene s;
        s.name = "fill-ellipse-clipped-two-rects";
        s.op = Op::fill_ellipse;
        set_rect(s, 1.5f, 1.25f, 37.0f, 26.0f);
        s.clip = {{0, 0, 16, 11}, {21, 5, 39, 20}};
        push(s);
        s.name = "stroke-polygon-clipped";
        s.op = Op::stroke_polygon;
        s.pen = 3;
        s.closed = true;
        s.points = {{3.5f, 2.25f}, {30.75f, 4.5f}, {18.2f, 11.6f},
                    {34.0f, 24.5f}, {6.3f, 20.1f}};
        s.clip = {{4, 3, 30, 13}, {2, 14, 12, 27}};
        push(s);
    }
    // -- every drawing mode on one antialiased shape ------------------------
    for (int patterned = 0; patterned < 2; ++patterned) {
        for (std::uint32_t mode = copy; mode <= select; ++mode) {
            static const char* const names[2][10] = {
                {"mode-copy", "mode-over", "mode-erase", "mode-invert",
                 "mode-add", "mode-subtract", "mode-blend", "mode-min",
                 "mode-max", "mode-select"},
                {"mode-copy-stipple", "mode-over-stipple", "mode-erase-stipple",
                 "mode-invert-stipple", "mode-add-stipple",
                 "mode-subtract-stipple", "mode-blend-stipple",
                 "mode-min-stipple", "mode-max-stipple", "mode-select-stipple"},
            };
            Scene s;
            s.name = names[patterned][mode];
            s.op = Op::fill_polygon;
            s.mode = mode;
            // Brightness 115 by brightness_for(): about half the background
            // is brighter and half darker, so B_OP_MIN and B_OP_MAX both have
            // pixels to take and pixels to leave.
            s.high[0] = 150;
            s.high[1] = 90;
            s.high[2] = 160;
            if (patterned)
                set_pattern(s, stipple);
            s.points = {{3.5f, 2.25f}, {30.75f, 4.5f}, {18.2f, 11.6f},
                        {34.0f, 24.5f}, {6.3f, 20.1f}, {11.0f, 12.0f}};
            push(s);
        }
    }
    for (int patterned = 0; patterned < 2; ++patterned) {
        for (std::uint32_t source = pixel_alpha; source <= constant_alpha; ++source) {
            for (std::uint32_t function = alpha_overlay;
                 function <= alpha_composite; ++function) {
                static const char* const names[2][2][2] = {
                    {{"mode-alpha-pixel-overlay", "mode-alpha-pixel-composite"},
                     {"mode-alpha-constant-overlay",
                      "mode-alpha-constant-composite"}},
                    {{"mode-alpha-pixel-overlay-stipple",
                      "mode-alpha-pixel-composite-stipple"},
                     {"mode-alpha-constant-overlay-stipple",
                      "mode-alpha-constant-composite-stipple"}},
                };
                Scene s;
                s.name = names[patterned][source][function];
                s.op = Op::fill_ellipse;
                s.mode = alpha;
                s.source_alpha = source;
                s.alpha_function = function;
                s.high[3] = 150;
                s.low[3] = 90;
                if (patterned)
                    set_pattern(s, stipple);
                set_rect(s, 3.2f, 2.6f, 30.7f, 20.4f);
                push(s);
                // The same, as a 1px stroke: spans narrower than four pixels
                // and solid hlines take different branches in these modes.
                s.name = patterned
                    ? (source ? (function ? "stroke-alpha-constant-composite-stipple"
                                          : "stroke-alpha-constant-overlay-stipple")
                              : (function ? "stroke-alpha-pixel-composite-stipple"
                                          : "stroke-alpha-pixel-overlay-stipple"))
                    : (source ? (function ? "stroke-alpha-constant-composite"
                                          : "stroke-alpha-constant-overlay")
                              : (function ? "stroke-alpha-pixel-composite"
                                          : "stroke-alpha-pixel-overlay"));
                s.op = Op::stroke_polygon;
                s.closed = true;
                s.pen = 1.5f;
                s.points = {{3.5f, 2.25f}, {30.75f, 4.5f}, {18.2f, 11.6f},
                            {34.0f, 24.5f}, {6.3f, 20.1f}};
                push(s);
            }
        }
    }
    {
        Scene s;
        s.name = "mode-over-solid-low-draws-nothing";
        s.op = Op::fill_ellipse;
        s.mode = over;
        set_pattern(s, solid_low);
        set_rect(s, 3.2f, 2.6f, 30.7f, 20.4f);
        push(s);
        s.name = "mode-copy-solid-low";
        s.mode = copy;
        push(s);
        s.name = "pattern-offsets";
        set_pattern(s, stipple);
        s.x_offset = 3;
        s.y_offset = 5;
        push(s);
    }
    // -- gradients: coverage and blend_color_hspan --------------------------
    for (std::uint32_t mode : {std::uint32_t(copy), std::uint32_t(over), std::uint32_t(erase),
                               std::uint32_t(add), std::uint32_t(alpha)}) {
        Scene s;
        s.op = Op::fill_ellipse;
        s.mode = mode;
        s.gradient = true;
        set_pattern(s, stipple); // must be overridden by SolidPatternGuard
        set_rect(s, 3.2f, 2.6f, 30.7f, 20.4f);
        s.gradient_color[3] = mode == alpha ? 170 : 255;
        s.name = mode == copy ? "gradient-fill-ellipse-copy"
            : mode == over ? "gradient-fill-ellipse-over"
            : mode == erase ? "gradient-fill-ellipse-erase"
            : mode == add ? "gradient-fill-ellipse-add"
            : "gradient-fill-ellipse-alpha";
        push(s);
    }
    {
        Scene s;
        s.gradient = true;
        s.name = "gradient-stroke-polygon";
        s.op = Op::stroke_polygon;
        s.pen = 2.5f;
        s.closed = false;
        s.points = {{3.5f, 22.0f}, {10.25f, 3.5f}, {17.0f, 21.0f},
                    {26.5f, 6.0f}, {36.0f, 20.5f}};
        push(s);
        s.name = "gradient-fill-rect-fractional";
        s.op = Op::fill_rect;
        set_rect(s, 2.3f, 3.7f, 17.6f, 12.2f);
        push(s);
        s.name = "gradient-stroke-line";
        s.op = Op::stroke_line;
        s.pen = 1;
        s.points = {{2.5f, 3.25f}, {33.75f, 21.5f}};
        push(s);
        s.name = "gradient-fill-round-rect";
        s.op = Op::fill_round_rect;
        set_rect(s, 2.4f, 3.1f, 33.2f, 24.6f);
        s.a = 7;
        s.b = 4.5f;
        push(s);
        s.name = "gradient-stroke-arc";
        s.op = Op::stroke_arc;
        set_rect(s, 3.3f, 2.5f, 33.6f, 24.2f);
        s.a = 30;
        s.b = 250;
        s.pen = 2;
        push(s);
        s.name = "gradient-fill-triangle";
        s.op = Op::fill_triangle;
        s.points = {{4.7f, 3.2f}, {35.1f, 11.9f}, {12.4f, 25.6f}};
        set_rect(s, 4, 3, 36, 26);
        push(s);
    }
    // -- caps, pen 9: the three Haiku cap numbers, and an illegal one --------
    // RP_SET_STROKE_MODE carries cap_mode verbatim (B_ROUND_CAP 0, B_BUTT_CAP
    // 3, B_SQUARE_CAP 4); 7 is no cap at all, which agg_line_cap_mode_for
    // turns into butt (Painter.cpp:1924-1936).
    for (int diagonal = 0; diagonal < 2; ++diagonal) {
        for (const std::uint32_t cap : {butt_cap, round_cap, square_cap, 7u}) {
            static const char* const names[2][4] = {
                {"cap-vertical-butt", "cap-vertical-round", "cap-vertical-square",
                 "cap-vertical-illegal"},
                {"cap-diagonal-butt", "cap-diagonal-round", "cap-diagonal-square",
                 "cap-diagonal-illegal"},
            };
            const int index = cap == butt_cap ? 0 : cap == round_cap ? 1
                : cap == square_cap ? 2 : 3;
            Scene s;
            s.name = names[diagonal][index];
            s.width = 64;
            s.height = 60;
            s.op = Op::stroke_line;
            s.pen = 9;
            s.cap = cap;
            s.points = diagonal ? std::vector<P> {{20, 20}, {40, 37}}
                                : std::vector<P> {{20, 20}, {20, 40}};
            push(s);
        }
    }
    {
        Scene s;
        s.name = "stroke-line-dot-stipple";
        s.op = Op::stroke_line;
        set_pattern(s, stipple);
        s.pattern[2] = 0xff;
        s.points = {{9.7f, 2.2f}, {9.7f, 2.2f}};
        push(s);
        s.name = "stroke-line-dot-alpha";
        s.mode = alpha;
        s.high[3] = 120;
        set_pattern(s, stipple);
        s.pattern[2] = 0xff;
        push(s);
    }
    {
        // B_PIXEL_ALPHA + B_ALPHA_OVERLAY with a pattern takes
        // blend_hline_alpha_po, which tests its 16-bit alpha against 255
        // instead of 255 * 255 (DrawingModeAlphaPO.h:55). With opaque colours a
        // fully covered run is therefore BLEND16'd at 65025, not assigned.
        Scene s;
        s.name = "mode-alpha-pixel-overlay-stipple-opaque";
        s.op = Op::fill_ellipse;
        s.mode = alpha;
        set_pattern(s, stipple);
        set_rect(s, 3.2f, 2.6f, 30.7f, 20.4f);
        push(s);
    }
    {
        // A line 5600 pixels long: through the subpixel rasteriser that is
        // 16800 cells, past AGG's 16384-cell dx_limit, where
        // rasterizer_cells_aa::line() splits the line in two -- and then,
        // missing the `return` (agg_rasterizer_cells_aa.h:322-328), renders the
        // whole of it a second time as well.
        Scene s;
        s.name = "fill-polygon-wider-than-dx-limit";
        s.width = 5600;
        s.height = 6;
        s.op = Op::fill_polygon;
        s.points = {{0.5f, 1.25f}, {5599.5f, 3.75f}, {5599.5f, 4.5f}, {0.5f, 2.0f}};
        push(s);
    }
    {
        // The dot special case (Painter.cpp:530-539) in B_OP_ALPHA with
        // B_ALPHA_COMPOSITE: blend_pixel_alpha_pc_solid, BLEND_COMPOSITE16.
        Scene s;
        s.name = "stroke-line-dot-alpha-composite";
        s.op = Op::stroke_line;
        s.mode = alpha;
        s.alpha_function = alpha_composite;
        s.high[3] = 120;
        s.points = {{12.6f, 7.9f}, {12.6f, 7.9f}};
        push(s);
    }
    {
        // A stroke under a 2x transform: conv_stroke's approximation scale is
        // the transform's scale (Painter.cpp:1979), which sets the number of
        // points in every round join and cap.
        Scene s;
        s.name = "stroke-polygon-transformed-round";
        s.op = Op::stroke_polygon;
        s.closed = false;
        s.pen = 2.5f;
        s.join = round_join;
        s.cap = round_cap;
        s.transform[0] = 2.2;
        s.transform[1] = 0.3;
        s.transform[2] = -0.4;
        s.transform[3] = 1.9;
        s.transform[4] = 2;
        s.transform[5] = 1;
        s.points = {{2.5f, 9.0f}, {6.0f, 2.5f}, {9.5f, 9.5f}, {15.0f, 3.0f}};
        push(s);
    }
    {
        Scene s;
        s.op = Op::fill_ellipse;
        s.gradient = true;
        set_rect(s, 3.2f, 2.6f, 30.7f, 20.4f);
        // blend_color_hspan_add skips colours with alpha 0
        // (DrawingModeAdd.h:138).
        s.name = "gradient-fill-ellipse-add-transparent";
        s.mode = add;
        s.gradient_color[3] = 0;
        push(s);
        // B_CONSTANT_ALPHA scales the gradient's alpha by the high colour's
        // (DrawingModeAlphaCO.h:153).
        s.name = "gradient-fill-ellipse-alpha-constant";
        s.mode = alpha;
        s.source_alpha = constant_alpha;
        s.high[3] = 128;
        s.gradient_color[3] = 200;
        push(s);
    }
    {
        // ArcTo radii are multiplied by the view scale (Painter.cpp:1833-1835);
        // these are large enough that bezier_arc_svg does not have to grow
        // them, so the scale shows.
        Scene s;
        s.name = "fill-shape-scaled-arc-radii";
        s.op = Op::fill_shape;
        s.shape_ops = {op_move_to | op_line_to | 1, op_small_arc_to_cw | 3,
                       op_close};
        s.points = {{4, 30}, {20, 34}, {30, 22}, {0, 0}, {44, 6}};
        s.shape_scale = 0.75f;
        s.shape_offset = {1, 0.5f};
        push(s);
        // ...and radii far too small for the chord: radii_check is above 10,
        // radii_ok() is false, and arc_to draws a straight line instead
        // (agg_bezier_arc.cpp:179, agg_path_storage.h:930-937).
        s.name = "fill-shape-tiny-arc-radii";
        s.points = {{4, 30}, {20, 34}, {1, 0.5f}, {0, 0}, {44, 6}};
        push(s);
    }
    {
        // A region of two rects meeting at x = 20/21: every span that crosses
        // the seam reaches the drawing mode as two shorter spans
        // (agg_renderer_region.h:286-298), and B_OP_ALPHA's solid hline
        // switches from blend_line32 to BLEND16 below four pixels
        // (DrawingModeAlphaCOSolid.h:52-61).
        Scene s;
        s.name = "fill-polygon-split-region-alpha";
        s.op = Op::fill_polygon;
        s.mode = alpha;
        s.source_alpha = constant_alpha;
        s.high[3] = 140;
        s.clip = {{0, 0, 20, 27}, {21, 0, 39, 27}};
        s.points = {{2.5f, 3.2f}, {36.0f, 1.7f}, {37.5f, 24.0f}, {4.0f, 22.5f}};
        push(s);
        s.name = "fill-polygon-split-region-subpixel-edge";
        s.mode = copy;
        s.points = {{2.5f, 3.2f}, {36.0f, 9.7f}, {37.5f, 24.0f}, {4.0f, 15.5f}};
        push(s);
        // The subpixel rasteriser's left clip edge is at 3/256 of where it
        // should be (agg_rasterizer_scanline_aa_subpix.h:376-384), so lines
        // left of the region are not clipped there; the region rects cut them.
        s.name = "fill-polygon-left-of-the-region";
        s.clip = {{10, 0, 39, 27}};
        s.points = {{1.5f, 3.2f}, {36.0f, 9.7f}, {37.5f, 24.0f}, {2.2f, 18.5f}};
        push(s);
    }
    {
        // Less than a pixel wide, its edges in the first and third subpixel
        // of the same pixel and nothing in the middle one: the
        // `area1 && !area2 && area3` arm of the subpixel sweep
        // (agg_rasterizer_scanline_aa_subpix.h:274-278).
        Scene s;
        s.name = "fill-polygon-thin-sliver";
        s.op = Op::fill_polygon;
        s.points = {{10.1f, 2.0f}, {10.8f, 2.0f}, {10.8f, 25.0f}, {10.1f, 25.0f}};
        push(s);
        s.name = "fill-polygon-thin-slanted-sliver";
        s.points = {{10.1f, 2.0f}, {10.8f, 2.0f}, {17.8f, 25.0f}, {17.1f, 25.0f}};
        push(s);
    }
    {
        // A gradient under a two-rect region: blend_color_hspan is cut per
        // rect as well (agg_renderer_base.h:391-414).
        Scene s;
        s.name = "gradient-fill-ellipse-split-region";
        s.op = Op::fill_ellipse;
        s.gradient = true;
        s.mode = blend;
        set_rect(s, 3.2f, 2.6f, 30.7f, 20.4f);
        s.clip = {{0, 0, 17, 27}, {18, 0, 39, 27}};
        push(s);
    }
    {
        // A rect one row tall: StrokeRect sends a two-point path through
        // close_polygon (Painter.cpp:906-916), and vcgen_stroke drops the
        // closed flag for fewer than three vertices (agg_vcgen_stroke.cpp:
        // 76) -- so it is stroked open, with caps.
        Scene s;
        s.name = "stroke-rect-one-row-pen3-round-cap";
        s.op = Op::stroke_rect;
        s.pen = 3;
        s.cap = round_cap;
        set_rect(s, 4.0f, 9.0f, 30.0f, 9.0f);
        push(s);
    }
    // -- a large shape, for a span wider than the short-span thresholds -----
    {
        Scene s;
        s.name = "fill-ellipse-wide-canvas";
        s.width = 97;
        s.height = 31;
        s.op = Op::fill_ellipse;
        set_rect(s, 1.6f, 1.2f, 95.3f, 29.5f);
        push(s);
    }
    // -- gradients: span_gradient's colours, for all five kinds -------------
    //
    // Four stops, unevenly spaced, one at a fractional offset, every channel
    // moving differently, so a reversed, rescaled or re-rounded colour table
    // cannot match by accident. The first and last stops sit at 0 and 255:
    // the 256-entry table _MakeGradient fills is a stack array it never
    // initialises (pod_auto_array, Painter.cpp:2293 and 2304-2306), so
    // entries outside the first..last stop are whatever was on the server's
    // stack -- nothing a golden could pin.
    const std::vector<GradientStop> four_stops {
        {{200, 30, 60, 255}, 0},
        {{20, 180, 90, 255}, 70.5f},
        {{240, 220, 10, 255}, 181},
        {{30, 40, 230, 255}, 255},
    };
    // Diamond and conic gradients reach offset 255 only 100px out, which no
    // canvas here is; these stops put the variation in the near range.
    const std::vector<GradientStop> near_stops {
        {{250, 250, 250, 255}, 0},
        {{220, 20, 40, 255}, 9.25f},
        {{10, 200, 60, 255}, 30},
        {{20, 30, 240, 255}, 60},
        {{0, 0, 0, 255}, 255},
    };
    const auto identity = [](Scene& s) {
        const double unit[6] {1, 0, 0, 1, 0, 0};
        for (int i = 0; i < 6; ++i)
            s.transform[i] = unit[i];
        s.x_offset = 0;
        s.y_offset = 0;
    };
    const auto set_transform = [](Scene& s, double sx, double shy, double shx,
                                  double sy, double tx, double ty) {
        s.transform[0] = sx;
        s.transform[1] = shy;
        s.transform[2] = shx;
        s.transform[3] = sy;
        s.transform[4] = tx;
        s.transform[5] = ty;
    };
    {
        Scene s;
        s.gradient = true;
        s.gradient_stops = four_stops;
        s.gradient_kind = gradient_linear;
        s.gradient_start = {3.5f, 4.25f};
        s.gradient_end = {31.75f, 19.5f};

        s.name = "gradient-linear-fill-rect";
        s.op = Op::fill_rect;
        set_rect(s, 1.3f, 1.6f, 37.2f, 25.4f);
        push(s);
        s.name = "gradient-linear-reversed-fill-ellipse";
        s.op = Op::fill_ellipse;
        s.gradient_start = {35.0f, 22.5f};
        s.gradient_end = {6.25f, 2.0f};
        set_rect(s, 2.2f, 1.6f, 37.7f, 25.4f);
        push(s);
        // Short: most of the shape lies beyond both ends, where the index
        // clamps (agg_span_gradient.h:100-101).
        s.name = "gradient-linear-short-clamped";
        s.gradient_start = {16.0f, 11.0f};
        s.gradient_end = {21.5f, 13.0f};
        push(s);
        // The gradient lives in view space: _CalcLinearGradientTransform
        // multiplies fTransform in (Painter.cpp:2147-2160), offsets and all.
        s.name = "gradient-linear-transformed";
        s.op = Op::fill_polygon;
        s.points = {{2.5f, 3.0f}, {30.0f, 1.5f}, {26.0f, 18.0f}, {5.5f, 16.5f}};
        s.gradient_start = {3.5f, 4.25f};
        s.gradient_end = {31.75f, 19.5f};
        set_transform(s, 1.1, 0.3, -0.25, 1.2, 2.5, 0.75);
        s.x_offset = 3;
        s.y_offset = 2;
        push(s);
        s.name = "gradient-linear-stroke-bezier-alpha";
        s.op = Op::stroke_bezier;
        identity(s);
        s.mode = alpha;
        s.pen = 3.5f;
        s.points = {{2.5f, 24.0f}, {8.0f, -6.0f}, {30.0f, 34.0f}, {37.0f, 3.5f}};
        s.gradient_stops = {
            {{200, 30, 60, 40}, 0},
            {{20, 180, 90, 255}, 100},
            {{240, 220, 10, 140}, 255},
        };
        push(s);
    }
    {
        // FillRectVerticalGradient (Painter.cpp:1115-1164): the other
        // gradient table, one colour per row, written raw. The gradient
        // starts and ends inside the rect, so both of its edge fills run, and
        // the clipping region cuts the rect so the table is offset from it.
        Scene s;
        s.gradient = true;
        s.gradient_stops = four_stops;
        s.gradient_kind = gradient_linear;
        s.name = "gradient-vertical-fill-rect-fast";
        s.op = Op::fill_rect;
        s.gradient_start = {12.75f, 5.6f};
        s.gradient_end = {12.75f, 21.9f};
        set_rect(s, 2.6f, 1.4f, 35.3f, 26.8f);
        s.clip = {{0, 3, 39, 24}};
        push(s);
        // Upside down: not the fast path (Painter.cpp:1046-1049).
        s.name = "gradient-vertical-upside-down";
        s.gradient_start = {12.75f, 21.9f};
        s.gradient_end = {12.75f, 5.6f};
        push(s);
        // B_OP_ALPHA: not the fast path either; the span path blends.
        s.name = "gradient-vertical-alpha-mode";
        s.mode = alpha;
        s.gradient_start = {12.75f, 5.6f};
        s.gradient_end = {12.75f, 21.9f};
        s.gradient_stops[1].color[3] = 90;
        push(s);
    }
    {
        // Radial: gradientStop is an int (Painter.h:343), so the radius is
        // truncated, 17.7 -> 17, before span_gradient scales it.
        Scene s;
        s.gradient = true;
        s.gradient_stops = four_stops;
        s.gradient_kind = gradient_radial;
        s.gradient_center = {14.5f, 11.25f};
        s.gradient_radius = 17.7f;
        s.name = "gradient-radial-fill-rect";
        s.op = Op::fill_rect;
        set_rect(s, 0.5f, 0.5f, 38.5f, 26.5f);
        push(s);
        s.name = "gradient-radial-transformed-ellipse";
        s.op = Op::fill_ellipse;
        set_rect(s, 1.2f, 1.6f, 36.7f, 25.4f);
        set_transform(s, 0.8, -0.35, 0.5, 1.3, 4.0, -1.5);
        push(s);
        // Radial focus: the focal point is never handed to AGG
        // (Painter.cpp:2102-2112) -- gradient_radial_focus is default
        // constructed, focus (0,0), radius 100 -- so it is a radial gradient
        // with gradient_radial_focus's own rounding.
        s.name = "gradient-radial-focus-fill-rect";
        s.gradient_kind = gradient_radial_focus;
        s.gradient_center = {22.25f, 9.5f};
        s.gradient_focal = {30.0f, 20.0f};
        s.gradient_radius = 13.4f;
        s.op = Op::fill_rect;
        set_rect(s, 0.5f, 0.5f, 38.5f, 26.5f);
        identity(s);
        push(s);
        s.name = "gradient-radial-focus-stroke-round-rect";
        s.op = Op::stroke_round_rect;
        s.pen = 4;
        s.a = 6;
        s.b = 9;
        set_rect(s, 3.5f, 2.5f, 35.25f, 24.0f);
        push(s);
    }
    {
        // Diamond and conic: nothing on the wire sets their extent;
        // gradientStop's default of 100 does (Painter.h:343,
        // Painter.cpp:2114-2137).
        Scene s;
        s.gradient = true;
        s.gradient_stops = near_stops;
        s.gradient_kind = gradient_diamond;
        s.gradient_center = {20.3f, 9.6f};
        s.name = "gradient-diamond-fill-rect";
        s.op = Op::fill_rect;
        set_rect(s, 0.5f, 0.5f, 38.5f, 26.5f);
        push(s);
        s.name = "gradient-diamond-rotated-ellipse";
        s.op = Op::fill_ellipse;
        set_rect(s, 1.2f, 1.6f, 36.7f, 25.4f);
        set_transform(s, 0.7, 0.6, -0.6, 0.7, 12.0, -4.0);
        push(s);
        // The conic's angle is not used either (Painter.cpp:2125-2136).
        s.gradient_kind = gradient_conic;
        s.gradient_stops = four_stops;
        s.gradient_center = {19.5f, 13.25f};
        s.gradient_angle = 35;
        s.name = "gradient-conic-rotated-ellipse";
        push(s);
        s.name = "gradient-conic-fill-rect";
        s.op = Op::fill_rect;
        set_rect(s, 0.5f, 0.5f, 38.5f, 26.5f);
        identity(s);
        push(s);
        // Two stops at one offset: a zero-length segment, which
        // _MakeGradient skips (`dist > 0`, Painter.cpp:2272).
        s.name = "gradient-conic-coincident-stops-region";
        s.op = Op::fill_region;
        s.region = {{1, 1, 17, 12}, {6, 13, 38, 26}};
        s.gradient_stops = {
            {{200, 30, 60, 255}, 0},
            {{20, 180, 90, 255}, 128},
            {{240, 220, 10, 255}, 128},
            {{30, 40, 230, 255}, 255},
        };
        push(s);
    }
    // -- DrawBitmap ---------------------------------------------------------
    //
    // BitmapPainter::Draw picks one of seven code paths from the scale, the
    // transform, the drawing mode and the options (BitmapPainter.cpp:58-171),
    // and each rounds a fractional rect its own way: (int32) truncation in
    // the no-scale, nearest-neighbour and bilinear loops, AGG coverage in the
    // generic one. _DetermineTransform rounds both rects first unless the view
    // is B_SUBPIXEL_PRECISE (:191-194), which the twins below exercise.
    {
        const auto set_source = [](Scene& s, float l, float t, float r, float b) {
            s.source[0] = l;
            s.source[1] = t;
            s.source[2] = r;
            s.source[3] = b;
        };
        Scene s;
        s.op = Op::draw_bitmap;
        // DrawBitmapNoScale<Bgr32Copy>: unscaled, B_OP_COPY.
        s.name = "bitmap-copy-unscaled-fractional";
        set_rect(s, 3.4f, 2.6f, 9.4f, 6.6f);
        push(s);
        s.name = "bitmap-copy-unscaled-cropped";
        set_source(s, 1.5f, 0.5f, 5.5f, 3.5f);
        set_rect(s, 10.3f, 4.7f, 14.3f, 7.7f);
        push(s);
        s.name = "bitmap-copy-unscaled-negative";
        set_source(s, 0, 0, 6, 4);
        set_rect(s, -1.5f, -0.7f, 4.5f, 3.3f);
        push(s);
        // The source rect reaches past the bitmap: _DetermineTransform cuts it
        // back and moves the destination by the scaled difference.
        s.name = "bitmap-copy-source-beyond-bounds";
        set_source(s, -2.25f, -1.5f, 8.5f, 6.25f);
        set_rect(s, 1.75f, 2.25f, 34.5f, 24.75f);
        push(s);
        // DrawBitmapNearestNeighborCopy.
        s.name = "bitmap-copy-scaled-nearest";
        set_source(s, 0, 0, 6, 4);
        set_rect(s, 2.3f, 1.6f, 30.7f, 20.2f);
        push(s);
        s.name = "bitmap-copy-scaled-nearest-cropped-clipped";
        set_source(s, 1.25f, 0.75f, 5.5f, 3.75f);
        set_rect(s, 4.6f, 3.3f, 37.2f, 25.4f);
        s.clip = {{0, 0, 21, 13}, {9, 14, 33, 22}};
        push(s);
        s.clip.clear();
        // DrawBitmapBilinear<ColorTypeRgb, DrawModeCopy>, BilinearDefault.
        s.bitmap_options = filter_bilinear;
        s.name = "bitmap-copy-bilinear";
        set_source(s, 0, 0, 6, 4);
        set_rect(s, 2.3f, 1.6f, 30.7f, 20.2f);
        push(s);
        s.name = "bitmap-copy-bilinear-cropped-clipped";
        set_source(s, 1.25f, 0.75f, 5.5f, 3.75f);
        set_rect(s, 4.6f, 3.3f, 37.2f, 25.4f);
        s.clip = {{0, 0, 21, 13}, {9, 14, 33, 22}};
        push(s);
        s.clip.clear();
        // An exact 2x: BilinearLowFilterRatio (DrawBitmapBilinear.h:600-610).
        s.name = "bitmap-copy-bilinear-2x";
        set_source(s, 0, 0, 6, 4);
        set_rect(s, 4.4f, 3.6f, 17.4f, 12.6f);
        push(s);
        s.bitmap_options = 0;
        // B_OP_OVER unscaled is DrawBitmapNoScale<Bgr32Alpha>; scaled, it is
        // DrawBitmapGeneric<Fill> through the drawing mode's colour span.
        s.mode = over;
        s.name = "bitmap-over-unscaled-fractional";
        set_rect(s, 12.6f, 9.4f, 18.6f, 13.4f);
        push(s);
        s.name = "bitmap-over-scaled-generic";
        set_rect(s, 3.3f, 2.8f, 33.6f, 23.3f);
        push(s);
        s.bitmap_options = filter_bilinear;
        s.name = "bitmap-over-bilinear-generic";
        push(s);
        s.bitmap_options = 0;
        // B_OP_ALPHA, B_PIXEL_ALPHA, B_ALPHA_OVERLAY: DrawBitmapNoScale
        // <Bgr32Alpha> unscaled, DrawBitmapBilinear<ColorTypeRgba,
        // DrawModeAlphaOverlay> filtered, the generic path otherwise.
        s.mode = alpha;
        s.name = "bitmap-alpha-unscaled";
        set_rect(s, 20.2f, 5.7f, 26.2f, 9.7f);
        push(s);
        s.name = "bitmap-alpha-scaled-generic";
        set_rect(s, 2.6f, 1.4f, 35.3f, 25.6f);
        push(s);
        s.bitmap_options = filter_bilinear;
        s.name = "bitmap-alpha-bilinear";
        push(s);
        s.source_alpha = constant_alpha;
        s.high[3] = 170;
        s.name = "bitmap-alpha-constant-bilinear-generic";
        push(s);
        s.source_alpha = pixel_alpha;
        s.high[3] = 255;
        s.bitmap_options = 0;
        s.mode = min;
        s.name = "bitmap-min-scaled-generic";
        set_rect(s, 1.5f, 2.5f, 31.25f, 20.75f);
        push(s);
        s.mode = copy;
        // A transform sends every mode through DrawBitmapGeneric<Fill>.
        s.name = "bitmap-copy-transformed";
        set_rect(s, 3.5f, 2.25f, 22.75f, 15.5f);
        set_transform(s, 1.1, 0.3, -0.25, 1.2, 2.5, 0.75);
        push(s);
        s.bitmap_options = filter_bilinear;
        s.name = "bitmap-copy-transformed-bilinear";
        push(s);
        identity(s);
        // Tiling: DrawBitmapGeneric<Tile>, scale 1, phase from the source.
        s.bitmap_options = tile_bitmap;
        s.name = "bitmap-tiled";
        set_source(s, 2.5f, 1.5f, 8.5f, 5.5f);
        set_rect(s, 1.6f, 2.3f, 36.4f, 24.8f);
        push(s);
        s.bitmap_options = tile_bitmap | filter_bilinear;
        s.name = "bitmap-tiled-bilinear";
        push(s);
        s.bitmap_options = tile_bitmap;
        s.mode = over;
        s.name = "bitmap-tiled-over-clipped";
        s.clip = {{2, 1, 30, 11}, {6, 12, 37, 26}};
        push(s);
        s.clip.clear();
        s.bitmap_options = 0;
        s.mode = alpha;
        // B_OP_ALPHA's other blending modes on B_RGBA32: constant alpha and
        // B_ALPHA_COMPOSITE take DrawBitmapGeneric<Fill> even unscaled.
        s.source_alpha = constant_alpha;
        s.high[3] = 170;
        s.name = "bitmap-alpha-constant-unscaled";
        set_source(s, 0, 0, 6, 4);
        set_rect(s, 20.2f, 5.7f, 26.2f, 9.7f);
        push(s);
        s.source_alpha = pixel_alpha;
        s.high[3] = 255;
        s.alpha_function = alpha_composite;
        s.name = "bitmap-alpha-composite-unscaled";
        push(s);
        s.alpha_function = alpha_overlay;
    }
    // -- B_RGB32 bitmaps -----------------------------------------------------
    //
    // _ConvertColorSpace (BitmapPainter.cpp:258-329) leaves a B_RGB32 bitmap
    // untouched under B_OP_COPY and B_OP_ALPHA -- so B_OP_ALPHA blends with
    // its fourth byte as alpha, "compatible to BeOS" (:265-278) -- and under
    // every other mode imports it as B_RGBA32 with alpha 255, the magic value
    // 0x00777477 then made transparent. The no-scale B_OP_OVER shortcut
    // (:97-103, Bgr32Over) works on the raw bits instead.
    {
        Scene s;
        s.op = Op::draw_bitmap;
        s.bitmap_color_space = rgb32;
        s.mode = alpha;
        // DrawBitmapNoScale<Bgr32Alpha>, DrawBitmapGeneric<Fill> and
        // DrawBitmapBilinear<ColorTypeRgba, DrawModeAlphaOverlay>.
        s.name = "bitmap-rgb32-alpha-unscaled";
        set_rect(s, 20.2f, 5.7f, 26.2f, 9.7f);
        push(s);
        s.name = "bitmap-rgb32-alpha-scaled-generic";
        set_rect(s, 2.6f, 1.4f, 35.3f, 25.6f);
        push(s);
        s.bitmap_options = filter_bilinear;
        s.name = "bitmap-rgb32-alpha-bilinear";
        push(s);
        s.source_alpha = constant_alpha;
        s.high[3] = 170;
        s.name = "bitmap-rgb32-alpha-constant-bilinear-generic";
        push(s);
        s.bitmap_options = 0;
        s.name = "bitmap-rgb32-alpha-constant-unscaled";
        set_rect(s, 20.2f, 5.7f, 26.2f, 9.7f);
        push(s);
        s.source_alpha = pixel_alpha;
        s.high[3] = 255;
        s.alpha_function = alpha_composite;
        s.name = "bitmap-rgb32-alpha-composite-unscaled";
        push(s);
        s.alpha_function = alpha_overlay;
        // B_OP_OVER: the raw Bgr32Over shortcut unscaled, the converted
        // bitmap through DrawBitmapGeneric<Fill> scaled.
        s.mode = over;
        s.name = "bitmap-rgb32-over-unscaled";
        set_rect(s, 12.6f, 9.4f, 18.6f, 13.4f);
        push(s);
        s.name = "bitmap-rgb32-over-scaled-generic";
        set_rect(s, 3.3f, 2.8f, 33.6f, 23.3f);
        push(s);
        // Any other mode converts too: B_OP_MIN through the generic path.
        s.mode = min;
        s.name = "bitmap-rgb32-min-scaled-generic";
        set_rect(s, 1.5f, 2.5f, 31.25f, 20.75f);
        push(s);
        // B_OP_COPY, unconverted: Bgr32Copy, and the generic path under a
        // transform.
        s.mode = copy;
        s.name = "bitmap-rgb32-copy-unscaled";
        set_rect(s, 3.4f, 2.6f, 9.4f, 6.6f);
        push(s);
        s.bitmap_options = filter_bilinear;
        s.name = "bitmap-rgb32-copy-transformed-bilinear";
        set_rect(s, 3.5f, 2.25f, 22.75f, 15.5f);
        set_transform(s, 1.1, 0.3, -0.25, 1.2, 2.5, 0.75);
        push(s);
        identity(s);
    }
    // -- B_SUBPIXEL_PRECISE: every scene again, with the flag set -----------
    //
    // Painter consults fSubpixelPrecise in _Align, AlignRect,
    // AlignEllipseRect and StrokeLine (Painter.cpp:532-600, 1291, 1667;
    // Painter.h:426), so the same call can land somewhere else; each twin is
    // checked against app_server's own render with the flag set.
    {
        static std::map<std::string, std::string> names;
        const std::size_t count = all.size();
        for (std::size_t i = 0; i < count; ++i) {
            Scene s = all[i];
            s.subpixel_precise = true;
            std::string& name = names[s.name];
            if (name.empty())
                name = std::string(s.name) + "-subpixel-precise";
            s.name = name.c_str();
            push(s);
        }
    }
    return all;
}


} // namespace painter_scenes
