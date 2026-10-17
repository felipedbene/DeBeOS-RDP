#pragma once

// A port of the parts of Anti-Grain Geometry 2.4 that app_server's Painter
// uses to turn a shape into coverage: path storage, curve flattening, the
// ellipse/arc/rounded-rect generators, the stroker, and the scanline
// rasterisers -- including Haiku's own subpixel rasteriser
// (src/servers/app/drawing/Painter/agg_rasterizer_scanline_aa_subpix.h).
//
// Ported from the copies in the DeBeOS tree, headers/libs/agg and
// src/libs/agg/src, which are not quite upstream AGG 2.4: conv_curve skips
// curves with a NaN control point, and the cell rasteriser keeps AGG 2.4's
// missing `return` after splitting a very wide line. Both are reproduced, as
// are every rounding, every epsilon and the order of every floating-point
// operation, because the coverage of an edge pixel depends on all of them.
// tools/painter_oracle/ builds those sources on the host and the render tests
// compare this port against them pixel for pixel.
//
// Vertex sources are materialised: where AGG chains generators that pull one
// vertex at a time, this port runs each stage over a whole VertexList. Every
// stage AGG uses here is a pure function of the sequence it reads, so the
// output is the same; the one place AGG reads a coordinate a source did not
// write (an end_poly vertex) is reproduced by carrying the previous value, see
// Vertex.
//
// AGG 2.4 is (c) 2002-2005 Maxim Shemanarev and licensed "permission to copy,
// use, modify, sell and distribute ... provided this copyright notice appears
// in all copies"; the Haiku additions are MIT.

#include <cstdint>
#include <memory>
#include <vector>

namespace haiku_remote::agg {

// agg_basics.h:368-389
enum : unsigned {
    path_cmd_stop = 0,
    path_cmd_move_to = 1,
    path_cmd_line_to = 2,
    path_cmd_curve3 = 3,
    path_cmd_curve4 = 4,
    path_cmd_end_poly = 0x0F,
    path_cmd_mask = 0x0F,
    path_flags_ccw = 0x10,
    path_flags_cw = 0x20,
    path_flags_close = 0x40,
};

inline bool is_vertex(unsigned c) { return c >= path_cmd_move_to && c < path_cmd_end_poly; }
inline bool is_stop(unsigned c) { return c == path_cmd_stop; }
inline bool is_move_to(unsigned c) { return c == path_cmd_move_to; }
inline bool is_end_poly(unsigned c) { return (c & path_cmd_mask) == path_cmd_end_poly; }
inline bool is_close(unsigned c)
{
    return (c & ~static_cast<unsigned>(path_flags_cw | path_flags_ccw))
        == (path_cmd_end_poly | path_flags_close);
}
inline unsigned get_close_flag(unsigned c) { return c & path_flags_close; }

// Double to int the way the server's hardware does it. AGG's conversions are
// plain C casts (agg_basics.h:188-199), undefined in C++ for a value outside
// int's range and for NaN; DeBeOS runs on arm64, where the conversion
// instruction (FCVTZS) saturates and maps NaN to 0. Wire coordinates are
// unvalidated, so that is the behaviour reproduced -- and it keeps the client
// free of undefined behaviour.
int saturating_int(double v);

// agg_basics.h:188-195 (the build defines neither AGG_FISTP nor AGG_QIFIST).
inline int iround(double v) { return saturating_int((v < 0.0) ? v - 0.5 : v + 0.5); }
inline int uround(double v) { return saturating_int(v + 0.5); }

// One vertex as a source returns it. A source that returns a command without
// writing the coordinates (an ellipse's or a stroke's end_poly) leaves the
// caller's variables as they were; such vertices carry the previous vertex's
// coordinates here, which is what AGG's next stage would read.
struct Vertex {
    double x;
    double y;
    unsigned cmd;
};
using VertexList = std::vector<Vertex>;

// agg::trans_affine, agg_trans_affine.h and agg_trans_affine.cpp.
struct TransAffine {
    double sx = 1.0;
    double shy = 0.0;
    double shx = 0.0;
    double sy = 1.0;
    double tx = 0.0;
    double ty = 0.0;

