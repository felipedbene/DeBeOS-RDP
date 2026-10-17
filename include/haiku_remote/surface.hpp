#pragma once

#include "haiku_remote/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace haiku_remote {

namespace painter {
struct Canvas;
}
using painter_canvas = painter::Canvas;

struct Bitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
};

// Haiku's `DrawBitmap` option bits, verbatim from
// headers/os/interface/InterfaceDefs.h:306-323. They reach us as the `options`
// word of RP_DRAW_BITMAP and RP_DRAW_BITMAP_RECTS, which the server copies
// straight from BView::DrawBitmap
// (src/servers/app/drawing/interface/remote/RemoteDrawingEngine.cpp:414 and
// :470). `BView::DrawTiledBitmap` defaults to B_TILE_BITMAP (View.h:278), so
// the tiling bits are not exotic: while they were discarded, every tiled draw
// rendered as a single stretched copy.
inline constexpr std::uint32_t tile_bitmap_x = 0x00000001;
inline constexpr std::uint32_t tile_bitmap_y = 0x00000002;
inline constexpr std::uint32_t tile_bitmap = tile_bitmap_x | tile_bitmap_y;
inline constexpr std::uint32_t filter_bitmap_bilinear = 0x00000100;

// One line of RP_STROKE_LINE_ARRAY (Haiku's ViewLineArrayInfo).
struct ColoredLine {
    Point from;
    Point to;
    Color color;
};

// app_server's subpixel (LCD) text blend for one pixel: what the
// fBlendSolidHSpanSubpix span PixelFormat::SetDrawingMode picks
// (drawing_modes/PixelFormat.cpp:157-357) does to `destination` for the three
// covers FreeType's LCD bitmap gives that pixel, left to right. Text always
// draws under SolidPatternGuard (Painter.cpp:1492, :1513), so the pattern is
// B_SOLID_HIGH and the renderer colour is the high colour (Painter.cpp:466-476,
// :1720-1726). Returns nullopt where the server leaves the pixel alone.
// It is the Painter port's span (painter::blend_text_subpix, src/painter.cpp),
// the one solid shapes use, run on a single pixel -- not a second copy.
//
// `ordering_rgb` is gSubpixelOrderingRGB. Nothing on the wire carries it; the
// server default is RGB (DesktopSettings.cpp:91), and that is what the client
// passes. gSubpixelAverageWeight is not an input at all, deliberately: the only
// code that reads it for pixels is the *_avrg_filtering scanline, selected by
// AVERAGE_BASED_SUBPIXEL_FILTERING, and that define is commented out
// (GlobalSubpixelSettings.h:20; defines.h:61-67 and FontEngine.h:157-161 fall
// back to the plain agg::scanline_u8_subpix, which stores covers untouched).
// The weight reaches only FontCacheEntry::GenerateSignature's cache key
// (FontCacheEntry.cpp:400-404).
[[nodiscard]] std::optional<Color> blend_subpixel_text(
    Color destination, const DrawState& state, std::uint8_t cover_left,
    std::uint8_t cover_middle, std::uint8_t cover_right,
    bool ordering_rgb = true);

class Surface {
public:
    static constexpr int max_dimension = 16384;

