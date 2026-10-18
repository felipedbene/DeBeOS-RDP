#include "agg_port.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numbers>

// Source references below are to the DeBeOS tree: headers/libs/agg/<file> for
// headers, src/libs/agg/src/<file> for the .cpp files, and
// src/servers/app/drawing/Painter/<file> for Haiku's own AGG additions.

namespace haiku_remote::agg {

namespace {

constexpr double pi = std::numbers::pi; // agg_basics.h:268

// agg_basics.h:253-258 (poly_subpixel_scale_e)
constexpr int poly_subpixel_shift = 8;
constexpr int poly_subpixel_scale = 1 << poly_subpixel_shift;
constexpr int poly_subpixel_mask = poly_subpixel_scale - 1;

// agg_math.h:30, :34
constexpr double vertex_dist_epsilon = 1e-14;
constexpr double intersection_epsilon = 1.0e-30;

// AGG's int arithmetic on cell coordinates can exceed 32 bits for a line
// thousands of pixels wide (see the dx_limit note in CellRasterizer::line).
// The server's compiled code wraps; C++ calls that undefined, so the products
// that can overflow are formed in 64 bits and wrapped explicitly.
int mul32(int a, int b)
{
    return static_cast<int>(static_cast<std::uint32_t>(
        static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b)));
}

// agg_math.h:37-41
double cross_product(double x1, double y1, double x2, double y2, double x,
                     double y)
{
    return (x - x2) * (y2 - y1) - (y - y2) * (x2 - x1);
}

// agg_math.h:57-62
double calc_distance(double x1, double y1, double x2, double y2)
{
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    return std::sqrt(dx * dx + dy * dy);
}

// agg_math.h:65-70
double calc_sq_distance(double x1, double y1, double x2, double y2)
{
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    return dx * dx + dy * dy;
}

// agg_math.h:136-148
bool calc_intersection(double ax, double ay, double bx, double by, double cx,
                       double cy, double dx, double dy, double* x, double* y)
{
    const double num = (ay - cy) * (dx - cx) - (ax - cx) * (dy - cy);
    const double den = (bx - ax) * (dy - cy) - (by - ay) * (dx - cx);
    if (std::fabs(den) < intersection_epsilon)
        return false;
    const double r = num / den;
    *x = ax + r * (bx - ax);
    *y = ay + r * (by - ay);
    return true;
}

// ---- curve4_div, agg_curves.h:375-470 and agg_curves.cpp:377-610 ----------

constexpr double curve_collinearity_epsilon = 1e-30;
constexpr double curve_angle_tolerance_epsilon = 0.01;
constexpr unsigned curve_recursion_limit = 32;

struct Point {
    double x;
    double y;
};

// agg::curve3_div, agg_curves.h:94-165 and agg_curves.cpp:129-237, with the
// angle tolerance at its default 0 (conv_curve never sets it). FontEngine's
// TrueType glyphs are quadratic, so their outlines reach it through
// conv_curve.
class Curve3Div {
public:
    void approximation_scale(double s) { approximation_scale_ = s; }

    void init(double x1, double y1, double x2, double y2, double x3, double y3)
    {
        points_.clear();
        distance_tolerance_square_ = 0.5 / approximation_scale_;
        distance_tolerance_square_ *= distance_tolerance_square_;
        bezier(x1, y1, x2, y2, x3, y3);
        count_ = 0;
    }

    void reset()
    {
        points_.clear();
        count_ = 0;
    }

    unsigned vertex(double* x, double* y)
    {
        if (count_ >= points_.size())
            return path_cmd_stop;
        const Point& p = points_[count_++];
        *x = p.x;
        *y = p.y;
        return (count_ == 1) ? path_cmd_move_to : path_cmd_line_to;
    }

private:
    void bezier(double x1, double y1, double x2, double y2, double x3, double y3)
    {
        points_.push_back({x1, y1});
        recursive_bezier(x1, y1, x2, y2, x3, y3, 0);
        points_.push_back({x3, y3});
    }

    // The same point cap as Curve4Div's, for the same reason.
    static constexpr std::size_t max_points = std::size_t {1} << 20;

    void recursive_bezier(double x1, double y1, double x2, double y2,
                          double x3, double y3, unsigned level)
    {
        if (level > curve_recursion_limit || points_.size() >= max_points)
            return;

        // Calculate all the mid-points of the line segments
        const double x12 = (x1 + x2) / 2;
        const double y12 = (y1 + y2) / 2;
        const double x23 = (x2 + x3) / 2;
        const double y23 = (y2 + y3) / 2;
        const double x123 = (x12 + x23) / 2;
        const double y123 = (y12 + y23) / 2;

        const double dx = x3 - x1;
        const double dy = y3 - y1;
        double d = std::fabs(((x2 - x3) * dy - (y2 - y3) * dx));
        double da;

        if (d > curve_collinearity_epsilon) {
            // Regular case
            if (d * d <= distance_tolerance_square_ * (dx * dx + dy * dy)) {
                // If the curvature doesn't exceed the distance_tolerance value
                // we tend to finish subdivisions.
                if (angle_tolerance_ < curve_angle_tolerance_epsilon) {
                    points_.push_back({x123, y123});
                    return;
                }

                // Angle & Cusp Condition
                da = std::fabs(std::atan2(y3 - y2, x3 - x2) - std::atan2(y2 - y1, x2 - x1));
                if (da >= pi)
                    da = 2 * pi - da;

                if (da < angle_tolerance_) {
                    // Finally we can stop the recursion
                    points_.push_back({x123, y123});
                    return;
                }
            }
        } else {
            // Collinear case
            da = dx * dx + dy * dy;
            if (da == 0) {
                d = calc_sq_distance(x1, y1, x2, y2);
            } else {
                d = ((x2 - x1) * dx + (y2 - y1) * dy) / da;
                if (d > 0 && d < 1) {
                    // Simple collinear case, 1---2---3
                    // We can leave just two endpoints
                    return;
                }
                if (d <= 0)
                    d = calc_sq_distance(x2, y2, x1, y1);
                else if (d >= 1)
                    d = calc_sq_distance(x2, y2, x3, y3);
                else
                    d = calc_sq_distance(x2, y2, x1 + d * dx, y1 + d * dy);
            }
            if (d < distance_tolerance_square_) {
                points_.push_back({x2, y2});
                return;
            }
        }

        // Continue subdivision
        recursive_bezier(x1, y1, x12, y12, x123, y123, level + 1);
        recursive_bezier(x123, y123, x23, y23, x3, y3, level + 1);
    }

    double approximation_scale_ = 1.0;
    double distance_tolerance_square_ = 0.0;
    double angle_tolerance_ = 0.0;
    std::vector<Point> points_;
    std::size_t count_ = 0;
};

class Curve4Div {
public:
    void approximation_scale(double s) { approximation_scale_ = s; }

    void init(double x1, double y1, double x2, double y2, double x3, double y3,
              double x4, double y4)
    {
        points_.clear();
        distance_tolerance_square_ = 0.5 / approximation_scale_;
        distance_tolerance_square_ *= distance_tolerance_square_;
        bezier(x1, y1, x2, y2, x3, y3, x4, y4);
        count_ = 0;
    }

    void reset()
    {
        points_.clear();
        count_ = 0;
    }

    unsigned vertex(double* x, double* y)
    {
        if (count_ >= points_.size())
            return path_cmd_stop;
        const Point& p = points_[count_++];
        *x = p.x;
        *y = p.y;
        return (count_ == 1) ? path_cmd_move_to : path_cmd_line_to;
    }

private:
    void bezier(double x1, double y1, double x2, double y2, double x3,
                double y3, double x4, double y4)
    {
        points_.push_back({x1, y1});
        recursive_bezier(x1, y1, x2, y2, x3, y3, x4, y4, 0);
        points_.push_back({x4, y4});
    }

    // curve_recursion_limit lets one curve subdivide into 2^32 points, which
    // a curve whose control points arrived as 1e30 does -- in the server too,
    // which then exhausts memory. The client stops adding points at 2^20, a
    // limit no curve that fits on a 16384px screen at 0.5px tolerance
    // approaches.
    static constexpr std::size_t max_points = std::size_t {1} << 20;

    void recursive_bezier(double x1, double y1, double x2, double y2,
                          double x3, double y3, double x4, double y4,
                          unsigned level)
    {
        if (level > curve_recursion_limit || points_.size() >= max_points)
            return;

        const double x12 = (x1 + x2) / 2;
        const double y12 = (y1 + y2) / 2;
        const double x23 = (x2 + x3) / 2;
        const double y23 = (y2 + y3) / 2;
        const double x34 = (x3 + x4) / 2;
        const double y34 = (y3 + y4) / 2;
        const double x123 = (x12 + x23) / 2;
        const double y123 = (y12 + y23) / 2;
        const double x234 = (x23 + x34) / 2;
        const double y234 = (y23 + y34) / 2;
        const double x1234 = (x123 + x234) / 2;
        const double y1234 = (y123 + y234) / 2;

        const double dx = x4 - x1;
        const double dy = y4 - y1;

        double d2 = std::fabs(((x2 - x4) * dy - (y2 - y4) * dx));
        double d3 = std::fabs(((x3 - x4) * dy - (y3 - y4) * dx));
        double da1;
        double da2;
        double k;

        switch ((int(d2 > curve_collinearity_epsilon) << 1)
                + int(d3 > curve_collinearity_epsilon)) {
        case 0:
            k = dx * dx + dy * dy;
            if (k == 0) {
                d2 = calc_sq_distance(x1, y1, x2, y2);
                d3 = calc_sq_distance(x4, y4, x3, y3);
            } else {
                k = 1 / k;
                da1 = x2 - x1;
                da2 = y2 - y1;
                d2 = k * (da1 * dx + da2 * dy);
                da1 = x3 - x1;
                da2 = y3 - y1;
                d3 = k * (da1 * dx + da2 * dy);
                if (d2 > 0 && d2 < 1 && d3 > 0 && d3 < 1)
                    return;
                if (d2 <= 0)
                    d2 = calc_sq_distance(x2, y2, x1, y1);
                else if (d2 >= 1)
                    d2 = calc_sq_distance(x2, y2, x4, y4);
                else
                    d2 = calc_sq_distance(x2, y2, x1 + d2 * dx, y1 + d2 * dy);

                if (d3 <= 0)
                    d3 = calc_sq_distance(x3, y3, x1, y1);
                else if (d3 >= 1)
                    d3 = calc_sq_distance(x3, y3, x4, y4);
                else
                    d3 = calc_sq_distance(x3, y3, x1 + d3 * dx, y1 + d3 * dy);
            }
            if (d2 > d3) {
                if (d2 < distance_tolerance_square_) {
                    points_.push_back({x2, y2});
                    return;
                }
            } else {
                if (d3 < distance_tolerance_square_) {
                    points_.push_back({x3, y3});
                    return;
                }
            }
            break;

        case 1:
            if (d3 * d3 <= distance_tolerance_square_ * (dx * dx + dy * dy)) {
                if (angle_tolerance_ < curve_angle_tolerance_epsilon) {
                    points_.push_back({x23, y23});
                    return;
                }
                da1 = std::fabs(std::atan2(y4 - y3, x4 - x3)
                                - std::atan2(y3 - y2, x3 - x2));
                if (da1 >= pi)
                    da1 = 2 * pi - da1;
                if (da1 < angle_tolerance_) {
                    points_.push_back({x2, y2});
                    points_.push_back({x3, y3});
                    return;
                }
                if (cusp_limit_ != 0.0 && da1 > cusp_limit_) {
                    points_.push_back({x3, y3});
                    return;
                }
            }
            break;

        case 2:
            if (d2 * d2 <= distance_tolerance_square_ * (dx * dx + dy * dy)) {
                if (angle_tolerance_ < curve_angle_tolerance_epsilon) {
                    points_.push_back({x23, y23});
                    return;
                }
                da1 = std::fabs(std::atan2(y3 - y2, x3 - x2)
                                - std::atan2(y2 - y1, x2 - x1));
                if (da1 >= pi)
                    da1 = 2 * pi - da1;
                if (da1 < angle_tolerance_) {
                    points_.push_back({x2, y2});
                    points_.push_back({x3, y3});
                    return;
                }
                if (cusp_limit_ != 0.0 && da1 > cusp_limit_) {
                    points_.push_back({x2, y2});
                    return;
                }
            }
            break;

        case 3:
            if ((d2 + d3) * (d2 + d3)
                <= distance_tolerance_square_ * (dx * dx + dy * dy)) {
                if (angle_tolerance_ < curve_angle_tolerance_epsilon) {
                    points_.push_back({x23, y23});
                    return;
                }
                k = std::atan2(y3 - y2, x3 - x2);
                da1 = std::fabs(k - std::atan2(y2 - y1, x2 - x1));
                da2 = std::fabs(std::atan2(y4 - y3, x4 - x3) - k);
                if (da1 >= pi)
                    da1 = 2 * pi - da1;
                if (da2 >= pi)
                    da2 = 2 * pi - da2;
                if (da1 + da2 < angle_tolerance_) {
                    points_.push_back({x23, y23});
                    return;
                }
                if (cusp_limit_ != 0.0) {
                    if (da1 > cusp_limit_) {
                        points_.push_back({x2, y2});
                        return;
                    }
                    if (da2 > cusp_limit_) {
                        points_.push_back({x3, y3});
                        return;
                    }
                }
            }
            break;
        }

        recursive_bezier(x1, y1, x12, y12, x123, y123, x1234, y1234, level + 1);
        recursive_bezier(x1234, y1234, x234, y234, x34, y34, x4, y4, level + 1);
    }

    double approximation_scale_ = 1.0;
    double distance_tolerance_square_ = 0.0;
    // Neither is ever set by Painter, so both keep curve4_div's 0.0
    // (agg_curves.h:378-383).
    double angle_tolerance_ = 0.0;
    double cusp_limit_ = 0.0;
    std::size_t count_ = 0;
    std::vector<Point> points_;
};

// ---- arc, agg_arc.h and agg_arc.cpp -------------------------------------

class Arc {
public:
    // arc::init with the header's default ccw = true (agg_arc.h:40-45).
    void init(double x, double y, double rx, double ry, double a1, double a2)
    {
        x_ = x;
        y_ = y;
        rx_ = rx;
        ry_ = ry;
        normalize(a1, a2, true);
    }

    void rewind()
    {
        path_cmd_ = path_cmd_move_to;
        angle_ = start_;
    }