    static TransAffine translation(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static TransAffine scaling(double s) { return {s, 0, 0, s, 0, 0}; }
    static TransAffine rotation(double a);

    // `*this = *this * m`: apply this, then m (agg_trans_affine.cpp:69-82).
    TransAffine& multiply(const TransAffine& m);
    TransAffine& invert();
    void transform(double* x, double* y) const
    {
        const double tmp = *x;
        *x = tmp * sx + *y * shx + tx;
        *y = tmp * shy + *y * sy + ty;
    }
    [[nodiscard]] double scale() const;
};

// agg::path_storage (path_base<vertex_block_storage<double>>),
// agg_path_storage.h.
class PathStorage {
public:
    void remove_all() { vertices_.clear(); }
    void move_to(double x, double y) { vertices_.push_back({x, y, path_cmd_move_to}); }
    void line_to(double x, double y) { vertices_.push_back({x, y, path_cmd_line_to}); }
    void curve4(double x_ctrl1, double y_ctrl1, double x_ctrl2, double y_ctrl2,
                double x_to, double y_to);
    void arc_to(double rx, double ry, double angle, bool large_arc_flag,
                bool sweep_flag, double x, double y);
    void end_poly(unsigned flags);
    void close_polygon(unsigned flags = 0) { end_poly(path_flags_close | flags); }
    [[nodiscard]] const VertexList& vertices() const { return vertices_; }

private:
    unsigned last_vertex(double* x, double* y) const;
    void join_path(const VertexList& source);

    VertexList vertices_;
};

// agg::conv_curve<VertexSource> with its default curve3/curve4 (curve_div).
VertexList conv_curve(const VertexList& source, double approximation_scale = 1.0);

// agg::ellipse(x, y, rx, ry, num_steps), agg_ellipse.h.
VertexList ellipse(double x, double y, double rx, double ry, unsigned num_steps);

// agg::rounded_rect with rect() and radius(rx, ry) set and normalize_radius()
// *not* called -- Painter never calls it (Painter.cpp:1207-1211).
VertexList rounded_rect(double x1, double y1, double x2, double y2, double rx,
                        double ry);

// agg::bezier_arc(x, y, rx, ry, start_angle, sweep_angle), agg_bezier_arc.cpp.
VertexList bezier_arc(double x, double y, double rx, double ry,
                      double start_angle, double sweep_angle);

// agg_math_stroke.h:29-51
enum class LineCap { butt, square, round };
enum class LineJoin { miter = 0, miter_revert = 1, round = 2, bevel = 3, miter_round = 4 };

struct StrokeStyle {
    double width = 1.0;
    LineCap cap = LineCap::butt;
    LineJoin join = LineJoin::miter;
    double miter_limit = 4.0;
    double approximation_scale = 1.0;
};

// agg::conv_stroke<VertexSource>: conv_adaptor_vcgen + vcgen_stroke +
// math_stroke, with the inner join and inner miter limit left at their
// defaults (inner_miter, 1.01) as Painter leaves them.
VertexList conv_stroke(const VertexList& source, const StrokeStyle& style);

// agg::conv_transform<VertexSource>.
VertexList conv_transform(const VertexList& source, const TransAffine& matrix);

// A run of a scanline. With a packed scanline (scanline_p8 and Haiku's
// scanline_p8_subpix) a negative len is a solid run whose single cover is
// covers[0]; the unpacked scanline_u8 always has len > 0. For the subpixel
// scanline len counts *subpixels*, three per pixel, and covers holds three per
// pixel. len is int16 in AGG (agg_scanline_p.h:50-55) and is kept that way.
struct Span {
    std::int16_t x;
    std::int16_t len;
    const std::uint8_t* covers;
};

class ScanlineSink {
public:
    virtual ~ScanlineSink() = default;
    virtual void render(int y, const Span* spans, unsigned count) = 0;
};

// agg::rasterizer_scanline_aa<rasterizer_sl_clip_int> (subpixel == false) or
// Haiku's agg::rasterizer_scanline_aa_subpix<> (subpixel == true), each with
// AGG's default gamma -- the identity table, because Painter installs none
// (ALIASED_DRAWING is 0, defines.h:43, and the gamma_threshold calls in the
// constructor are compiled out with it, Painter.cpp:228-231) -- and the
// non-zero filling rule, which is DrawState's default (DrawState.cpp:65) and
// which the remote protocol has no message to change.
class Rasterizer {
public:
    explicit Rasterizer(bool subpixel);
    ~Rasterizer();
    Rasterizer(const Rasterizer&) = delete;
    Rasterizer& operator=(const Rasterizer&) = delete;

