#pragma once

// app_server's software renderer for shapes, as the client reproduces it: the
// DrawingEngine entry points the RP_* drawing messages correspond to, the
// Painter methods behind them, AGG (agg_port.hpp) underneath, and
// PixelFormat's per-drawing-mode span functions at the bottom. Each function
// here is named after, and follows, one DrawingEngine method; see painter.cpp
// for the file:line of every rule.
//
// Internal to the client: Surface is the public interface.

#include "haiku_remote/types.hpp"

#include <cstdint>
#include <span>

namespace haiku_remote::painter {

struct Canvas {
    std::uint8_t* bgra = nullptr;
    int width = 0;
    int height = 0;
};

// One BShape as RP_*_SHAPE carries it (RemoteDrawingEngine::DrawShape).
struct ShapeData {
    std::span<const std::uint32_t> ops;
    std::span<const Point> points;
    Point offset;
    float scale = 1;
};

// One ViewLineArrayInfo of RP_STROKE_LINE_ARRAY.
struct ArrayLine {
    Point from;
    Point to;
    Color color;
};

// A B_RGBA32 bitmap as BitmapPainter sees it once _ConvertColorSpace has run:
// BGRA, straight alpha, rows of exactly width * 4 bytes.
struct BitmapData {
    const std::uint8_t* bgra = nullptr;
    int width = 0;
    int height = 0;
};

// DrawingEngine::FillRect(BRect) and its gradient overload.
void fill_rect(Canvas canvas, const DrawState& state, Rect rect,
               const Gradient* gradient = nullptr);
// DrawingEngine::FillRect(BRect, rgb_color): a raw fill that ignores the
// drawing mode and the pattern.
void fill_rect_color(Canvas canvas, const DrawState& state, Rect rect, Color color);
// DrawingEngine::FillRegion(BRegion&) and its gradient overload.
void fill_region(Canvas canvas, const DrawState& state, std::span<const Rect> rects,
                 const Gradient* gradient = nullptr);
// DrawingEngine::StrokeRect(BRect) and its gradient overload.
void stroke_rect(Canvas canvas, const DrawState& state, Rect rect,
                 const Gradient* gradient = nullptr);
// DrawingEngine::StrokeRect(BRect, rgb_color): four raw one-pixel lines.
void stroke_rect_color(Canvas canvas, const DrawState& state, Rect rect, Color color);
// DrawingEngine::StrokeLine(BPoint, BPoint) and its gradient overload.
void stroke_line(Canvas canvas, const DrawState& state, Point from, Point to,
                 const Gradient* gradient = nullptr);
// DrawingEngine::StrokeLine(BPoint, BPoint, rgb_color).
void stroke_line_color(Canvas canvas, const DrawState& state, Point from, Point to,
                       Color color);
// DrawingEngine::StrokePoint(BPoint, rgb_color).
void stroke_point_color(Canvas canvas, const DrawState& state, Point point, Color color);
// DrawingEngine::StrokeLineArray.
void stroke_line_array(Canvas canvas, const DrawState& state,
                       std::span<const ArrayLine> lines);
// DrawingEngine::DrawEllipse.
void draw_ellipse(Canvas canvas, const DrawState& state, Rect rect, bool filled,
                  const Gradient* gradient = nullptr);
// DrawingEngine::DrawArc.
void draw_arc(Canvas canvas, const DrawState& state, Rect rect, float angle,
              float span, bool filled, const Gradient* gradient = nullptr);
// DrawingEngine::DrawRoundRect.
void draw_round_rect(Canvas canvas, const DrawState& state, Rect rect,
                     float x_radius, float y_radius, bool filled,
                     const Gradient* gradient = nullptr);
// DrawingEngine::DrawPolygon. `bounds` is the rect the message carries.
void draw_polygon(Canvas canvas, const DrawState& state,
                  std::span<const Point> points, Rect bounds, bool filled,
                  bool closed, const Gradient* gradient = nullptr);
// DrawingEngine::DrawBezier.
void draw_bezier(Canvas canvas, const DrawState& state,
                 std::span<const Point, 4> points, bool filled,
                 const Gradient* gradient = nullptr);
// DrawingEngine::DrawTriangle. `bounds` is the rect the message carries.
void draw_triangle(Canvas canvas, const DrawState& state,
                   std::span<const Point, 3> points, Rect bounds, bool filled,
                   const Gradient* gradient = nullptr);
// DrawingEngine::DrawShape.
void draw_shape(Canvas canvas, const DrawState& state, const ShapeData& shape,
                bool filled, const Gradient* gradient = nullptr);

// One pixel of subpixel (LCD) text at device (x, y), already clipped by the
// caller: PixelFormat's subpixel span for the current drawing mode, under the
// B_SOLID_HIGH pattern text is always drawn with, `color` as the high colour.
// `covers` are FreeType's three LCD values in screen order (left to right),
// as FontEngine's decompose_ft_bitmap_subpix stores them
// (FontEngine.cpp:399-405). `ordering_rgb` is gSubpixelOrderingRGB, which the
// wire does not carry; the server default is RGB (DesktopSettings.cpp:91).
// Returns whether the pixel was written; where it was not, the server leaves
// the pixel alone too.
bool blend_text_subpix(Canvas canvas, const DrawState& state, int x, int y,
                       Color color, const std::uint8_t covers[3],
                       bool ordering_rgb = true);

// DrawingEngine::DrawBitmap(bitmap, bitmapRect, viewRect, options).
void draw_bitmap(Canvas canvas, const DrawState& state, BitmapData bitmap,
                 Rect bitmap_rect, Rect view_rect, std::uint32_t options);

} // namespace haiku_remote::painter