    unsigned vertex(double* x, double* y)
    {
        if (is_stop(path_cmd_))
            return path_cmd_stop;
        if ((angle_ < end_ - da_ / 4) != ccw_) {
            *x = x_ + std::cos(end_) * rx_;
            *y = y_ + std::sin(end_) * ry_;
            path_cmd_ = path_cmd_stop;
            return path_cmd_line_to;
        }
        *x = x_ + std::cos(angle_) * rx_;
        *y = y_ + std::sin(angle_) * ry_;
        angle_ += da_;
        const unsigned pf = path_cmd_;
        path_cmd_ = path_cmd_line_to;
        return pf;
    }

private:
    void normalize(double a1, double a2, bool ccw)
    {
        const double ra = (std::fabs(rx_) + std::fabs(ry_)) / 2;
        da_ = std::acos(ra / (ra + 0.125 / scale_)) * 2;
        if (ccw) {
            while (a2 < a1)
                a2 += pi * 2.0;
        } else {
            while (a1 < a2)
                a1 += pi * 2.0;
            da_ = -da_;
        }
        // A corner radius off the wire can be 1e30, which makes da so small
        // that the arc is quadrillions of points long; the server would never
        // finish. The client caps a corner at 2^16 points by widening da,
        // which no radius under about 1e8 pixels reaches.
        if (std::fabs(a2 - a1) > 65536.0 * std::fabs(da_))
            da_ = (a2 - a1) / 65536.0;
        ccw_ = ccw;
        start_ = a1;
        end_ = a2;
    }

    double x_ = 0;
    double y_ = 0;
    double rx_ = 0;
    double ry_ = 0;
    double angle_ = 0;
    double start_ = 0;
    double end_ = 0;
    double scale_ = 1.0; // agg_arc.h:36, never changed by rounded_rect
    double da_ = 0;
    bool ccw_ = true;
    unsigned path_cmd_ = path_cmd_stop;
};

// ---- bezier_arc, agg_bezier_arc.h and agg_bezier_arc.cpp ----------------

constexpr double bezier_arc_angle_epsilon = 0.01;

void arc_to_bezier(double cx, double cy, double rx, double ry,
                   double start_angle, double sweep_angle, double* curve)
{
    const double x0 = std::cos(sweep_angle / 2.0);
    const double y0 = std::sin(sweep_angle / 2.0);
    const double tx = (1.0 - x0) * 4.0 / 3.0;
    const double ty = y0 - tx * x0 / y0;
    double px[4];
    double py[4];
    px[0] = x0;
    py[0] = -y0;
    px[1] = x0 + tx;
    py[1] = -ty;
    px[2] = x0 + tx;
    py[2] = ty;
    px[3] = x0;
    py[3] = y0;

    const double sn = std::sin(start_angle + sweep_angle / 2.0);
    const double cs = std::cos(start_angle + sweep_angle / 2.0);

    for (unsigned i = 0; i < 4; i++) {
        curve[i * 2] = cx + rx * (px[i] * cs - py[i] * sn);
        curve[i * 2 + 1] = cy + ry * (px[i] * sn + py[i] * cs);
    }
}

struct BezierArc {
    unsigned num_vertices = 0;
    double vertices[26] {};
    unsigned cmd = path_cmd_line_to;

    void init(double x, double y, double rx, double ry, double start_angle,
              double sweep_angle)
    {
        start_angle = std::fmod(start_angle, 2.0 * pi);
        if (sweep_angle >= 2.0 * pi)
            sweep_angle = 2.0 * pi;
        if (sweep_angle <= -2.0 * pi)
            sweep_angle = -2.0 * pi;

        if (std::fabs(sweep_angle) < 1e-10) {
            num_vertices = 4;
            cmd = path_cmd_line_to;
            vertices[0] = x + rx * std::cos(start_angle);
            vertices[1] = y + ry * std::sin(start_angle);
            vertices[2] = x + rx * std::cos(start_angle + sweep_angle);
            vertices[3] = y + ry * std::sin(start_angle + sweep_angle);
            return;
        }

        double total_sweep = 0.0;
        double local_sweep = 0.0;
        double prev_sweep;
        num_vertices = 2;
        cmd = path_cmd_curve4;
        bool done = false;
        do {
            if (sweep_angle < 0.0) {
                prev_sweep = total_sweep;
                local_sweep = -pi * 0.5;
                total_sweep -= pi * 0.5;
                if (total_sweep <= sweep_angle + bezier_arc_angle_epsilon) {
                    local_sweep = sweep_angle - prev_sweep;
                    done = true;
                }
            } else {
                prev_sweep = total_sweep;
                local_sweep = pi * 0.5;
                total_sweep += pi * 0.5;
                if (total_sweep >= sweep_angle - bezier_arc_angle_epsilon) {
                    local_sweep = sweep_angle - prev_sweep;
                    done = true;
                }
            }

            arc_to_bezier(x, y, rx, ry, start_angle, local_sweep,
                          vertices + num_vertices - 2);

            num_vertices += 6;
            start_angle += local_sweep;
        } while (!done && num_vertices < 26);
    }

    // bezier_arc::vertex: the first vertex is a move_to, the rest `cmd`.
    [[nodiscard]] VertexList vertex_list() const
    {
        VertexList out;
        for (unsigned v = 0; v < num_vertices; v += 2)
            out.push_back({vertices[v], vertices[v + 1],
                           v == 0 ? static_cast<unsigned>(path_cmd_move_to) : cmd});
        return out;
    }
};

// bezier_arc_svg::init, agg_bezier_arc.cpp:137-255
bool bezier_arc_svg(double x0, double y0, double rx, double ry, double angle,
                    bool large_arc_flag, bool sweep_flag, double x2,
                    double y2, BezierArc& arc)
{
    bool radii_ok = true;

    if (rx < 0.0)
        rx = -rx;
    if (ry < 0.0)
        ry = -ry;

    const double dx2 = (x0 - x2) / 2.0;
    const double dy2 = (y0 - y2) / 2.0;

    const double cos_a = std::cos(angle);
    const double sin_a = std::sin(angle);

    const double x1 = cos_a * dx2 + sin_a * dy2;
    const double y1 = -sin_a * dx2 + cos_a * dy2;

    double prx = rx * rx;
    double pry = ry * ry;
    const double px1 = x1 * x1;
    const double py1 = y1 * y1;

    const double radii_check = px1 / prx + py1 / pry;
    if (radii_check > 1.0) {
        rx = std::sqrt(radii_check) * rx;
        ry = std::sqrt(radii_check) * ry;
        prx = rx * rx;
        pry = ry * ry;
        if (radii_check > 10.0)
            radii_ok = false;
    }

    double sign = (large_arc_flag == sweep_flag) ? -1.0 : 1.0;
    const double sq = (prx * pry - prx * py1 - pry * px1) / (prx * py1 + pry * px1);
    const double coef = sign * std::sqrt((sq < 0) ? 0 : sq);
    const double cx1 = coef * ((rx * y1) / ry);
    const double cy1 = coef * -((ry * x1) / rx);

    const double sx2 = (x0 + x2) / 2.0;
    const double sy2 = (y0 + y2) / 2.0;
    const double cx = sx2 + (cos_a * cx1 - sin_a * cy1);
    const double cy = sy2 + (sin_a * cx1 + cos_a * cy1);

    const double ux = (x1 - cx1) / rx;
    const double uy = (y1 - cy1) / ry;
    const double vx = (-x1 - cx1) / rx;
    const double vy = (-y1 - cy1) / ry;
    double p;
    double n;

    n = std::sqrt(ux * ux + uy * uy);
    p = ux;
    sign = (uy < 0) ? -1.0 : 1.0;
    double v = p / n;
    if (v < -1.0)
        v = -1.0;
    if (v > 1.0)
        v = 1.0;
    const double start_angle = sign * std::acos(v);

    n = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    p = ux * vx + uy * vy;
    sign = (ux * vy - uy * vx < 0) ? -1.0 : 1.0;
    v = p / n;
    if (v < -1.0)
        v = -1.0;
    if (v > 1.0)
        v = 1.0;
    double sweep_angle = sign * std::acos(v);
    if (!sweep_flag && sweep_angle > 0) {
        sweep_angle -= pi * 2.0;
    } else if (sweep_flag && sweep_angle < 0) {
        sweep_angle += pi * 2.0;
    }

    arc.init(0.0, 0.0, rx, ry, start_angle, sweep_angle);
    TransAffine mtx = TransAffine::rotation(angle);
    mtx.multiply(TransAffine::translation(cx, cy));

    for (unsigned i = 2; i < arc.num_vertices - 2; i += 2)
        mtx.transform(arc.vertices + i, arc.vertices + i + 1);

    arc.vertices[0] = x0;
    arc.vertices[1] = y0;
    if (arc.num_vertices > 2) {
        arc.vertices[arc.num_vertices - 2] = x2;
        arc.vertices[arc.num_vertices - 1] = y2;
    }
    return radii_ok;
}

// ---- vertex_dist and vertex_sequence, agg_vertex_sequence.h -------------

struct VertexDist {
    double x;
    double y;
    double dist = 0.0;

    // vertex_dist::operator(), agg_vertex_sequence.h:145-150
    bool operator()(const VertexDist& val)
    {
        const bool ret = (dist = calc_distance(x, y, val.x, val.y))
            > vertex_dist_epsilon;
        if (!ret)
            dist = 1.0 / vertex_dist_epsilon;
        return ret;
    }
};

class VertexSequence {
public:
    [[nodiscard]] std::size_t size() const { return v_.size(); }
    VertexDist& operator[](std::size_t i) { return v_[i]; }
    void remove_all() { v_.clear(); }
    // pod_bvector::remove_last, agg_array.h:663-666: `if(m_size) --m_size;`
    void remove_last()
    {
        if (!v_.empty())
            v_.pop_back();
    }

    // vertex_sequence::add, agg_vertex_sequence.h:80-91
    void add(const VertexDist& val)
    {
        if (v_.size() > 1) {
            if (!v_[v_.size() - 2](v_[v_.size() - 1]))
                remove_last();
        }
        v_.push_back(val);
    }

    // :95-100
    void modify_last(const VertexDist& val)
    {
        remove_last();
        add(val);
    }

    // :105-124
    void close(bool closed)
    {
        while (v_.size() > 1) {
            if (v_[v_.size() - 2](v_[v_.size() - 1]))
                break;
            const VertexDist t = v_[v_.size() - 1];
            remove_last();
            modify_last(t);
        }
        if (closed) {
            while (v_.size() > 1) {
                if (v_[v_.size() - 1](v_[0]))
                    break;
                remove_last();
            }
        }
    }

    // vertex_sequence::prev/curr/next (pod_bvector), agg_array.h:406-434
    VertexDist& prev(std::size_t idx) { return v_[(idx + v_.size() - 1) % v_.size()]; }
    VertexDist& curr(std::size_t idx) { return v_[idx]; }
    VertexDist& next(std::size_t idx) { return v_[(idx + 1) % v_.size()]; }

private:
    std::vector<VertexDist> v_;
};

// ---- math_stroke, agg_math_stroke.h -------------------------------------

// int(steps) for the number of points in a round cap or join
// (agg_math_stroke.h:159-198, :290-348). The step angle shrinks as the pen
// grows, and a pen of 1e30 off the wire asks for about 1e15 points -- the
// server would never finish. The client caps an arc at 2^16 points, reached
// only by a pen wider than roughly 4e8 pixels; NaN gives none.
int arc_steps(double steps)
{
    if (!(steps > 0))
        return 0;
    return steps >= 65536.0 ? 65536 : int(steps);
}

class MathStroke {
public:
    explicit MathStroke(const StrokeStyle& style)
        : miter_limit_(style.miter_limit)
        , approx_scale_(style.approximation_scale)
        , line_cap_(style.cap)
        , line_join_(style.join)
    {
        // math_stroke::width, agg_math_stroke.h:135-149
        width_ = style.width * 0.5;
        if (width_ < 0) {
            width_abs_ = -width_;
            width_sign_ = -1;
        } else {
            width_abs_ = width_;
            width_sign_ = 1;
        }
    }

    void calc_cap(std::vector<Point>& out, const VertexDist& v0,
                  const VertexDist& v1, double len) const
    {
        out.clear();

        double dx1 = (v1.y - v0.y) / len;
        double dy1 = (v1.x - v0.x) / len;
        double dx2 = 0;
        double dy2 = 0;

        dx1 *= width_;
        dy1 *= width_;

        if (line_cap_ != LineCap::round) {
            if (line_cap_ == LineCap::square) {
                dx2 = dy1 * width_sign_;
                dy2 = dx1 * width_sign_;
            }
            out.push_back({v0.x - dx1 - dx2, v0.y + dy1 - dy2});
            out.push_back({v0.x + dx1 - dx2, v0.y - dy1 - dy2});
        } else {
            double da = std::acos(width_abs_ / (width_abs_ + 0.125 / approx_scale_)) * 2;
            double a1;
            const int n = arc_steps(pi / da);

            da = pi / (n + 1);
            out.push_back({v0.x - dx1, v0.y + dy1});
            if (width_sign_ > 0) {
                a1 = std::atan2(dy1, -dx1);
                a1 += da;
                for (int i = 0; i < n; i++) {
                    out.push_back({v0.x + std::cos(a1) * width_,
                                   v0.y + std::sin(a1) * width_});
                    a1 += da;
                }
            } else {
                a1 = std::atan2(-dy1, dx1);
                a1 -= da;
                for (int i = 0; i < n; i++) {
                    out.push_back({v0.x + std::cos(a1) * width_,
                                   v0.y + std::sin(a1) * width_});
                    a1 -= da;
                }
            }
            out.push_back({v0.x + dx1, v0.y - dy1});
        }
    }