    Surface(int width, int height);

    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] int stride() const { return width_ * 4; }
    [[nodiscard]] std::span<const std::uint8_t> pixels() const { return pixels_; }
    [[nodiscard]] std::span<std::uint8_t> pixels() { return pixels_; }

    [[nodiscard]] Color pixel(int x, int y) const;
    void set_pixel(int x, int y, Color color);
    void clear(Color color = {0, 0, 0, 255});

    // Reallocate the canvas at a new size, for a live resolution change
    // (RP_UPDATE_DISPLAY_MODE mid-session). The overlap of the old and new
    // canvases keeps its pixels at the same coordinates; anything newly exposed
    // starts opaque black, as a fresh Surface does. app_server treats the new
    // mode as an all-new buffer and repaints the whole frame
    // (Desktop.cpp:3644-3663), so these are placeholders until that repaint
    // lands -- keeping the overlap only spares the user a black flash. Throws
    // std::invalid_argument, leaving the surface untouched, when either
    // dimension is outside [1, max_dimension].
    void resize(int width, int height);

    // The shape primitives. Each is one app_server DrawingEngine call --
    // named after it, taking the arguments its RP_* message carries -- and
    // renders the way that call does on a local screen: through Painter's
    // AGG pipeline with its subpixel-antialiased rasteriser, its pixel-centre
    // alignment rules and every drawing mode's own coverage arithmetic (see
    // src/painter.cpp). A null or absent `gradient` is the solid variant.
    // `state == nullptr` means a default DrawState: B_OP_COPY, B_SOLID_HIGH,
    // no transform and no clipping.

    // DrawingEngine::FillRect(BRect) / (BRect, BGradient)
    void fill_rect(Rect rect, const DrawState& state,
                   const Gradient* gradient = nullptr);
    void fill_gradient_rect(Rect rect, const Gradient& gradient,
                            const DrawState& state)
    {
        fill_rect(rect, state, &gradient);
    }
    // DrawingEngine::FillRect(BRect, rgb_color): raw, ignores mode and pattern.
    void fill_rect_color(Rect rect, Color color, const DrawState* state = nullptr);
    // DrawingEngine::FillRegion(BRegion&) / (BRegion&, BGradient)
    void fill_region(std::span<const Rect> rects, const DrawState& state,
                     const Gradient* gradient = nullptr);
    // DrawingEngine::StrokeRect(BRect, rgb_color): four raw 1px lines.
    void stroke_rect(Rect rect, Color color, const DrawState* state = nullptr);
    // DrawingEngine::StrokeRect(BRect) / (BRect, BGradient)
    void stroke_rect(Rect rect, const DrawState& state,
                     const Gradient* gradient = nullptr);
    // DrawingEngine::StrokeLine(BPoint, BPoint) / (..., BGradient)
    void stroke_line(Point from, Point to, const DrawState& state,
                     const Gradient* gradient = nullptr);
    // DrawingEngine::StrokeLine(BPoint, BPoint, rgb_color)
    void stroke_line_color(Point from, Point to, Color color,
                           const DrawState& state);
    // DrawingEngine::StrokePoint(BPoint, rgb_color)
    void stroke_point_color(Point point, Color color, const DrawState& state);
    // DrawingEngine::StrokeLineArray
    void stroke_line_array(std::span<const ColoredLine> lines,
                           const DrawState& state);
    // DrawingEngine::DrawEllipse
    void fill_ellipse(Rect rect, const DrawState& state,
                      const Gradient* gradient = nullptr);
    void stroke_ellipse(Rect rect, const DrawState& state,
                        const Gradient* gradient = nullptr);
    // DrawingEngine::DrawArc
    void fill_arc(Rect rect, float angle, float span, const DrawState& state,
                  const Gradient* gradient = nullptr);
    void stroke_arc(Rect rect, float angle, float span, const DrawState& state,
                    const Gradient* gradient = nullptr);
    // DrawingEngine::DrawRoundRect
    void fill_round_rect(Rect rect, float x_radius, float y_radius,
                         const DrawState& state,
                         const Gradient* gradient = nullptr);
    void stroke_round_rect(Rect rect, float x_radius, float y_radius,
                           const DrawState& state,
                           const Gradient* gradient = nullptr);
    // DrawingEngine::DrawPolygon. `bounds` is the rect the message carries;
    // the server only uses it to decide whether the call touches the
    // clipping region at all.
    void draw_polygon(std::span<const Point> points, Rect bounds, bool filled,
                      bool closed, const DrawState& state,
                      const Gradient* gradient = nullptr);
    // DrawPolygon with the points' own bounding box, for local callers.
    void fill_polygon(std::span<const Point> points, const DrawState& state);
    void stroke_polyline(std::span<const Point> points, const DrawState& state,
                         bool closed = false);
    // DrawingEngine::DrawBezier
    void draw_bezier(std::span<const Point, 4> points, bool filled,
                     const DrawState& state, const Gradient* gradient = nullptr);
    // DrawingEngine::DrawTriangle
    void draw_triangle(std::span<const Point, 3> points, Rect bounds,
                       bool filled, const DrawState& state,
                       const Gradient* gradient = nullptr);
    // DrawingEngine::DrawShape: BShape op words and points, as
    // RemoteDrawingEngine::DrawShape sends them.
    void draw_shape(std::span<const std::uint32_t> ops,
                    std::span<const Point> points, Point offset, float scale,
                    bool filled, const DrawState& state,
                    const Gradient* gradient = nullptr);
    void invert_rect(Rect rect, const DrawState* state = nullptr);
    void copy_rect(Rect source, int dx, int dy);
    void draw_bitmap(const Bitmap& bitmap, Rect source, Rect destination,
                     const DrawState& state, std::uint32_t options = 0);
    void paint_coverage(int x, int y, Color color, const DrawState& state,
                        std::uint8_t coverage, bool high_selected = true);
    void paint_subpixel_coverage(int x, int y, Color color,
                                 const DrawState& state,
                                 std::uint8_t red_coverage,
                                 std::uint8_t green_coverage,
                                 std::uint8_t blue_coverage);

private:
    painter_canvas canvas();

    int width_;
    int height_;
    std::vector<std::uint8_t> pixels_;

    [[nodiscard]] bool visible(int x, int y, const DrawState* state) const;
    void composite(int x, int y, Color source, const DrawState& state,
                   bool high_selected, std::uint8_t coverage = 255);
};

} // namespace haiku_remote