    void clip_box(double x1, double y1, double x2, double y2);
    void add_path(const VertexList& path);
    // render_scanlines(ras, scanline_p8 / scanline_p8_subpix, renderer).
    void render_packed(ScanlineSink& sink);
    // render_scanlines(ras, scanline_u8, renderer). Not for the subpixel
    // rasteriser, which Painter only ever pairs with packed scanlines.
    void render_unpacked(ScanlineSink& sink);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- gradients: agg_span_gradient.h and what it is built from ------------

// agg_span_gradient.h:30-35
constexpr int gradient_subpixel_shift = 4;
constexpr int gradient_subpixel_scale = 1 << gradient_subpixel_shift;

// agg::fast_sqrt, agg_math.h:261-335, with the tables of agg_sqrt_tables.cpp.
unsigned fast_sqrt(unsigned val);

// agg::dda2_line_interpolator, the forward-adjusted constructor and
// operator++ (agg_dda_line.h:85-209).
class Dda2LineInterpolator {
public:
    Dda2LineInterpolator() = default;
    Dda2LineInterpolator(int y1, int y2, int count);
    void operator++();
    [[nodiscard]] int y() const { return y_; }

private:
    int cnt_ = 1;
    int lft_ = 0;
    int rem_ = 0;
    int mod_ = 0;
    int y_ = 0;
};

// agg::span_interpolator_linear<trans_affine, 8>,
// agg_span_interpolator_linear.h:28-101.
class SpanInterpolatorLinear {
public:
    static constexpr int subpixel_shift = 8;
    static constexpr int subpixel_scale = 1 << subpixel_shift;

    explicit SpanInterpolatorLinear(const TransAffine& trans) : trans_(trans) {}
    void begin(double x, double y, unsigned len);
    void operator++()
    {
        ++li_x_;
        ++li_y_;
    }
    void coordinates(int* x, int* y) const
    {
        *x = li_x_.y();
        *y = li_y_.y();
    }

private:
    TransAffine trans_;
    Dda2LineInterpolator li_x_;
    Dda2LineInterpolator li_y_;
};

// The five gradient functions Painter uses (Painter.cpp:2079-2136):
// gradient_x, gradient_radial, a default-constructed gradient_radial_focus,
// gradient_diamond and gradient_conic.
enum class GradientShape { x, radial, radial_focus, diamond, conic };

// agg::span_gradient<rgba8, span_interpolator_linear<>, GradientF,
// pod_auto_array<rgba8, 256>> (agg_span_gradient.h:44-114), producing the
// colour-table index of each pixel rather than the colour: the table itself
// is Painter's business (_MakeGradient).
class SpanGradient {
public:
    SpanGradient(const TransAffine& transform, GradientShape shape, double d1,
                 double d2, int color_count = 256);
    void generate(int* indices, int x, int y, unsigned len);

private:
    static constexpr int downscale_shift
        = SpanInterpolatorLinear::subpixel_shift - gradient_subpixel_shift;
    [[nodiscard]] int calculate(int x, int y) const;

    SpanInterpolatorLinear interpolator_;
    GradientShape shape_;
    int d1_;
    int d2_;
    int color_count_;
};

} // namespace haiku_remote::agg