    void calc_join(std::vector<Point>& out, const VertexDist& v0,
                   const VertexDist& v1, const VertexDist& v2, double len1,
                   double len2) const
    {
        double d;
        const double dx1 = width_ * (v1.y - v0.y) / len1;
        const double dy1 = width_ * (v1.x - v0.x) / len1;
        const double dx2 = width_ * (v2.y - v1.y) / len2;
        const double dy2 = width_ * (v2.x - v1.x) / len2;

        out.clear();

        const double cp = cross_product(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
        if (cp != 0 && (cp > 0) == (width_ > 0)) {
            // Inner join. Painter leaves inner_join at inner_miter
            // (math_stroke(), agg_math_stroke.h:131), so only that arm is
            // reachable.
            double limit = ((len1 < len2) ? len1 : len2) / width_abs_;
            if (limit < inner_miter_limit_)
                limit = inner_miter_limit_;
            calc_miter(out, v0, v1, v2, dx1, dy1, dx2, dy2,
                       LineJoin::miter_revert, limit);
        } else {
            // Outer join
            LineJoin lj = line_join_;
            if (line_join_ == LineJoin::round || line_join_ == LineJoin::bevel) {
                // This is an optimization that reduces the number of points
                // in cases of almost collinear segments.
                const double dx = (dx1 + dx2) / 2;
                const double dy = (dy1 + dy2) / 2;
                d = width_abs_ - std::sqrt(dx * dx + dy * dy);
                if (d < 0.0625 / approx_scale_)
                    lj = LineJoin::miter;
            }

            switch (lj) {
            case LineJoin::miter:
            case LineJoin::miter_revert:
            case LineJoin::miter_round:
                calc_miter(out, v0, v1, v2, dx1, dy1, dx2, dy2, lj, miter_limit_);
                break;
            case LineJoin::round:
                calc_arc(out, v1.x, v1.y, dx1, -dy1, dx2, -dy2);
                break;
            default: // Bevel join
                out.push_back({v1.x + dx1, v1.y - dy1});
                out.push_back({v1.x + dx2, v1.y - dy2});
                break;
            }
        }
    }

private:
    void calc_arc(std::vector<Point>& out, double x, double y, double dx1,
                  double dy1, double dx2, double dy2) const
    {
        double a1 = std::atan2(dy1 * width_sign_, dx1 * width_sign_);
        double a2 = std::atan2(dy2 * width_sign_, dx2 * width_sign_);
        double da = std::acos(width_abs_ / (width_abs_ + 0.125 / approx_scale_)) * 2;
        int n;

        out.push_back({x + dx1, y + dy1});
        if (width_sign_ > 0) {
            if (a1 > a2)
                a2 += 2 * pi;
            n = arc_steps((a2 - a1) / da);
            da = (a2 - a1) / (n + 1);
            a1 += da;
            for (int i = 0; i < n; i++) {
                out.push_back({x + std::cos(a1) * width_, y + std::sin(a1) * width_});
                a1 += da;
            }
        } else {
            if (a1 < a2)
                a2 -= 2 * pi;
            n = arc_steps((a1 - a2) / da);
            da = (a1 - a2) / (n + 1);
            a1 -= da;
            for (int i = 0; i < n; i++) {
                out.push_back({x + std::cos(a1) * width_, y + std::sin(a1) * width_});
                a1 -= da;
            }
        }
        out.push_back({x + dx2, y + dy2});
    }

    void calc_miter(std::vector<Point>& out, const VertexDist& v0,
                    const VertexDist& v1, const VertexDist& v2, double dx1,
                    double dy1, double dx2, double dy2, LineJoin lj,
                    double ml) const
    {
        double xi = v1.x;
        double yi = v1.y;
        bool miter_limit_exceeded = true; // Assume the worst

        if (calc_intersection(v0.x + dx1, v0.y - dy1, v1.x + dx1, v1.y - dy1,
                              v1.x + dx2, v1.y - dy2, v2.x + dx2, v2.y - dy2,
                              &xi, &yi)) {
            // Calculation of the intersection succeeded
            const double d1 = calc_distance(v1.x, v1.y, xi, yi);
            const double lim = width_abs_ * ml;
            if (d1 <= lim) {
                // Inside the miter limit
                out.push_back({xi, yi});
                miter_limit_exceeded = false;
            }
        } else {
            // Calculation of the intersection failed, most probably the three
            // points lie one straight line.
            const double x2 = v1.x + dx1;
            const double y2 = v1.y - dy1;
            if (((x2 - v0.x) * dy1 - (v0.y - y2) * dx1 < 0.0)
                != ((x2 - v2.x) * dy1 - (v2.y - y2) * dx1 < 0.0)) {
                // This case means that the next segment continues the
                // previous one (straight line).
                out.push_back({v1.x + dx1, v1.y - dy1});
                miter_limit_exceeded = false;
            }
        }

        if (miter_limit_exceeded) {
            // Miter limit exceeded
            switch (lj) {
            case LineJoin::miter_revert:
                out.push_back({v1.x + dx1, v1.y - dy1});
                out.push_back({v1.x + dx2, v1.y - dy2});
                break;
            case LineJoin::miter_round:
                calc_arc(out, v1.x, v1.y, dx1, -dy1, dx2, -dy2);
                break;
            default:
                // If no miter-revert, calculate new dx1, dy1, dx2, dy2
                ml *= width_sign_;
                out.push_back({v1.x + dx1 + dy1 * ml, v1.y - dy1 + dx1 * ml});
                out.push_back({v1.x + dx2 - dy2 * ml, v1.y - dy2 - dx2 * ml});
                break;
            }
        }
    }

    double width_ = 0.5;
    double width_abs_ = 0.5;
    int width_sign_ = 1;
    double miter_limit_ = 4.0;
    double inner_miter_limit_ = 1.01; // math_stroke(), agg_math_stroke.h:127
    double approx_scale_ = 1.0;
    LineCap line_cap_ = LineCap::butt;
    LineJoin line_join_ = LineJoin::miter;
};

// ---- vcgen_stroke, agg_vcgen_stroke.h and agg_vcgen_stroke.cpp ----------

class VcgenStroke {
public:
    explicit VcgenStroke(const StrokeStyle& style) : stroker_(style) {}

    void remove_all()
    {
        src_vertices_.remove_all();
        closed_ = 0;
        status_ = initial;
    }

    void add_vertex(double x, double y, unsigned cmd)
    {
        status_ = initial;
        if (is_move_to(cmd)) {
            src_vertices_.modify_last({x, y});
        } else {
            if (is_vertex(cmd))
                src_vertices_.add({x, y});
            else
                closed_ = get_close_flag(cmd);
        }
    }

    void rewind()
    {
        if (status_ == initial) {
            src_vertices_.close(closed_ != 0);
            // shorten_path(m_src_vertices, m_shorten, m_closed) is a no-op:
            // Painter never sets shorten, which stays 0.0.
            if (src_vertices_.size() < 3)
                closed_ = 0;
        }
        status_ = ready;
        src_vertex_ = 0;
        out_vertex_ = 0;
    }

    unsigned vertex(double* x, double* y)
    {
        unsigned cmd = path_cmd_line_to;
        while (!is_stop(cmd)) {
            switch (status_) {
            case initial:
                rewind();
                [[fallthrough]];

            case ready:
                if (src_vertices_.size() < 2 + unsigned(closed_ != 0)) {
                    cmd = path_cmd_stop;
                    break;
                }
                status_ = closed_ ? outline1 : cap1;
                cmd = path_cmd_move_to;
                src_vertex_ = 0;
                out_vertex_ = 0;
                break;

            case cap1:
                stroker_.calc_cap(out_vertices_, src_vertices_[0],
                                  src_vertices_[1], src_vertices_[0].dist);
                src_vertex_ = 1;
                prev_status_ = outline1;
                status_ = out_vertices;
                out_vertex_ = 0;
                break;

            case cap2:
                stroker_.calc_cap(out_vertices_,
                                  src_vertices_[src_vertices_.size() - 1],
                                  src_vertices_[src_vertices_.size() - 2],
                                  src_vertices_[src_vertices_.size() - 2].dist);
                prev_status_ = outline2;
                status_ = out_vertices;
                out_vertex_ = 0;
                break;

            case outline1:
                if (closed_) {
                    if (src_vertex_ >= src_vertices_.size()) {
                        prev_status_ = close_first;
                        status_ = end_poly1;
                        break;
                    }
                } else {
                    if (src_vertex_ >= src_vertices_.size() - 1) {
                        status_ = cap2;
                        break;
                    }
                }
                stroker_.calc_join(out_vertices_,
                                   src_vertices_.prev(src_vertex_),
                                   src_vertices_.curr(src_vertex_),
                                   src_vertices_.next(src_vertex_),
                                   src_vertices_.prev(src_vertex_).dist,
                                   src_vertices_.curr(src_vertex_).dist);
                ++src_vertex_;
                prev_status_ = status_;
                status_ = out_vertices;
                out_vertex_ = 0;
                break;

            case close_first:
                status_ = outline2;
                cmd = path_cmd_move_to;
                [[fallthrough]];

            case outline2:
                if (src_vertex_ <= unsigned(closed_ == 0)) {
                    status_ = end_poly2;
                    prev_status_ = stop;
                    break;
                }

                --src_vertex_;
                stroker_.calc_join(out_vertices_,
                                   src_vertices_.next(src_vertex_),
                                   src_vertices_.curr(src_vertex_),
                                   src_vertices_.prev(src_vertex_),
                                   src_vertices_.curr(src_vertex_).dist,
                                   src_vertices_.prev(src_vertex_).dist);

                prev_status_ = status_;
                status_ = out_vertices;
                out_vertex_ = 0;
                break;

            case out_vertices:
                if (out_vertex_ >= out_vertices_.size()) {
                    status_ = prev_status_;
                } else {
                    const Point& c = out_vertices_[out_vertex_++];
                    *x = c.x;
                    *y = c.y;
                    return cmd;
                }
                break;

            case end_poly1:
                status_ = prev_status_;
                return path_cmd_end_poly | path_flags_close | path_flags_ccw;

            case end_poly2:
                status_ = prev_status_;
                return path_cmd_end_poly | path_flags_close | path_flags_cw;

            case stop:
                cmd = path_cmd_stop;
                break;
            }
        }
        return cmd;
    }

private:
    enum Status {
        initial,
        ready,
        cap1,
        cap2,
        outline1,
        close_first,
        outline2,
        out_vertices,
        end_poly1,
        end_poly2,
        stop,
    };

    MathStroke stroker_;
    VertexSequence src_vertices_;
    std::vector<Point> out_vertices_;
    unsigned closed_ = 0;
    Status status_ = initial;
    Status prev_status_ = initial;
    std::size_t src_vertex_ = 0;
    std::size_t out_vertex_ = 0;
};

// agg::vcgen_contour, agg_vcgen_contour.h and agg_vcgen_contour.cpp, as
// AGGTextRenderer configures it: auto_detect_orientation(false)
// (AGGTextRenderer.cpp:63) and every other stroker setting at its default.
class VcgenContour {
public:
    explicit VcgenContour(double width) : width_(width), stroker_(StrokeStyle {width}) {}

    // vcgen_contour::remove_all, agg_vcgen_contour.cpp:42-48
    void remove_all()
    {
        src_vertices_.remove_all();
        closed_ = 0;
        orientation_ = 0;
        status_ = initial;
    }

    // vcgen_contour::add_vertex, :51-74
    void add_vertex(double x, double y, unsigned cmd)
    {
        status_ = initial;
        if (is_move_to(cmd)) {
            src_vertices_.modify_last({x, y});
        } else if (is_vertex(cmd)) {
            src_vertices_.add({x, y});
        } else if (is_end_poly(cmd)) {
            closed_ = get_close_flag(cmd);
            if (orientation_ == 0)
                orientation_ = cmd & (path_flags_cw | path_flags_ccw);
        }
    }

    // vcgen_contour::rewind, :77-96. Orientation is never auto-detected, so
    // the stroker keeps the width it was given unless the source's end_poly
    // carried an orientation flag (FontEngine's never does).
    void rewind()
    {
        if (status_ == initial) {
            src_vertices_.close(true);
            if (orientation_ != 0) {
                stroker_ = MathStroke(StrokeStyle {
                    (orientation_ & path_flags_ccw) != 0 ? width_ : -width_});
            }
        }
        status_ = ready;
        src_vertex_ = 0;
    }

    // vcgen_contour::vertex, :99-155
    unsigned vertex(double* x, double* y)
    {
        unsigned cmd = path_cmd_line_to;
        while (!is_stop(cmd)) {
            switch (status_) {
            case initial:
                rewind();
                [[fallthrough]];

            case ready:
                if (src_vertices_.size() < 2 + unsigned(closed_ != 0)) {
                    cmd = path_cmd_stop;
                    break;
                }
                status_ = outline;
                cmd = path_cmd_move_to;
                src_vertex_ = 0;
                out_vertex_ = 0;
                [[fallthrough]];

            case outline:
                if (src_vertex_ >= src_vertices_.size()) {
                    status_ = end_poly;
                    break;
                }
                stroker_.calc_join(out_vertices_, src_vertices_.prev(src_vertex_),
                                   src_vertices_.curr(src_vertex_),
                                   src_vertices_.next(src_vertex_),
                                   src_vertices_.prev(src_vertex_).dist,
                                   src_vertices_.curr(src_vertex_).dist);
                ++src_vertex_;
                status_ = out_vertices;
                out_vertex_ = 0;
                [[fallthrough]];

            case out_vertices:
                if (out_vertex_ >= out_vertices_.size()) {
                    status_ = outline;
                } else {
                    const Point& c = out_vertices_[out_vertex_++];
                    *x = c.x;
                    *y = c.y;
                    return cmd;
                }
                break;

            case end_poly:
                if (!closed_)
                    return path_cmd_stop;
                status_ = stop;
                return path_cmd_end_poly | path_flags_close | path_flags_ccw;

            case stop:
                return path_cmd_stop;
            }
        }
        return cmd;
    }

private:
    enum Status { initial, ready, outline, out_vertices, end_poly, stop };

    double width_ = 0;
    MathStroke stroker_;
    VertexSequence src_vertices_;
    std::vector<Point> out_vertices_;
    Status status_ = initial;
    std::size_t src_vertex_ = 0;
    std::size_t out_vertex_ = 0;
    unsigned closed_ = 0;
    unsigned orientation_ = 0;
};


// A cursor over a materialised source: AGG's `vs.vertex(&x, &y)`, which writes
// the coordinates of every vertex it returns and leaves them untouched at the
// end (path_cmd_stop).
class Source {
public:
    explicit Source(const VertexList& list) : list_(list) {}
    unsigned vertex(double* x, double* y)
    {
        if (next_ >= list_.size())
            return path_cmd_stop;
        const Vertex& v = list_[next_++];
        *x = v.x;
        *y = v.y;
        return v.cmd;
    }

private:
    const VertexList& list_;
    std::size_t next_ = 0;
};

// ---- the cell rasteriser, agg_rasterizer_cells_aa.h ---------------------

struct Cell {
    int x;
    int y;
    int cover;
    int area;
};

class CellRasterizer {
public:
    void reset()
    {
        cells_.clear();
        curr_ = initial_cell();
        sorted_ = false;
        min_x_ = 0x7FFFFFFF;
        min_y_ = 0x7FFFFFFF;
        max_x_ = -0x7FFFFFFF;
        max_y_ = -0x7FFFFFFF;
    }

