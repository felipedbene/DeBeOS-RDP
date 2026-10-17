#pragma once

#include "haiku_remote/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace haiku_remote {

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

// app_server's subpixel (LCD) text blend for one pixel: what the
// fBlendSolidHSpanSubpix span PixelFormat::SetDrawingMode picks
// (drawing_modes/PixelFormat.cpp:157-357) does to `destination` for the three
// covers FreeType's LCD bitmap gives that pixel, left to right. Text always
// draws under SolidPatternGuard (Painter.cpp:1492, :1513), so the pattern is
// B_SOLID_HIGH and the renderer colour is the high colour (Painter.cpp:466-476,
// :1720-1726). Returns nullopt where the server leaves the pixel alone.
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

    void fill_rect(Rect rect, const DrawState& state);
    void fill_rect_color(Rect rect, Color color, const DrawState* state = nullptr);
    void stroke_rect(Rect rect, Color color, const DrawState* state = nullptr);
    void line(Point from, Point to, Color color, const DrawState* state = nullptr,
              bool use_pattern = false);
    void fill_ellipse(Rect rect, const DrawState& state);
    void stroke_ellipse(Rect rect, const DrawState& state);
    void fill_arc(Rect rect, float angle, float span, const DrawState& state);
    void stroke_arc(Rect rect, float angle, float span, const DrawState& state);
    void fill_polygon(std::span<const Point> points, const DrawState& state);
    void stroke_polyline(std::span<const Point> points, const DrawState& state,
                         bool closed = false);
    void fill_gradient_polygon(std::span<const Point> points,
                               const Gradient& gradient,
                               const DrawState& state);
    void stroke_gradient_polyline(std::span<const Point> points,
                                  const Gradient& gradient,
                                  const DrawState& state,
                                  bool closed = false);
    void fill_gradient_rect(Rect rect, const Gradient& gradient,
                            const DrawState& state);
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
    int width_;
    int height_;
    std::vector<std::uint8_t> pixels_;

    [[nodiscard]] bool visible(int x, int y, const DrawState* state) const;
    void composite(int x, int y, Color source, const DrawState& state,
                   bool high_selected, std::uint8_t coverage = 255);
};

} // namespace haiku_remote
