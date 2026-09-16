#pragma once

#include "haiku_remote/types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace haiku_remote {

struct Bitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
};

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
                     const DrawState& state);
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