    [[nodiscard]] int min_x() const { return min_x_; }
    [[nodiscard]] int min_y() const { return min_y_; }
    [[nodiscard]] int max_x() const { return max_x_; }
    [[nodiscard]] int max_y() const { return max_y_; }
    [[nodiscard]] bool sorted() const { return sorted_; }
    [[nodiscard]] std::size_t total_cells() const { return cells_.size(); }

    [[nodiscard]] unsigned scanline_num_cells(int y) const
    {
        return rows_[static_cast<std::size_t>(y - min_y_)].num;
    }
    [[nodiscard]] const Cell* const* scanline_cells(int y) const
    {
        return sorted_cells_.data() + rows_[static_cast<std::size_t>(y - min_y_)].start;
    }

    // rasterizer_cells_aa::line, agg_rasterizer_cells_aa.h:315-463
    void line(int x1, int y1, int x2, int y2)
    {
        constexpr int dx_limit = 16384 << poly_subpixel_shift;

        int dx = x2 - x1;

        if (dx >= dx_limit || dx <= -dx_limit) {
            const int cx = static_cast<int>(
                (static_cast<std::int64_t>(x1) + x2) >> 1);
            const int cy = static_cast<int>(
                (static_cast<std::int64_t>(y1) + y2) >> 1);
            line(x1, y1, cx, cy);
            line(cx, cy, x2, y2);
            // AGG 2.4 as shipped has no `return` here, so the whole line is
            // then rendered a second time below -- reached only by a line
            // 16384 cells across, which with the subpixel rasteriser's 3x
            // horizontal scale is about 5461 pixels. The DeBeOS copy
            // (agg_rasterizer_cells_aa.h:322-328) has the same omission, so it
            // is kept.
        }

        int dy = y2 - y1;
        const int ex1 = x1 >> poly_subpixel_shift;
        const int ex2 = x2 >> poly_subpixel_shift;
        int ey1 = y1 >> poly_subpixel_shift;
        const int ey2 = y2 >> poly_subpixel_shift;
        const int fy1 = y1 & poly_subpixel_mask;
        const int fy2 = y2 & poly_subpixel_mask;

        int x_from;
        int x_to;
        int p;
        int rem;
        int mod;
        int lift;
        int delta;
        int first;
        int incr;

        if (ex1 < min_x_) min_x_ = ex1;
        if (ex1 > max_x_) max_x_ = ex1;
        if (ey1 < min_y_) min_y_ = ey1;
        if (ey1 > max_y_) max_y_ = ey1;
        if (ex2 < min_x_) min_x_ = ex2;
        if (ex2 > max_x_) max_x_ = ex2;
        if (ey2 < min_y_) min_y_ = ey2;
        if (ey2 > max_y_) max_y_ = ey2;

        set_curr_cell(ex1, ey1);

        // everything is on a single hline
        if (ey1 == ey2) {
            render_hline(ey1, x1, fy1, x2, fy2);
            return;
        }

        // Vertical line - we have to calculate start and end cells, and then
        // the common values of the area and coverage for all cells of the line.
        // We know exactly there's only one cell, so, we don't have to call
        // render_hline().
        incr = 1;
        if (dx == 0) {
            const int ex = x1 >> poly_subpixel_shift;
            const int two_fx = (x1 - (ex << poly_subpixel_shift)) << 1;
            int area;

            first = poly_subpixel_scale;
            if (dy < 0) {
                first = 0;
                incr = -1;
            }

            x_from = x1;

            delta = first - fy1;
            curr_.cover += delta;
            curr_.area += two_fx * delta;

            ey1 += incr;
            set_curr_cell(ex, ey1);

            delta = first + first - poly_subpixel_scale;
            area = two_fx * delta;
            while (ey1 != ey2) {
                curr_.cover = delta;
                curr_.area = area;
                ey1 += incr;
                set_curr_cell(ex, ey1);
            }
            delta = fy2 - poly_subpixel_scale + first;
            curr_.cover += delta;
            curr_.area += two_fx * delta;
            return;
        }

        // ok, we have to render several hlines
        p = mul32(poly_subpixel_scale - fy1, dx);
        first = poly_subpixel_scale;

        if (dy < 0) {
            p = mul32(fy1, dx);
            first = 0;
            incr = -1;
            dy = -dy;
        }

        delta = p / dy;
        mod = p % dy;

        if (mod < 0) {
            delta--;
            mod += dy;
        }

        x_from = x1 + delta;
        render_hline(ey1, x1, fy1, x_from, first);

        ey1 += incr;
        set_curr_cell(x_from >> poly_subpixel_shift, ey1);

        if (ey1 != ey2) {
            p = mul32(poly_subpixel_scale, dx);
            lift = p / dy;
            rem = p % dy;

            if (rem < 0) {
                lift--;
                rem += dy;
            }
            mod -= dy;

            while (ey1 != ey2) {
                delta = lift;
                mod += rem;
                if (mod >= 0) {
                    mod -= dy;
                    delta++;
                }

                x_to = x_from + delta;
                render_hline(ey1, x_from, poly_subpixel_scale - first, x_to, first);
                x_from = x_to;

                ey1 += incr;
                set_curr_cell(x_from >> poly_subpixel_shift, ey1);
            }
        }
        render_hline(ey1, x_from, poly_subpixel_scale - first, x2, fy2);
    }

    // rasterizer_cells_aa::sort_cells, :622-724
    void sort_cells()
    {
        if (sorted_)
            return; // Perform sort only the first time.

        add_curr_cell();
        curr_.x = 0x7FFFFFFF;
        curr_.y = 0x7FFFFFFF;
        curr_.cover = 0;
        curr_.area = 0;

        if (cells_.empty())
            return;

        rows_.assign(static_cast<std::size_t>(max_y_ - min_y_ + 1), {0, 0});
        for (const Cell& cell : cells_)
            ++rows_[static_cast<std::size_t>(cell.y - min_y_)].start;
        unsigned start = 0;
        for (auto& row : rows_) {
            const unsigned v = row.start;
            row.start = start;
            start += v;
        }
        sorted_cells_.assign(cells_.size(), nullptr);
        for (const Cell& cell : cells_) {
            auto& row = rows_[static_cast<std::size_t>(cell.y - min_y_)];
            sorted_cells_[row.start + row.num] = &cell;
            ++row.num;
        }
        // AGG sorts each row by x with its own quicksort (qsort_cells), which
        // is not stable. Order among cells of equal x is immaterial: both
        // sweeps only ever sum them.
        for (const auto& row : rows_) {
            if (row.num) {
                std::sort(sorted_cells_.begin() + row.start,
                          sorted_cells_.begin() + row.start + row.num,
                          [](const Cell* a, const Cell* b) { return a->x < b->x; });
            }
        }
        sorted_ = true;
    }

private:
    struct Row {
        unsigned start;
        unsigned num;
    };

    // cell_block_limit (1024) blocks of cell_block_size (4096) cells: past
    // that, add_curr_cell() silently drops cells (:52-53, :179-192).
    static constexpr std::size_t cell_limit = std::size_t {1024} * 4096;

    static Cell initial_cell() { return {0x7FFFFFFF, 0x7FFFFFFF, 0, 0}; }

    void add_curr_cell()
    {
        if (curr_.area | curr_.cover) {
            if (cells_.size() >= cell_limit)
                return;
            cells_.push_back(curr_);
        }
    }

    void set_curr_cell(int x, int y)
    {
        if ((x - curr_.x) | (y - curr_.y)) {
            add_curr_cell();
            curr_.x = x;
            curr_.y = y;
            curr_.cover = 0;
            curr_.area = 0;
        }
    }

    // rasterizer_cells_aa::render_hline, :210-305
    void render_hline(int ey, int x1, int y1, int x2, int y2)
    {
        int ex1 = x1 >> poly_subpixel_shift;
        const int ex2 = x2 >> poly_subpixel_shift;
        const int fx1 = x1 & poly_subpixel_mask;
        const int fx2 = x2 & poly_subpixel_mask;

        int delta;
        int p;
        int first;
        int dx;
        int incr;
        int lift;
        int mod;
        int rem;

        // trivial case. Happens often
        if (y1 == y2) {
            set_curr_cell(ex2, ey);
            return;
        }

        // everything is located in a single cell.  That is easy!
        if (ex1 == ex2) {
            delta = y2 - y1;
            curr_.cover += delta;
            curr_.area += (fx1 + fx2) * delta;
            return;
        }

        // ok, we'll have to render a run of adjacent cells on the same
        // hline...
        p = (poly_subpixel_scale - fx1) * (y2 - y1);
        first = poly_subpixel_scale;
        incr = 1;

        dx = x2 - x1;

        if (dx < 0) {
            p = fx1 * (y2 - y1);
            first = 0;
            incr = -1;
            dx = -dx;
        }

        delta = p / dx;
        mod = p % dx;

        if (mod < 0) {
            delta--;
            mod += dx;
        }

        curr_.cover += delta;
        curr_.area += (fx1 + first) * delta;

        ex1 += incr;
        set_curr_cell(ex1, ey);
        y1 += delta;

        if (ex1 != ex2) {
            p = poly_subpixel_scale * (y2 - y1 + delta);
            lift = p / dx;
            rem = p % dx;

            if (rem < 0) {
                lift--;
                rem += dx;
            }

            mod -= dx;

            while (ex1 != ex2) {
                delta = lift;
                mod += rem;
                if (mod >= 0) {
                    mod -= dx;
                    delta++;
                }

                curr_.cover += delta;
                curr_.area += poly_subpixel_scale * delta;
                y1 += delta;
                ex1 += incr;
                set_curr_cell(ex1, ey);
            }
        }
        delta = y2 - y1;
        curr_.cover += delta;
        curr_.area += (fx2 + poly_subpixel_scale - first) * delta;
    }

    std::vector<Cell> cells_;
    std::vector<const Cell*> sorted_cells_;
    std::vector<Row> rows_;
    Cell curr_ = initial_cell();
    int min_x_ = 0x7FFFFFFF;
    int min_y_ = 0x7FFFFFFF;
    int max_x_ = -0x7FFFFFFF;
    int max_y_ = -0x7FFFFFFF;
    bool sorted_ = false;
};

// ---- rasterizer_sl_clip<ras_conv_int>, agg_rasterizer_sl_clip.h ---------

struct ClipRect {
    int x1;
    int y1;
    int x2;
    int y2;
};

// agg_clip_liang_barsky.h:57-78
unsigned clipping_flags(int x, int y, const ClipRect& clip_box)
{
    return static_cast<unsigned>(x > clip_box.x2)
        | (static_cast<unsigned>(y > clip_box.y2) << 1)
        | (static_cast<unsigned>(x < clip_box.x1) << 2)
        | (static_cast<unsigned>(y < clip_box.y1) << 3);
}

unsigned clipping_flags_y(int y, const ClipRect& clip_box)
{
    return (static_cast<unsigned>(y > clip_box.y2) << 1)
        | (static_cast<unsigned>(y < clip_box.y1) << 3);
}

class LineClipper {
public:
    void clip_box(int x1, int y1, int x2, int y2)
    {
        clip_box_ = {x1, y1, x2, y2};
        // rect_base::normalize, agg_basics.h
        if (clip_box_.x1 > clip_box_.x2)
            std::swap(clip_box_.x1, clip_box_.x2);
        if (clip_box_.y1 > clip_box_.y2)
            std::swap(clip_box_.y1, clip_box_.y2);
        clipping_ = true;
    }

    void move_to(int x1, int y1)
    {
        x1_ = x1;
        y1_ = y1;
        if (clipping_)
            f1_ = clipping_flags(x1, y1, clip_box_);
    }

    void line_to(CellRasterizer& ras, int x2, int y2)
    {
        if (clipping_) {
            const unsigned f2 = clipping_flags(x2, y2, clip_box_);

            // Invisible by Y
            if ((f1_ & 10) == (f2 & 10) && (f1_ & 10) != 0) {
                x1_ = x2;
                y1_ = y2;
                f1_ = f2;
                return;
            }

            const int x1 = x1_;
            const int y1 = y1_;
            const unsigned f1 = f1_;
            int y3;
            int y4;
            unsigned f3;
            unsigned f4;

            switch (((f1 & 5) << 1) | (f2 & 5)) {
            case 0: // Visible by X
                line_clip_y(ras, x1, y1, x2, y2, f1, f2);
                break;

            case 1: // x2 > clip.x2
                y3 = y1 + mul_div(clip_box_.x2 - x1, y2 - y1, x2 - x1);
                f3 = clipping_flags_y(y3, clip_box_);
                line_clip_y(ras, x1, y1, clip_box_.x2, y3, f1, f3);
                line_clip_y(ras, clip_box_.x2, y3, clip_box_.x2, y2, f3, f2);
                break;

            case 2: // x1 > clip.x2
                y3 = y1 + mul_div(clip_box_.x2 - x1, y2 - y1, x2 - x1);
                f3 = clipping_flags_y(y3, clip_box_);
                line_clip_y(ras, clip_box_.x2, y1, clip_box_.x2, y3, f1, f3);
                line_clip_y(ras, clip_box_.x2, y3, x2, y2, f3, f2);
                break;

            case 3: // x1 > clip.x2 && x2 > clip.x2
                line_clip_y(ras, clip_box_.x2, y1, clip_box_.x2, y2, f1, f2);
                break;

            case 4: // x2 < clip.x1
                y3 = y1 + mul_div(clip_box_.x1 - x1, y2 - y1, x2 - x1);
                f3 = clipping_flags_y(y3, clip_box_);
                line_clip_y(ras, x1, y1, clip_box_.x1, y3, f1, f3);
                line_clip_y(ras, clip_box_.x1, y3, clip_box_.x1, y2, f3, f2);
                break;

            case 6: // x1 > clip.x2 && x2 < clip.x1
                y3 = y1 + mul_div(clip_box_.x2 - x1, y2 - y1, x2 - x1);
                y4 = y1 + mul_div(clip_box_.x1 - x1, y2 - y1, x2 - x1);
                f3 = clipping_flags_y(y3, clip_box_);
                f4 = clipping_flags_y(y4, clip_box_);
                line_clip_y(ras, clip_box_.x2, y1, clip_box_.x2, y3, f1, f3);
                line_clip_y(ras, clip_box_.x2, y3, clip_box_.x1, y4, f3, f4);
                line_clip_y(ras, clip_box_.x1, y4, clip_box_.x1, y2, f4, f2);
                break;

            case 8: // x1 < clip.x1
                y3 = y1 + mul_div(clip_box_.x1 - x1, y2 - y1, x2 - x1);
                f3 = clipping_flags_y(y3, clip_box_);
                line_clip_y(ras, clip_box_.x1, y1, clip_box_.x1, y3, f1, f3);
                line_clip_y(ras, clip_box_.x1, y3, x2, y2, f3, f2);
                break;

            case 9: // x1 < clip.x1 && x2 > clip.x2
                y3 = y1 + mul_div(clip_box_.x1 - x1, y2 - y1, x2 - x1);
                y4 = y1 + mul_div(clip_box_.x2 - x1, y2 - y1, x2 - x1);
                f3 = clipping_flags_y(y3, clip_box_);
                f4 = clipping_flags_y(y4, clip_box_);
                line_clip_y(ras, clip_box_.x1, y1, clip_box_.x1, y3, f1, f3);
                line_clip_y(ras, clip_box_.x1, y3, clip_box_.x2, y4, f3, f4);
                line_clip_y(ras, clip_box_.x2, y4, clip_box_.x2, y2, f4, f2);
                break;

            case 12: // x1 < clip.x1 && x2 < clip.x1
                line_clip_y(ras, clip_box_.x1, y1, clip_box_.x1, y2, f1, f2);
                break;
            }
            f1_ = f2;
        } else {
            ras.line(x1_, y1_, x2, y2);
        }
        x1_ = x2;
        y1_ = y2;
    }

private:
    // ras_conv_int::mul_div, agg_rasterizer_sl_clip.h:32-35
    static int mul_div(double a, double b, double c) { return iround(a * b / c); }

    void line_clip_y(CellRasterizer& ras, int x1, int y1, int x2, int y2,
                     unsigned f1, unsigned f2) const
    {
        f1 &= 10;
        f2 &= 10;
        if ((f1 | f2) == 0) {
            // Fully visible
            ras.line(x1, y1, x2, y2);
        } else {
            if (f1 == f2) {
                // Invisible by Y
                return;
            }

            int tx1 = x1;
            int ty1 = y1;
            int tx2 = x2;
            int ty2 = y2;

            if (f1 & 8) { // y1 < clip.y1
                tx1 = x1 + mul_div(clip_box_.y1 - y1, x2 - x1, y2 - y1);
                ty1 = clip_box_.y1;
            }

            if (f1 & 2) { // y1 > clip.y2
                tx1 = x1 + mul_div(clip_box_.y2 - y1, x2 - x1, y2 - y1);
                ty1 = clip_box_.y2;
            }

            if (f2 & 8) { // y2 < clip.y1
                tx2 = x1 + mul_div(clip_box_.y1 - y1, x2 - x1, y2 - y1);
                ty2 = clip_box_.y1;
            }

            if (f2 & 2) { // y2 > clip.y2
                tx2 = x1 + mul_div(clip_box_.y2 - y1, x2 - x1, y2 - y1);
                ty2 = clip_box_.y2;
            }
            ras.line(tx1, ty1, tx2, ty2);
        }
    }

    ClipRect clip_box_ {0, 0, 0, 0};
    int x1_ = 0;
    int y1_ = 0;
    unsigned f1_ = 0;
    bool clipping_ = false;
};

// ---- scanlines ------------------------------------------------------------

// scanline_p8 (agg_scanline_p.h) and Haiku's scanline_p8_subpix
// (agg_scanline_p_subpix.h), which stores three covers per cell. `stride` is
// 1 or 3.
class PackedScanline {
public:
    explicit PackedScanline(unsigned stride) : stride_(stride) {}

    void reset(int min_x, int max_x)
    {
        const auto max_len = static_cast<std::size_t>(
            stride_ * static_cast<unsigned>(max_x - min_x + 3));
        if (max_len > spans_.size()) {
            spans_.resize(max_len);
            covers_.resize(max_len);
        }
        reset_spans();
    }

    void reset_spans()
    {
        last_x_ = 0x7FFFFFF0;
        cover_ = 0;
        cur_ = 0;
        spans_[0].len = 0;
    }

    void add_cell(int x, unsigned cover)
    {
        covers_[cover_] = static_cast<std::uint8_t>(cover);
        if (x == last_x_ + 1 && spans_[cur_].len > 0) {
            spans_[cur_].len = static_cast<std::int16_t>(spans_[cur_].len + 1);
        } else {
            ++cur_;
            spans_[cur_] = {static_cast<std::int16_t>(x), 1, cover_};
        }
        last_x_ = x;
        ++cover_;
    }

    void add_cell(int x, unsigned cover1, unsigned cover2, unsigned cover3)
    {
        covers_[cover_] = static_cast<std::uint8_t>(cover1);
        covers_[cover_ + 1] = static_cast<std::uint8_t>(cover2);
        covers_[cover_ + 2] = static_cast<std::uint8_t>(cover3);
        if (x == last_x_ + 1 && spans_[cur_].len > 0) {
            spans_[cur_].len = static_cast<std::int16_t>(spans_[cur_].len + 3);
        } else {
            ++cur_;
            spans_[cur_] = {static_cast<std::int16_t>(x), 3, cover_};
        }
        last_x_ = x;
        cover_ += 3;
    }

    void add_span(int x, unsigned len, unsigned cover)
    {
        if (x == last_x_ + 1 && spans_[cur_].len < 0
            && cover == covers_[spans_[cur_].cover]) {
            spans_[cur_].len = static_cast<std::int16_t>(
                spans_[cur_].len
                - static_cast<int>(stride_) * static_cast<std::int16_t>(len));
        } else {
            covers_[cover_] = static_cast<std::uint8_t>(cover);
            ++cur_;
            spans_[cur_] = {
                static_cast<std::int16_t>(x),
                static_cast<std::int16_t>(
                    static_cast<int>(stride_)
                    * static_cast<std::int16_t>(-static_cast<int>(len))),
                cover_};
            cover_ += stride_;
        }
        last_x_ = x + static_cast<int>(len) - 1;
    }

    [[nodiscard]] unsigned num_spans() const { return static_cast<unsigned>(cur_); }

    void emit(int y, ScanlineSink& sink)
    {
        out_.clear();
        for (std::size_t i = 1; i <= cur_; ++i)
            out_.push_back({spans_[i].x, spans_[i].len, &covers_[spans_[i].cover]});
        sink.render(y, out_.data(), static_cast<unsigned>(out_.size()));
    }

private:
    struct Stored {
        std::int16_t x;
        std::int16_t len;
        std::size_t cover;
    };

    unsigned stride_;
    int last_x_ = 0x7FFFFFF0;
    std::vector<std::uint8_t> covers_ = std::vector<std::uint8_t>(4);
    std::size_t cover_ = 0;
    std::vector<Stored> spans_ = std::vector<Stored>(4);
    std::size_t cur_ = 0;
    std::vector<Span> out_;
};

// scanline_u8, agg_scanline_u.h
class UnpackedScanline {
public:
    void reset(int min_x, int max_x)
    {
        const auto max_len = static_cast<std::size_t>(max_x - min_x + 2);
        if (max_len > spans_.size()) {
            spans_.resize(max_len);
            covers_.resize(max_len);
        }
        last_x_ = 0x7FFFFFF0;
        min_x_ = min_x;
        cur_ = 0;
    }

    void reset_spans()
    {
        last_x_ = 0x7FFFFFF0;
        cur_ = 0;
    }

    void add_cell(int x, unsigned cover)
    {
        x -= min_x_;
        covers_[static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(cover);
        if (x == last_x_ + 1) {
            spans_[cur_].len = static_cast<std::int16_t>(spans_[cur_].len + 1);
        } else {
            ++cur_;
            spans_[cur_] = {static_cast<std::int16_t>(x + min_x_), 1,
                            static_cast<std::size_t>(x)};
        }
        last_x_ = x;
    }

    void add_span(int x, unsigned len, unsigned cover)
    {
        x -= min_x_;
        std::memset(&covers_[static_cast<std::size_t>(x)], static_cast<int>(cover), len);
        if (x == last_x_ + 1) {
            spans_[cur_].len = static_cast<std::int16_t>(
                spans_[cur_].len + static_cast<std::int16_t>(len));
        } else {
            ++cur_;
            spans_[cur_] = {static_cast<std::int16_t>(x + min_x_),
                            static_cast<std::int16_t>(len),
                            static_cast<std::size_t>(x)};
        }
        last_x_ = x + static_cast<int>(len) - 1;
    }

    [[nodiscard]] unsigned num_spans() const { return static_cast<unsigned>(cur_); }

    void emit(int y, ScanlineSink& sink)
    {
        out_.clear();
        for (std::size_t i = 1; i <= cur_; ++i)
            out_.push_back({spans_[i].x, spans_[i].len, &covers_[spans_[i].cover]});
        sink.render(y, out_.data(), static_cast<unsigned>(out_.size()));
    }

private:
    struct Stored {
        std::int16_t x;
        std::int16_t len;
        std::size_t cover;
    };

    int last_x_ = 0x7FFFFFF0;
    int min_x_ = 0;
    std::vector<std::uint8_t> covers_;
    std::vector<Stored> spans_ = std::vector<Stored>(2);
    std::size_t cur_ = 0;
    std::vector<Span> out_;
};

// scanline_u8_subpix, Painter/agg_scanline_u_subpix.h: scanline_u8 with three
// covers per pixel. AGGTextRenderer renders its vector glyphs through it
// (AGGTextRenderer.cpp:154-157 with the Painter's fSubpixUnpackedScanline,
// defines.h:66, AVERAGE_BASED_SUBPIXEL_FILTERING being off).
class UnpackedSubpixScanline {
public:
    void reset(int min_x, int max_x)
    {
        const auto max_len = static_cast<std::size_t>(3 * (max_x - min_x + 2));
        if (max_len > spans_.size()) {
            spans_.resize(max_len);
            covers_.resize(max_len);
        }
        last_x_ = 0x7FFFFFF0;
        min_x_ = min_x;
        cur_ = 0;
    }

    void reset_spans()
    {
        last_x_ = 0x7FFFFFF0;
        cur_ = 0;
    }

    void add_cell(int x, unsigned cover1, unsigned cover2, unsigned cover3)
    {
        x -= min_x_;
        const auto at = static_cast<std::size_t>(3 * x);
        covers_[at] = static_cast<std::uint8_t>(cover1);
        covers_[at + 1] = static_cast<std::uint8_t>(cover2);
        covers_[at + 2] = static_cast<std::uint8_t>(cover3);
        if (x == last_x_ + 1) {
            spans_[cur_].len = static_cast<std::int16_t>(spans_[cur_].len + 3);
        } else {
            ++cur_;
            spans_[cur_] = {static_cast<std::int16_t>(x + min_x_), 3, at};
        }
        last_x_ = x;
    }

    void add_span(int x, unsigned len, unsigned cover)
    {
        x -= min_x_;
        const auto at = static_cast<std::size_t>(3 * x);
        std::memset(&covers_[at], static_cast<int>(cover), 3 * len);
        if (x == last_x_ + 1) {
            spans_[cur_].len = static_cast<std::int16_t>(
                spans_[cur_].len + 3 * static_cast<std::int16_t>(len));
        } else {
            ++cur_;
            spans_[cur_] = {static_cast<std::int16_t>(x + min_x_),
                            static_cast<std::int16_t>(3 * static_cast<std::int16_t>(len)),
                            at};
        }
        last_x_ = x + static_cast<int>(len) - 1;
    }

    [[nodiscard]] unsigned num_spans() const { return static_cast<unsigned>(cur_); }

    void emit(int y, ScanlineSink& sink)
    {
        out_.clear();
        for (std::size_t i = 1; i <= cur_; ++i)
            out_.push_back({spans_[i].x, spans_[i].len, &covers_[spans_[i].cover]});
        sink.render(y, out_.data(), static_cast<unsigned>(out_.size()));
    }

private:
    struct Stored {
        std::int16_t x;
        std::int16_t len;
        std::size_t cover;
    };

    int last_x_ = 0x7FFFFFF0;
    int min_x_ = 0;
    std::vector<std::uint8_t> covers_;
    std::vector<Stored> spans_ = std::vector<Stored>(2);
    std::size_t cur_ = 0;
    std::vector<Span> out_;
};

} // namespace

int saturating_int(double v)
{
    if (std::isnan(v))
        return 0;
    if (v >= 2147483647.0)
        return std::numeric_limits<int>::max();
    if (v <= -2147483648.0)
        return std::numeric_limits<int>::min();
    return static_cast<int>(v);
}

// ---- trans_affine ---------------------------------------------------------

TransAffine TransAffine::rotation(double a)
{
    // trans_affine_rotation, agg_trans_affine.h:416-421
    return {std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0.0, 0.0};
}

TransAffine& TransAffine::multiply(const TransAffine& m)
{
    // agg_trans_affine.cpp:69-82
    const double t0 = sx * m.sx + shy * m.shx;
    const double t2 = shx * m.sx + sy * m.shx;
    const double t4 = tx * m.sx + ty * m.shx + m.tx;
    shy = sx * m.shy + shy * m.sy;
    sy = shx * m.shy + sy * m.sy;
    ty = tx * m.shy + ty * m.sy + m.ty;
    sx = t0;
    shx = t2;
    tx = t4;
    return *this;
}

TransAffine& TransAffine::invert()
{
    // agg_trans_affine.cpp:85-101, determinant_reciprocal() at
    // agg_trans_affine.h:265-268
    const double d = 1.0 / (sx * sy - shy * shx);

    const double t0 = sy * d;
    sy = sx * d;
    shy = -shy * d;
    shx = -shx * d;

    const double t4 = -tx * t0 - ty * shx;
    ty = -tx * shy - ty * sy;

    sx = t0;
    tx = t4;
    return *this;
}

double TransAffine::scale() const
{
    // agg_trans_affine.h:319-324
    constexpr double sqrt1_2 = 0.70710678118654752440; // M_SQRT1_2
    const double x = sqrt1_2 * sx + sqrt1_2 * shx;
    const double y = sqrt1_2 * shy + sqrt1_2 * sy;
    return std::sqrt(x * x + y * y);
}

// ---- path_storage ---------------------------------------------------------

void PathStorage::curve4(double x_ctrl1, double y_ctrl1, double x_ctrl2,
                         double y_ctrl2, double x_to, double y_to)
{
    // agg_path_storage.h:1011-1019
    vertices_.push_back({x_ctrl1, y_ctrl1, path_cmd_curve4});
    vertices_.push_back({x_ctrl2, y_ctrl2, path_cmd_curve4});
    vertices_.push_back({x_to, y_to, path_cmd_curve4});
}

unsigned PathStorage::last_vertex(double* x, double* y) const
{
    // vertex_block_storage::last_vertex, agg_path_storage.h:232-237
    if (vertices_.empty())
        return path_cmd_stop;
    *x = vertices_.back().x;
    *y = vertices_.back().y;
    return vertices_.back().cmd;
}

void PathStorage::end_poly(unsigned flags)
{
    // agg_path_storage.h:1072-1079
    if (!vertices_.empty() && is_vertex(vertices_.back().cmd))
        vertices_.push_back({0.0, 0.0, path_cmd_end_poly | flags});
}

void PathStorage::join_path(const VertexList& source)
{
    // path_base::join_path, agg_path_storage.h:735-776
    Source vs(source);
    double x = 0;
    double y = 0;
    unsigned cmd = vs.vertex(&x, &y);
    if (is_stop(cmd))
        return;
    if (is_vertex(cmd)) {
        double x0 = 0;
        double y0 = 0;
        const unsigned cmd0 = last_vertex(&x0, &y0);
        if (is_vertex(cmd0)) {
            if (calc_distance(x, y, x0, y0) > vertex_dist_epsilon) {
                if (is_move_to(cmd))
                    cmd = path_cmd_line_to;
                vertices_.push_back({x, y, cmd});
            }
        } else {
            if (is_stop(cmd0)) {
                cmd = path_cmd_move_to;
            } else {
                if (is_move_to(cmd))
                    cmd = path_cmd_line_to;
            }
            vertices_.push_back({x, y, cmd});
        }
    }
    while (!is_stop(cmd = vs.vertex(&x, &y)))
        vertices_.push_back({x, y, is_move_to(cmd) ? static_cast<unsigned>(path_cmd_line_to) : cmd});
}

void PathStorage::arc_to(double rx, double ry, double angle,
                         bool large_arc_flag, bool sweep_flag, double x,
                         double y)
{
    // agg_path_storage.h:898-940
    if (!vertices_.empty() && is_vertex(vertices_.back().cmd)) {
        const double epsilon = 1e-30;
        double x0 = 0.0;
        double y0 = 0.0;
        last_vertex(&x0, &y0);

        rx = std::fabs(rx);
        ry = std::fabs(ry);

        // Ensure radii are valid
        if (rx < epsilon || ry < epsilon) {
            line_to(x, y);
            return;
        }

        if (calc_distance(x0, y0, x, y) < epsilon) {
            // If the endpoints (x, y) and (x0, y0) are identical, then this
            // is equivalent to omitting the elliptical arc segment entirely.
            return;
        }
        BezierArc a;
        if (bezier_arc_svg(x0, y0, rx, ry, angle, large_arc_flag, sweep_flag,
                           x, y, a)) {
            join_path(a.vertex_list());
        } else {
            line_to(x, y);
        }
    } else {
        move_to(x, y);
    }
}

// ---- generators -----------------------------------------------------------

VertexList conv_curve(const VertexList& list, double approximation_scale)
{
    // conv_curve::vertex, agg_conv_curve.h:143-179 (DeBeOS's copy, with the
    // NaN guards). Shapes only ever reach curve4; FontEngine's TrueType
    // outlines are quadratic and reach curve3.
    Source source(list);
    Curve3Div curve3;
    Curve4Div curve4;
    curve3.approximation_scale(approximation_scale);
    curve4.approximation_scale(approximation_scale);
    curve3.reset();
    curve4.reset();
    double last_x = 0.0;
    double last_y = 0.0;
    double x = 0;
    double y = 0;
    VertexList out;
    for (;;) {
        if (!is_stop(curve3.vertex(&x, &y))) {
            last_x = x;
            last_y = y;
            out.push_back({x, y, path_cmd_line_to});
            continue;
        }
        if (!is_stop(curve4.vertex(&x, &y))) {
            last_x = x;
            last_y = y;
            out.push_back({x, y, path_cmd_line_to});
            continue;
        }

        double ct2_x = 0;
        double ct2_y = 0;
        double end_x = 0;
        double end_y = 0;

        unsigned cmd = source.vertex(&x, &y);
        if (is_stop(cmd))
            break;
        if (cmd == path_cmd_curve3) {
            source.vertex(&end_x, &end_y);
            if (!std::isnan(last_x) && !std::isnan(last_y) && !std::isnan(x)
                && !std::isnan(y) && !std::isnan(end_x) && !std::isnan(end_y)) {
                curve3.init(last_x, last_y, x, y, end_x, end_y);
                curve3.vertex(&x, &y); // First call returns path_cmd_move_to
                curve3.vertex(&x, &y); // This is the first vertex of the curve
            }
            cmd = path_cmd_line_to;
        } else if (cmd == path_cmd_curve4) {
            source.vertex(&ct2_x, &ct2_y);
            source.vertex(&end_x, &end_y);
            if (!std::isnan(last_x) && !std::isnan(last_y) && !std::isnan(x)
                && !std::isnan(y) && !std::isnan(end_x) && !std::isnan(end_y)) {
                curve4.init(last_x, last_y, x, y, ct2_x, ct2_y, end_x, end_y);
                curve4.vertex(&x, &y); // First call returns path_cmd_move_to
                curve4.vertex(&x, &y); // This is the first vertex of the curve
            }
            cmd = path_cmd_line_to;
        }
        last_x = x;
        last_y = y;
        out.push_back({x, y, cmd});
    }
    return out;
}

VertexList ellipse(double x, double y, double rx, double ry, unsigned num_steps)
{
    // ellipse::vertex, agg_ellipse.h:100-115, with m_cw false.
    VertexList out;
    double px = 0;
    double py = 0;
    for (unsigned step = 0; step < num_steps; ++step) {
        const double angle = double(step) / double(num_steps) * 2.0 * pi;
        px = x + std::cos(angle) * rx;
        py = y + std::sin(angle) * ry;
        out.push_back({px, py, step == 0 ? static_cast<unsigned>(path_cmd_move_to)
                                         : static_cast<unsigned>(path_cmd_line_to)});
    }
    out.push_back({px, py, path_cmd_end_poly | path_flags_close | path_flags_ccw});
    return out;
}

VertexList rounded_rect(double x1, double y1, double x2, double y2, double rx,
                        double ry)
{
    // rounded_rect::rect (agg_rounded_rect.cpp:36-45), radius(rx, ry)
    // (:53-58) and vertex (:104-160).
    if (x1 > x2)
        std::swap(x1, x2);
    if (y1 > y2)
        std::swap(y1, y2);

    VertexList out;
    Arc arc;
    double x = 0;
    double y = 0;
    const auto corner = [&](double cx, double cy, double a1, double a2,
                            bool first) {
        arc.init(cx, cy, rx, ry, a1, a2);
        arc.rewind();
        for (;;) {
            const unsigned cmd = arc.vertex(&x, &y);
            if (is_stop(cmd))
                break;
            out.push_back({x, y, first ? cmd : static_cast<unsigned>(path_cmd_line_to)});
        }
    };
    corner(x1 + rx, y1 + ry, pi, pi + pi * 0.5, true);
    corner(x2 - rx, y1 + ry, pi + pi * 0.5, 0.0, false);
    corner(x2 - rx, y2 - ry, 0.0, pi * 0.5, false);
    corner(x1 + rx, y2 - ry, pi * 0.5, pi, false);
    out.push_back({x, y, path_cmd_end_poly | path_flags_close | path_flags_ccw});
    return out;
}

VertexList bezier_arc(double x, double y, double rx, double ry,
                      double start_angle, double sweep_angle)
{
    BezierArc arc;
    arc.init(x, y, rx, ry, start_angle, sweep_angle);
    return arc.vertex_list();
}

VertexList conv_stroke(const VertexList& list, const StrokeStyle& style)
{
    // conv_adaptor_vcgen<VertexSource, vcgen_stroke>::vertex,
    // agg_conv_adaptor_vcgen.h:88-153
    Source source(list);
    VcgenStroke generator(style);
    VertexList out;
    enum { initial, accumulate, generate } status = initial;
    unsigned last_cmd = path_cmd_stop;
    double start_x = 0;
    double start_y = 0;
    double x = 0;
    double y = 0;

    for (;;) {
        unsigned cmd = path_cmd_stop;
        bool done = false;
        while (!done) {
            switch (status) {
            case initial:
                last_cmd = source.vertex(&start_x, &start_y);
                status = accumulate;
                [[fallthrough]];

            case accumulate:
                if (is_stop(last_cmd))
                    return out;

                generator.remove_all();
                generator.add_vertex(start_x, start_y, path_cmd_move_to);

                for (;;) {
                    cmd = source.vertex(&x, &y);
                    if (is_vertex(cmd)) {
                        last_cmd = cmd;
                        if (is_move_to(cmd)) {
                            start_x = x;
                            start_y = y;
                            break;
                        }
                        generator.add_vertex(x, y, cmd);
                    } else {
                        if (is_stop(cmd)) {
                            last_cmd = path_cmd_stop;
                            break;
                        }
                        if (is_end_poly(cmd)) {
                            generator.add_vertex(x, y, cmd);
                            break;
                        }
                    }
                }
                generator.rewind();
                status = generate;
                [[fallthrough]];

            case generate:
                cmd = generator.vertex(&x, &y);
                if (is_stop(cmd)) {
                    status = accumulate;
                    break;
                }
                done = true;
                break;
            }
        }
        out.push_back({x, y, cmd});
    }
}

VertexList conv_contour(const VertexList& list, double width)
{
    // conv_adaptor_vcgen<VertexSource, vcgen_contour>::vertex,
    // agg_conv_adaptor_vcgen.h:88-153: the same accumulate/generate loop as
    // conv_stroke's, around the contour generator.
    Source source(list);
    VcgenContour generator(width);
    VertexList out;
    enum { initial, accumulate, generate } status = initial;
    unsigned last_cmd = path_cmd_stop;
    double start_x = 0;
    double start_y = 0;
    double x = 0;
    double y = 0;

    for (;;) {
        unsigned cmd = path_cmd_stop;
        bool done = false;
        while (!done) {
            switch (status) {
            case initial:
                last_cmd = source.vertex(&start_x, &start_y);
                status = accumulate;
                [[fallthrough]];

            case accumulate:
                if (is_stop(last_cmd))
                    return out;

                generator.remove_all();
                generator.add_vertex(start_x, start_y, path_cmd_move_to);

                for (;;) {
                    cmd = source.vertex(&x, &y);
                    if (is_vertex(cmd)) {
                        last_cmd = cmd;
                        if (is_move_to(cmd)) {
                            start_x = x;
                            start_y = y;
                            break;
                        }
                        generator.add_vertex(x, y, cmd);
                    } else {
                        if (is_stop(cmd)) {
                            last_cmd = path_cmd_stop;
                            break;
                        }
                        if (is_end_poly(cmd)) {
                            generator.add_vertex(x, y, cmd);
                            break;
                        }
                    }
                }
                generator.rewind();
                status = generate;
                [[fallthrough]];

            case generate:
                cmd = generator.vertex(&x, &y);
                if (is_stop(cmd)) {
                    status = accumulate;
                    break;
                }
                done = true;
                break;
            }
        }
        out.push_back({x, y, cmd});
    }
}

VertexList conv_transform(const VertexList& list, const TransAffine& matrix)
{
    // conv_transform::vertex, agg_conv_transform.h:41-49
    VertexList out = list;
    for (auto& v : out) {
        if (is_vertex(v.cmd))
            matrix.transform(&v.x, &v.y);
    }
    return out;
}

// ---- the scanline rasterisers ---------------------------------------------

struct Rasterizer::Impl {
    explicit Impl(bool subpixel_) : subpixel(subpixel_) {}

    enum Status { status_initial, status_move_to, status_line_to, status_closed };

    bool subpixel;
    CellRasterizer outline;
    LineClipper clipper;
    bool auto_close = true;
    int start_x = 0;
    int start_y = 0;
    Status status = status_initial;
    int scan_y = 0;

    static int upscale(double v) { return iround(v * poly_subpixel_scale); }

    void reset()
    {
        outline.reset();
        status = status_initial;
    }

    void close_polygon()
    {
        if (status == status_line_to) {
            clipper.line_to(outline, start_x, start_y);
            status = status_closed;
        }
    }

    void move_to_d(double x, double y)
    {
        if (outline.sorted())
            reset();
        if (auto_close)
            close_polygon();
        clipper.move_to(start_x = upscale(x), start_y = upscale(y));
        status = status_move_to;
    }

    void line_to_d(double x, double y)
    {
        clipper.line_to(outline, upscale(x), upscale(y));
        status = status_line_to;
    }

    void add_vertex(double x, double y, unsigned cmd)
    {
        if (is_move_to(cmd)) {
            move_to_d(x, y);
        } else if (is_vertex(cmd)) {
            line_to_d(x, y);
        } else if (is_close(cmd)) {
            close_polygon();
        }
    }

    bool rewind_scanlines()
    {
        if (auto_close)
            close_polygon();
        outline.sort_cells();
        if (outline.total_cells() == 0)
            return false;
        scan_y = outline.min_y();
        return true;
    }

    // calculate_alpha, agg_rasterizer_scanline_aa.h:214-229 and
    // agg_rasterizer_scanline_aa_subpix.h:137-152 (identical): the identity
    // gamma table, non-zero filling rule.
    //
    // m_gamma is the identity unless gamma_threshold() replaced it.
    std::array<std::uint8_t, 256> gamma = identity_gamma();

    static std::array<std::uint8_t, 256> identity_gamma()
    {
        std::array<std::uint8_t, 256> table {};
        for (unsigned i = 0; i < 256; ++i)
            table[i] = static_cast<std::uint8_t>(i);
        return table;
    }

    unsigned calculate_alpha(int area) const
    {
        int cover = area >> (poly_subpixel_shift * 2 + 1 - 8);
        if (cover < 0)
            cover = -cover;
        if (cover > 255)
            cover = 255;
        return gamma[static_cast<std::size_t>(cover)];
    }

    template<class Scanline>
    bool sweep_scanline(Scanline& sl)
    {
        // rasterizer_scanline_aa::sweep_scanline,
        // agg_rasterizer_scanline_aa.h:231-287
        for (;;) {
            if (scan_y > outline.max_y())
                return false;
            sl.reset_spans();
            unsigned num_cells = outline.scanline_num_cells(scan_y);
            const Cell* const* cells = outline.scanline_cells(scan_y);
            int cover = 0;

            while (num_cells) {
                const Cell* cur_cell = *cells;
                int x = cur_cell->x;
                int area = cur_cell->area;
                unsigned alpha;

                cover += cur_cell->cover;

                // accumulate all cells with the same X
                while (--num_cells) {
                    cur_cell = *++cells;
                    if (cur_cell->x != x)
                        break;
                    area += cur_cell->area;
                    cover += cur_cell->cover;
                }

                if (area) {
                    alpha = calculate_alpha(
                        static_cast<int>(static_cast<unsigned>(cover)
                                         << (poly_subpixel_shift + 1))
                        - area);
                    if (alpha)
                        sl.add_cell(x, alpha);
                    x++;
                }

                if (num_cells && cur_cell->x > x) {
                    alpha = calculate_alpha(static_cast<int>(
                        static_cast<unsigned>(cover) << (poly_subpixel_shift + 1)));
                    if (alpha)
                        sl.add_span(x, static_cast<unsigned>(cur_cell->x - x), alpha);
                }
            }

            if (sl.num_spans())
                break;
            ++scan_y;
        }

        ++scan_y;
        return true;
    }

    static int shifted(int cover)
    {
        return static_cast<int>(static_cast<unsigned>(cover) << (poly_subpixel_shift + 1));
    }

    template<class Scanline>
    bool sweep_scanline_subpix(Scanline& sl)
    {
        // rasterizer_scanline_aa_subpix::sweep_scanline,
        // agg_rasterizer_scanline_aa_subpix.h:154-325
        for (;;) {
            if (scan_y > outline.max_y())
                return false;
            sl.reset_spans();
            unsigned num_cells = outline.scanline_num_cells(scan_y);
            const Cell* const* cells = outline.scanline_cells(scan_y);
            int cover = 0;
            int cover2 = 0;
            int cover3 = 0;

            while (num_cells) {
                const Cell* cur_cell = *cells;
                int x = cur_cell->x;
                int area1 = cur_cell->area;
                int area2;
                int area3;
                unsigned alpha1;
                unsigned alpha2;
                unsigned alpha3;

                const int last_cover = cover3;
                cover = cover3;
                cover += cur_cell->cover;

                while (--num_cells) {
                    cur_cell = *++cells;
                    if (cur_cell->x != x)
                        break;
                    area1 += cur_cell->area;
                    cover += cur_cell->cover;
                }

                if (x % 3 == 0) {
                    if (cur_cell->x == x + 1) {
                        area2 = cur_cell->area;
                        cover2 = cover + cur_cell->cover;

                        while (--num_cells) {
                            cur_cell = *++cells;
                            if (cur_cell->x != x + 1)
                                break;
                            area2 += cur_cell->area;
                            cover2 += cur_cell->cover;
                        }
                    } else {
                        area2 = 0;
                        cover2 = cover;
                    }

                    if (cur_cell->x == x + 2) {
                        area3 = cur_cell->area;
                        cover3 = cover2 + cur_cell->cover;

                        while (--num_cells) {
                            cur_cell = *++cells;
                            if (cur_cell->x != x + 2)
                                break;
                            area3 += cur_cell->area;
                            cover3 += cur_cell->cover;
                        }
                    } else {
                        area3 = 0;
                        cover3 = cover2;
                    }
                } else if (x % 3 == 1) {
                    area2 = area1;
                    area1 = 0;
                    cover2 = cover;
                    cover = last_cover;
                    if (cur_cell->x == x + 1) {
                        area3 = cur_cell->area;
                        cover3 = cover2 + cur_cell->cover;

                        while (--num_cells) {
                            cur_cell = *++cells;
                            if (cur_cell->x != x + 1)
                                break;
                            area3 += cur_cell->area;
                            cover3 += cur_cell->cover;
                        }
                    } else {
                        area3 = 0;
                        cover3 = cover2;
                    }
                } else { // if (x % 3 == 2)
                    area3 = area1;
                    area2 = 0;
                    area1 = 0;
                    cover3 = cover;
                    cover = last_cover;
                    cover2 = last_cover;
                }

                alpha1 = area1 ? calculate_alpha(shifted(cover) - area1) : 0;
                alpha2 = area2 ? calculate_alpha(shifted(cover2) - area2) : 0;
                alpha3 = area3 ? calculate_alpha(shifted(cover3) - area3) : 0;
                if (alpha1 || alpha2 || alpha3) {
                    x += 3 - (x % 3);
                    if (area1 && !area2 && area3) {
                        alpha2 = calculate_alpha(shifted(cover));
                    } else if (num_cells && cur_cell->x >= x) {
                        if (area1 && !area2) {
                            alpha2 = calculate_alpha(shifted(cover));
                            alpha3 = alpha2;
                        }
                        if (area2 && !area3)
                            alpha3 = calculate_alpha(shifted(cover2));
                    }
                    if (!area1) {
                        if (area2) {
                            alpha1 = calculate_alpha(shifted(cover));
                        } else if (area3) {
                            alpha2 = calculate_alpha(shifted(cover));
                            alpha1 = alpha2;
                        }
                    }
                    sl.add_cell(x / 3 - 1, alpha1, alpha2, alpha3);
                }

                if (num_cells && cur_cell->x - x >= 3) {
                    alpha1 = calculate_alpha(shifted(cover3));
                    sl.add_span(x / 3, static_cast<unsigned>(cur_cell->x / 3 - x / 3),
                                alpha1);
                }
            }

            if (sl.num_spans())
                break;
            ++scan_y;
        }

        ++scan_y;
        return true;
    }
};

Rasterizer::Rasterizer(bool subpixel) : impl_(std::make_unique<Impl>(subpixel)) {}

Rasterizer::~Rasterizer() = default;

void Rasterizer::clip_box(double x1, double y1, double x2, double y2)
{
    impl_->reset();
    if (impl_->subpixel) {
        // agg_rasterizer_scanline_aa_subpix.h:376-384, verbatim -- including
        // the left edge, which passes through ras_conv_int::downscale (the
        // identity on int, so a truncation of the double) instead of
        // upscale, landing at 3/256 of the intended coordinate. It all but
        // disables clipping at the left edge of the box.
        impl_->clipper.clip_box(3 * saturating_int(x1), Impl::upscale(y1),
                                Impl::upscale(3 * x2), Impl::upscale(y2));
    } else {
        // agg_rasterizer_scanline_aa.h:338-346
        impl_->clipper.clip_box(Impl::upscale(x1), Impl::upscale(y1),
                                Impl::upscale(x2), Impl::upscale(y2));
    }
}

// Past about two million pixels, AGG's own int arithmetic overflows: the
// clipper subtracts upscaled coordinates (agg_rasterizer_sl_clip.h:147-296),
// and int overflow is undefined behaviour in the server as much as here. Wire
// coordinates are unvalidated, so vertices are clamped to +-2^21 pixels (2^29
// subpixel units, whose differences fit an int) -- far outside any surface,
// where the clipper discards or flattens them exactly as it would the
// original -- and a NaN, which upscale() would turn into 0 on arm64, is made
// 0 explicitly.
static double clamp_coordinate(double v)
{
    constexpr double limit = 1 << 21;
    if (std::isnan(v))
        return 0.0;
    return std::clamp(v, -limit, limit);
}

void Rasterizer::add_path(const VertexList& path)
{
    if (impl_->outline.sorted())
        impl_->reset();
    for (const Vertex& v : path) {
        double x = v.x;
        // agg_rasterizer_scanline_aa_subpix.h:108-123: the subpixel
        // rasteriser triples every vertex's x, and so works in thirds of a
        // pixel horizontally.
        if (impl_->subpixel && is_vertex(v.cmd))
            x *= 3;
        impl_->add_vertex(clamp_coordinate(x), clamp_coordinate(v.y), v.cmd);
    }
}

void Rasterizer::render_packed(ScanlineSink& sink)
{
    // render_scanlines, agg_renderer_scanline.h:438-449
    if (!impl_->rewind_scanlines())
        return;
    if (impl_->subpixel) {
        PackedScanline sl(3);
        // min_x()/max_x() divide by 3, agg_rasterizer_scanline_aa_subpix.h:126-129
        sl.reset(impl_->outline.min_x() / 3, impl_->outline.max_x() / 3);
        while (impl_->sweep_scanline_subpix(sl))
            sl.emit(impl_->scan_y - 1, sink);
    } else {
        PackedScanline sl(1);
        sl.reset(impl_->outline.min_x(), impl_->outline.max_x());
        while (impl_->sweep_scanline(sl))
            sl.emit(impl_->scan_y - 1, sink);
    }
}

void Rasterizer::render_unpacked(ScanlineSink& sink)
{
    if (!impl_->rewind_scanlines())
        return;
    if (impl_->subpixel) {
        UnpackedSubpixScanline sl;
        // min_x()/max_x() divide by 3, agg_rasterizer_scanline_aa_subpix.h:126-129
        sl.reset(impl_->outline.min_x() / 3, impl_->outline.max_x() / 3);
        while (impl_->sweep_scanline_subpix(sl))
            sl.emit(impl_->scan_y - 1, sink);
        return;
    }
    UnpackedScanline sl;
    sl.reset(impl_->outline.min_x(), impl_->outline.max_x());
    while (impl_->sweep_scanline(sl))
        sl.emit(impl_->scan_y - 1, sink);
}

void Rasterizer::gamma_threshold(double threshold)
{
    // rasterizer_scanline_aa::gamma, agg_rasterizer_scanline_aa.h:131-140,
    // with gamma_threshold, agg_gamma_functions.h:96-108:
    // m_gamma[i] = uround(gamma(i / aa_mask) * aa_mask).
    for (unsigned i = 0; i < 256; ++i) {
        const double x = double(i) / 255.0;
        impl_->gamma[i] = static_cast<std::uint8_t>(
            uround((x < threshold ? 0.0 : 1.0) * 255.0));
    }
}

// ---- span_gradient, agg_span_gradient.h -------------------------------------

namespace {

// agg_sqrt_tables.cpp, verbatim.
const std::uint16_t g_sqrt_table[1024] = {
    0, 2048, 2896, 3547, 4096, 4579, 5017, 5418, 5793, 6144, 6476, 6792,
    7094, 7384, 7663, 7932, 8192, 8444, 8689, 8927, 9159, 9385, 9606, 9822,
    10033, 10240, 10443, 10642, 10837, 11029, 11217, 11403, 11585, 11765, 11942, 12116,
    12288, 12457, 12625, 12790, 12953, 13114, 13273, 13430, 13585, 13738, 13890, 14040,
    14189, 14336, 14482, 14626, 14768, 14910, 15050, 15188, 15326, 15462, 15597, 15731,
    15864, 15995, 16126, 16255, 16384, 16512, 16638, 16764, 16888, 17012, 17135, 17257,
    17378, 17498, 17618, 17736, 17854, 17971, 18087, 18203, 18318, 18432, 18545, 18658,
    18770, 18882, 18992, 19102, 19212, 19321, 19429, 19537, 19644, 19750, 19856, 19961,
    20066, 20170, 20274, 20377, 20480, 20582, 20684, 20785, 20886, 20986, 21085, 21185,
    21283, 21382, 21480, 21577, 21674, 21771, 21867, 21962, 22058, 22153, 22247, 22341,
    22435, 22528, 22621, 22713, 22806, 22897, 22989, 23080, 23170, 23261, 23351, 23440,
    23530, 23619, 23707, 23796, 23884, 23971, 24059, 24146, 24232, 24319, 24405, 24491,
    24576, 24661, 24746, 24831, 24915, 24999, 25083, 25166, 25249, 25332, 25415, 25497,
    25580, 25661, 25743, 25824, 25905, 25986, 26067, 26147, 26227, 26307, 26387, 26466,
    26545, 26624, 26703, 26781, 26859, 26937, 27015, 27092, 27170, 27247, 27324, 27400,
    27477, 27553, 27629, 27705, 27780, 27856, 27931, 28006, 28081, 28155, 28230, 28304,
    28378, 28452, 28525, 28599, 28672, 28745, 28818, 28891, 28963, 29035, 29108, 29180,
    29251, 29323, 29394, 29466, 29537, 29608, 29678, 29749, 29819, 29890, 29960, 30030,
    30099, 30169, 30238, 30308, 30377, 30446, 30515, 30583, 30652, 30720, 30788, 30856,
    30924, 30992, 31059, 31127, 31194, 31261, 31328, 31395, 31462, 31529, 31595, 31661,
    31727, 31794, 31859, 31925, 31991, 32056, 32122, 32187, 32252, 32317, 32382, 32446,
    32511, 32575, 32640, 32704, 32768, 32832, 32896, 32959, 33023, 33086, 33150, 33213,
    33276, 33339, 33402, 33465, 33527, 33590, 33652, 33714, 33776, 33839, 33900, 33962,
    34024, 34086, 34147, 34208, 34270, 34331, 34392, 34453, 34514, 34574, 34635, 34695,
    34756, 34816, 34876, 34936, 34996, 35056, 35116, 35176, 35235, 35295, 35354, 35413,
    35472, 35531, 35590, 35649, 35708, 35767, 35825, 35884, 35942, 36001, 36059, 36117,
    36175, 36233, 36291, 36348, 36406, 36464, 36521, 36578, 36636, 36693, 36750, 36807,
    36864, 36921, 36978, 37034, 37091, 37147, 37204, 37260, 37316, 37372, 37429, 37485,
    37540, 37596, 37652, 37708, 37763, 37819, 37874, 37929, 37985, 38040, 38095, 38150,
    38205, 38260, 38315, 38369, 38424, 38478, 38533, 38587, 38642, 38696, 38750, 38804,
    38858, 38912, 38966, 39020, 39073, 39127, 39181, 39234, 39287, 39341, 39394, 39447,
    39500, 39553, 39606, 39659, 39712, 39765, 39818, 39870, 39923, 39975, 40028, 40080,
    40132, 40185, 40237, 40289, 40341, 40393, 40445, 40497, 40548, 40600, 40652, 40703,
    40755, 40806, 40857, 40909, 40960, 41011, 41062, 41113, 41164, 41215, 41266, 41317,
    41368, 41418, 41469, 41519, 41570, 41620, 41671, 41721, 41771, 41821, 41871, 41922,
    41972, 42021, 42071, 42121, 42171, 42221, 42270, 42320, 42369, 42419, 42468, 42518,
    42567, 42616, 42665, 42714, 42763, 42813, 42861, 42910, 42959, 43008, 43057, 43105,
    43154, 43203, 43251, 43300, 43348, 43396, 43445, 43493, 43541, 43589, 43637, 43685,
    43733, 43781, 43829, 43877, 43925, 43972, 44020, 44068, 44115, 44163, 44210, 44258,
    44305, 44352, 44400, 44447, 44494, 44541, 44588, 44635, 44682, 44729, 44776, 44823,
    44869, 44916, 44963, 45009, 45056, 45103, 45149, 45195, 45242, 45288, 45334, 45381,
    45427, 45473, 45519, 45565, 45611, 45657, 45703, 45749, 45795, 45840, 45886, 45932,
    45977, 46023, 46069, 46114, 46160, 46205, 46250, 46296, 46341, 46386, 46431, 46477,
    46522, 46567, 46612, 46657, 46702, 46746, 46791, 46836, 46881, 46926, 46970, 47015,
    47059, 47104, 47149, 47193, 47237, 47282, 47326, 47370, 47415, 47459, 47503, 47547,
    47591, 47635, 47679, 47723, 47767, 47811, 47855, 47899, 47942, 47986, 48030, 48074,
    48117, 48161, 48204, 48248, 48291, 48335, 48378, 48421, 48465, 48508, 48551, 48594,
    48637, 48680, 48723, 48766, 48809, 48852, 48895, 48938, 48981, 49024, 49067, 49109,
    49152, 49195, 49237, 49280, 49322, 49365, 49407, 49450, 49492, 49535, 49577, 49619,
    49661, 49704, 49746, 49788, 49830, 49872, 49914, 49956, 49998, 50040, 50082, 50124,
    50166, 50207, 50249, 50291, 50332, 50374, 50416, 50457, 50499, 50540, 50582, 50623,
    50665, 50706, 50747, 50789, 50830, 50871, 50912, 50954, 50995, 51036, 51077, 51118,
    51159, 51200, 51241, 51282, 51323, 51364, 51404, 51445, 51486, 51527, 51567, 51608,
    51649, 51689, 51730, 51770, 51811, 51851, 51892, 51932, 51972, 52013, 52053, 52093,
    52134, 52174, 52214, 52254, 52294, 52334, 52374, 52414, 52454, 52494, 52534, 52574,
    52614, 52654, 52694, 52734, 52773, 52813, 52853, 52892, 52932, 52972, 53011, 53051,
    53090, 53130, 53169, 53209, 53248, 53287, 53327, 53366, 53405, 53445, 53484, 53523,
    53562, 53601, 53640, 53679, 53719, 53758, 53797, 53836, 53874, 53913, 53952, 53991,
    54030, 54069, 54108, 54146, 54185, 54224, 54262, 54301, 54340, 54378, 54417, 54455,
    54494, 54532, 54571, 54609, 54647, 54686, 54724, 54762, 54801, 54839, 54877, 54915,
    54954, 54992, 55030, 55068, 55106, 55144, 55182, 55220, 55258, 55296, 55334, 55372,
    55410, 55447, 55485, 55523, 55561, 55599, 55636, 55674, 55712, 55749, 55787, 55824,
    55862, 55900, 55937, 55975, 56012, 56049, 56087, 56124, 56162, 56199, 56236, 56273,
    56311, 56348, 56385, 56422, 56459, 56497, 56534, 56571, 56608, 56645, 56682, 56719,
    56756, 56793, 56830, 56867, 56903, 56940, 56977, 57014, 57051, 57087, 57124, 57161,
    57198, 57234, 57271, 57307, 57344, 57381, 57417, 57454, 57490, 57527, 57563, 57599,
    57636, 57672, 57709, 57745, 57781, 57817, 57854, 57890, 57926, 57962, 57999, 58035,
    58071, 58107, 58143, 58179, 58215, 58251, 58287, 58323, 58359, 58395, 58431, 58467,
    58503, 58538, 58574, 58610, 58646, 58682, 58717, 58753, 58789, 58824, 58860, 58896,
    58931, 58967, 59002, 59038, 59073, 59109, 59144, 59180, 59215, 59251, 59286, 59321,
    59357, 59392, 59427, 59463, 59498, 59533, 59568, 59603, 59639, 59674, 59709, 59744,
    59779, 59814, 59849, 59884, 59919, 59954, 59989, 60024, 60059, 60094, 60129, 60164,
    60199, 60233, 60268, 60303, 60338, 60373, 60407, 60442, 60477, 60511, 60546, 60581,
    60615, 60650, 60684, 60719, 60753, 60788, 60822, 60857, 60891, 60926, 60960, 60995,
    61029, 61063, 61098, 61132, 61166, 61201, 61235, 61269, 61303, 61338, 61372, 61406,
    61440, 61474, 61508, 61542, 61576, 61610, 61644, 61678, 61712, 61746, 61780, 61814,
    61848, 61882, 61916, 61950, 61984, 62018, 62051, 62085, 62119, 62153, 62186, 62220,
    62254, 62287, 62321, 62355, 62388, 62422, 62456, 62489, 62523, 62556, 62590, 62623,
    62657, 62690, 62724, 62757, 62790, 62824, 62857, 62891, 62924, 62957, 62991, 63024,
    63057, 63090, 63124, 63157, 63190, 63223, 63256, 63289, 63323, 63356, 63389, 63422,
    63455, 63488, 63521, 63554, 63587, 63620, 63653, 63686, 63719, 63752, 63785, 63817,
    63850, 63883, 63916, 63949, 63982, 64014, 64047, 64080, 64113, 64145, 64178, 64211,
    64243, 64276, 64309, 64341, 64374, 64406, 64439, 64471, 64504, 64536, 64569, 64601,
    64634, 64666, 64699, 64731, 64763, 64796, 64828, 64861, 64893, 64925, 64957, 64990,
    65022, 65054, 65086, 65119, 65151, 65183, 65215, 65247, 65279, 65312, 65344, 65376,
    65408, 65440, 65472, 65504,
};

const std::int8_t g_elder_bit_table[256] = {
    0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
};

// The server's int arithmetic wraps on overflow; C++ calls that undefined, so
// the sums the gradient code forms from unvalidated wire coordinates are
// formed unsigned, which wraps identically.
int add32(int a, int b)
{
    return static_cast<int>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

int sub32(int a, int b)
{
    return static_cast<int>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

// `unsigned(v)` on arm64 (FCVTZU): NaN and negatives go to 0, values past
// the top saturate. agg::uround (agg_basics.h:192-195) is
// `unsigned(v + 0.5)` returned as an int.
std::uint32_t to_uint32(double v)
{
    if (!(v > 0.0))
        return 0;
    if (v >= 4294967296.0)
        return 0xffffffffu;
    return static_cast<std::uint32_t>(v);
}

int agg_uround(double v)
{
    return static_cast<int>(to_uint32(v + 0.5));
}

// gradient_radial_focus, agg_span_gradient.h:184-309. Painter constructs it
// with no arguments and never calls init() (Painter.cpp:2102-2112): radius
// 100 (1600 in gradient subpixels), focus (0, 0).
class GradientRadialFocus {
public:
    GradientRadialFocus()
        : radius_(100 * gradient_subpixel_scale)
        , focus_x_(0)
        , focus_y_(0)
    {
        update_values();
    }

    [[nodiscard]] int calculate(int x, int y) const
    {
        double solution_x;
        double solution_y;

        // Special case to avoid divide by zero or very near zero
        if (x == iround(focus_x_)) {
            solution_x = focus_x_;
            solution_y = 0.0;
            solution_y += (y > focus_y_) ? trivial_ : -trivial_;
        } else {
            // Slope of the focus-current line
            const double slope = double(sub32(y, focus_y_)) / double(sub32(x, focus_x_));
            // y-intercept of that same line
            const double yint = double(y) - (slope * x);
            // Use the classical quadratic formula to calculate the
            // intersection point
            const double a = (slope * slope) + 1;
            const double b = 2 * slope * yint;
            const double c = yint * yint - radius2_;
            const double det = std::sqrt((b * b) - (4.0 * a * c));
            solution_x = -b;
            // Choose the positive or negative root depending on where the X
            // coord lies with respect to the focus.
            solution_x += (x < focus_x_) ? -det : det;
            solution_x /= 2.0 * a;
            // Calculating of Y is trivial
            solution_y = (slope * solution_x) + yint;
        }

        // Calculate the percentage (0...1) of the current point along the
        // focus-circumference line and return the normalized (0...d) value
        solution_x -= double(focus_x_);
        solution_y -= double(focus_y_);
        const double int_to_focus = solution_x * solution_x + solution_y * solution_y;
        const double cur_to_focus = double(sub32(x, focus_x_)) * double(sub32(x, focus_x_))
            + double(sub32(y, focus_y_)) * double(sub32(y, focus_y_));

        return iround(std::sqrt(cur_to_focus / int_to_focus) * radius_);
    }

private:
    void update_values()
    {
        radius2_ = double(radius_) * double(radius_);
        const double dist = std::sqrt(double(focus_x_) * double(focus_x_)
                                      + double(focus_y_) * double(focus_y_));
        const double r = radius_ * 0.99;
        if (dist > r) {
            const double a = std::atan2(double(focus_y_), double(focus_x_));
            focus_x_ = iround(r * std::cos(a));
            focus_y_ = iround(r * std::sin(a));
        }
        trivial_ = std::sqrt(radius2_ - (focus_x_ * focus_x_));
    }

    int radius_;
    int focus_x_;
    int focus_y_;
    double radius2_ = 0;
    double trivial_ = 0;
};

const GradientRadialFocus& radial_focus()
{
    static const GradientRadialFocus function;
    return function;
}

} // namespace

// agg_math.h:261-335, the portable branch.
unsigned fast_sqrt(unsigned val)
{
    const unsigned t = val;
    int bit = 0;
    unsigned shift = 11;

    bit = static_cast<int>(t >> 24);
    if (bit) {
        bit = g_elder_bit_table[bit] + 24;
    } else {
        bit = static_cast<int>((t >> 16) & 0xFF);
        if (bit) {
            bit = g_elder_bit_table[bit] + 16;
        } else {
            bit = static_cast<int>((t >> 8) & 0xFF);
            if (bit)
                bit = g_elder_bit_table[bit] + 8;
            else
                bit = g_elder_bit_table[t];
        }
    }

    // This is calculation sqrt itself.
    bit -= 9;
    if (bit > 0) {
        bit = (bit >> 1) + (bit & 1);
        shift -= static_cast<unsigned>(bit);
        val >>= (bit << 1);
    }
    return g_sqrt_table[val] >> shift;
}

// dda2_line_interpolator(y1, y2, count), agg_dda_line.h:95-109
Dda2LineInterpolator::Dda2LineInterpolator(int y1, int y2, int count)
    : cnt_(count <= 0 ? 1 : count)
    , lft_(sub32(y2, y1) / cnt_)
    , rem_(sub32(y2, y1) % cnt_)
    , mod_(rem_)
    , y_(y1)
{
    if (mod_ <= 0) {
        mod_ += count;
        rem_ += count;
        lft_--;
    }
    mod_ -= count;
}

// agg_dda_line.h:159-168
void Dda2LineInterpolator::operator++()
{
    mod_ += rem_;
    y_ = add32(y_, lft_);
    if (mod_ > 0) {
        mod_ -= cnt_;
        y_ = add32(y_, 1);
    }
}

// span_interpolator_linear::begin, agg_span_interpolator_linear.h:54-73
void SpanInterpolatorLinear::begin(double x, double y, unsigned len)
{
    double tx = x;
    double ty = y;
    trans_.transform(&tx, &ty);
    const int x1 = iround(tx * subpixel_scale);
    const int y1 = iround(ty * subpixel_scale);

    tx = x + len;
    ty = y;
    trans_.transform(&tx, &ty);
    const int x2 = iround(tx * subpixel_scale);
    const int y2 = iround(ty * subpixel_scale);

    li_x_ = Dda2LineInterpolator(x1, x2, static_cast<int>(len));
    li_y_ = Dda2LineInterpolator(y1, y2, static_cast<int>(len));
}

SpanGradient::SpanGradient(const TransAffine& transform, GradientShape shape,
                           double d1, double d2, int color_count)
    : interpolator_(transform)
    , shape_(shape)
    , d1_(iround(d1 * gradient_subpixel_scale))
    , d2_(iround(d2 * gradient_subpixel_scale))
    , color_count_(color_count)
{
}

// The gradient functions, agg_span_gradient.h:162-169 (radial), 184-309
// (radial focus), 314-318 (x), 330-339 (diamond) and 365-372 (conic).
int SpanGradient::calculate(int x, int y) const
{
    switch (shape_) {
    case GradientShape::x:
        return x;
    case GradientShape::radial: {
        // int(fast_sqrt(x*x + y*y)): the int products wrap on the server.
        const auto ux = static_cast<std::uint32_t>(x);
        const auto uy = static_cast<std::uint32_t>(y);
        return static_cast<int>(fast_sqrt(ux * ux + uy * uy));
    }
    case GradientShape::radial_focus:
        return radial_focus().calculate(x, y);
    case GradientShape::diamond: {
        const int ax = std::abs(x);
        const int ay = std::abs(y);
        return ax > ay ? ax : ay;
    }
    case GradientShape::conic:
        return agg_uround(std::fabs(std::atan2(double(y), double(x))) * double(d2_) / pi);
    }
    return 0;
}

// span_gradient::generate, agg_span_gradient.h:89-107
void SpanGradient::generate(int* indices, int x, int y, unsigned len)
{
    int dd = sub32(d2_, d1_);
    if (dd < 1)
        dd = 1;
    interpolator_.begin(x + 0.5, y + 0.5, len);
    do {
        interpolator_.coordinates(&x, &y);
        int d = calculate(x >> downscale_shift, y >> downscale_shift);
        d = static_cast<int>(static_cast<std::int64_t>(static_cast<std::int32_t>(
                static_cast<std::uint32_t>(sub32(d, d1_)) * static_cast<std::uint32_t>(color_count_)))
            / dd);
        if (d < 0)
            d = 0;
        if (d >= color_count_)
            d = color_count_ - 1;
        *indices++ = d;
        ++interpolator_;
    } while (--len);
}

} // namespace haiku_remote::agg
