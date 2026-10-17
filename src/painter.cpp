#include "painter.hpp"

#include "agg_port.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

// References are to the DeBeOS tree under src/servers/app/drawing/:
// DrawingEngine.cpp, Painter/Painter.cpp, Painter/Painter.h,
// Painter/drawing_modes/*.h, Painter/agg_renderer_region.h, PatternHandler.h,
// drawing_support.h; and headers/libs/agg/agg_renderer_base.h,
// agg_renderer_scanline.h. tools/painter_oracle/ compiles those very sources
// on the host, and tests/render_tests.cpp checks this file against them for
// every pixel of every scene in tests/painter_scenes.hpp.
//
// Two server settings are not on the wire and are taken at app_server's
// defaults (DesktopSettings.cpp:88-91): subpixel antialiasing is on, which is
// what selects Painter's subpixel rasteriser for every solid shape
// (Painter.cpp:2041-2045) -- and what the client's text already assumes -- and
// the subpixel order is RGB.
//
// The view's B_SUBPIXEL_PRECISE flag is DrawState::subpixel_precise, which is
// fSubpixelPrecise (Painter.cpp:311). A stock app_server never sends it --
// RemoteDrawingEngine::SetDrawState forwards pen, modes, pattern, stroke mode,
// colours, font and transform and nothing else (RemoteDrawingEngine.cpp:
// 209-224) -- so there it stays at the DrawState default, false
// (DrawState.cpp:61). A server with RP_CAP_SUBPIXEL_PRECISE sends it as
// RP_SET_SUBPIXEL_PRECISE (PROTOCOL.md), and HaikuControlLook sets it for
// slider triangles and pop-up markers (HaikuControlLook.cpp:1468, 3008).
//
// The canvas is kept opaque: every write stores alpha 255. The framebuffer's
// alpha byte is invisible on screen, blend_line32 leaves it uninitialised
// (drawing_support.h:39-67), and with it at 255 BLEND_COMPOSITE always takes
// the branch it takes for an opaque destination (DrawingMode.h:107-111).

namespace haiku_remote::painter {

namespace {

using agg::VertexList;

constexpr bool subpixel_ordering_rgb = true; // gSubpixelOrderingRGB

// ---- BPoint / BRect, with Haiku's float semantics -----------------------

struct BPoint {
    float x = 0;
    float y = 0;

    bool operator==(const BPoint&) const = default;
};

struct BRect {
    float left = 0;
    float top = 0;
    float right = -1;
    float bottom = -1;

    [[nodiscard]] bool IsValid() const { return left <= right && top <= bottom; }
    [[nodiscard]] float Width() const { return right - left; }
    [[nodiscard]] float Height() const { return bottom - top; }

    void InsetBy(float dx, float dy)
    {
        left += dx;
        right -= dx;
        top += dy;
        bottom -= dy;
    }

    // BRect::OffsetTo, src/kits/interface/Rect.cpp:169-175
    void OffsetTo(float x, float y)
    {
        right = (right - left) + x;
        left = x;
        bottom = (bottom - top) + y;
        top = y;
    }
};

BRect to_brect(Rect r) { return {r.left, r.top, r.right, r.bottom}; }
BPoint to_bpoint(Point p) { return {p.x, p.y}; }

// (int32)value on the server's arm64: FCVTZS saturates and maps NaN to 0.
std::int32_t to_int32(float value)
{
    return agg::saturating_int(static_cast<double>(value));
}

struct IntBox {
    int left;
    int top;
    int right;
    int bottom;
};

// ---- the clipping region --------------------------------------------------

// fClippingRegion, as the client knows it: the token's
// RP_CONSTRAIN_CLIPPING_REGION rects, or the whole canvas when none ever
// arrived. Each rect is also clipped to the canvas, which the server's
// regions never exceed (they are visible regions of a screen) but a message
// off the wire might.
struct ClipRegion {
    std::vector<IntBox> rects;

    [[nodiscard]] bool frame_valid() const { return !rects.empty(); }

    // BRegion::FrameInt()
    [[nodiscard]] IntBox frame() const
    {
        IntBox f = rects.front();
        for (const auto& r : rects) {
            f.left = std::min(f.left, r.left);
            f.top = std::min(f.top, r.top);
            f.right = std::max(f.right, r.right);
            f.bottom = std::max(f.bottom, r.bottom);
        }
        return f;
    }

    // BRegion::Contains(BPoint), Region.cpp:268-271
    [[nodiscard]] bool contains(BPoint p) const
    {
        const int x = to_int32(p.x);
        const int y = to_int32(p.y);
        return std::any_of(rects.begin(), rects.end(), [&](const IntBox& r) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        });
    }
};

ClipRegion clip_region(Canvas canvas, const DrawState& state)
{
    ClipRegion region;
    const IntBox bounds {0, 0, canvas.width - 1, canvas.height - 1};
    if (!state.clipping_set) {
        region.rects.push_back(bounds);
        return region;
    }
    for (const auto& rect : state.clip_rects) {
        const auto r = raster_bounds(rect);
        const IntBox clipped {std::max(r.left, bounds.left), std::max(r.top, bounds.top),
                              std::min(r.right, bounds.right),
                              std::min(r.bottom, bounds.bottom)};
        if (clipped.left <= clipped.right && clipped.top <= clipped.bottom)
            region.rects.push_back(clipped);
    }
    return region;
}

// ---- PatternHandler, PatternHandler.h -----------------------------------

struct PatternHandler {
    std::array<std::uint8_t, 8> pattern {};
    Color high;
    Color low;
    std::int32_t x_offset = 0;
    std::int32_t y_offset = 0;

    // IsHighColor(int, int), PatternHandler.h:161-170
    [[nodiscard]] bool is_high(int x, int y) const
    {
        x -= static_cast<std::uint16_t>(x_offset);
        y -= static_cast<std::uint16_t>(y_offset);
        return (pattern[static_cast<std::size_t>(y & 7)] & (1 << (7 - (x & 7)))) != 0;
    }
    [[nodiscard]] Color color_at(int x, int y) const { return is_high(x, y) ? high : low; }
    // fPattern == B_SOLID_HIGH / B_SOLID_LOW: all 0xff, all 0x00. Not "all
    // bytes equal" (CLAUDE.md).
    [[nodiscard]] bool solid_high() const
    {
        return std::all_of(pattern.begin(), pattern.end(), [](auto b) { return b == 0xff; });
    }
    [[nodiscard]] bool solid_low() const
    {
        return std::all_of(pattern.begin(), pattern.end(), [](auto b) { return b == 0x00; });
    }
    [[nodiscard]] bool solid() const { return solid_high() || solid_low(); }
};

// ---- the drawing modes' blend macros, DrawingMode.h ---------------------

using u8 = std::uint8_t;

inline void assign(u8* d, int r, int g, int b)
{
    d[0] = static_cast<u8>(b);
    d[1] = static_cast<u8>(g);
    d[2] = static_cast<u8>(r);
    d[3] = 255;
}

// BLEND, DrawingMode.h:28-36
inline void blend(u8* d, int r, int g, int b, int a)
{
    const int d0 = d[0];
    const int d1 = d[1];
    const int d2 = d[2];
    d[0] = static_cast<u8>((((b - d0) * a) + (d0 << 8)) >> 8);
    d[1] = static_cast<u8>((((g - d1) * a) + (d1 << 8)) >> 8);
    d[2] = static_cast<u8>((((r - d2) * a) + (d2 << 8)) >> 8);
    d[3] = 255;
}

// BLEND_SUBPIX, :39-47
inline void blend_subpix(u8* d, int r, int g, int b, int a1, int a2, int a3)
{
    const int d0 = d[0];
    const int d1 = d[1];
    const int d2 = d[2];
    d[0] = static_cast<u8>((((b - d0) * a1) + (d0 << 8)) >> 8);
    d[1] = static_cast<u8>((((g - d1) * a2) + (d1 << 8)) >> 8);
    d[2] = static_cast<u8>((((r - d2) * a3) + (d2 << 8)) >> 8);
    d[3] = 255;
}

// BLEND_FROM, :56-62: blends between two colours and ignores the destination.
inline void blend_from(u8* d, int r1, int g1, int b1, int r2, int g2, int b2, int a)
{
    d[0] = static_cast<u8>((((b2 - b1) * a) + (b1 << 8)) >> 8);
    d[1] = static_cast<u8>((((g2 - g1) * a) + (g1 << 8)) >> 8);
    d[2] = static_cast<u8>((((r2 - r1) * a) + (r1 << 8)) >> 8);
    d[3] = 255;
}

// BLEND_FROM_SUBPIX, :64-70
inline void blend_from_subpix(u8* d, int r1, int g1, int b1, int r2, int g2,
                              int b2, int a1, int a2, int a3)
{
    d[0] = static_cast<u8>((((b2 - b1) * a1) + (b1 << 8)) >> 8);
    d[1] = static_cast<u8>((((g2 - g1) * a2) + (g1 << 8)) >> 8);
    d[2] = static_cast<u8>((((r2 - r1) * a3) + (r1 << 8)) >> 8);
    d[3] = 255;
}

// BLEND16, :78-86: alpha in 0..65025, shifted by 16.
inline void blend16(u8* d, int r, int g, int b, int a)
{
    const auto d0 = static_cast<std::int64_t>(d[0]);
    const auto d1 = static_cast<std::int64_t>(d[1]);
    const auto d2 = static_cast<std::int64_t>(d[2]);
    d[0] = static_cast<u8>((((b - d0) * a) + (d0 << 16)) >> 16);
    d[1] = static_cast<u8>((((g - d1) * a) + (d1 << 16)) >> 16);
    d[2] = static_cast<u8>((((r - d2) * a) + (d2 << 16)) >> 16);
    d[3] = 255;
}

// BLEND16_SUBPIX, :89-97
inline void blend16_subpix(u8* d, int r, int g, int b, int a1, int a2, int a3)
{
    const auto d0 = static_cast<std::int64_t>(d[0]);
    const auto d1 = static_cast<std::int64_t>(d[1]);
    const auto d2 = static_cast<std::int64_t>(d[2]);
    d[0] = static_cast<u8>((((b - d0) * a1) + (d0 << 16)) >> 16);
    d[1] = static_cast<u8>((((g - d1) * a2) + (d1 << 16)) >> 16);
    d[2] = static_cast<u8>((((r - d2) * a3) + (d2 << 16)) >> 16);
    d[3] = 255;
}

// BLEND_COMPOSITE, :103-129. The canvas is opaque, so the first branch is the
// one ever taken; the others are kept so the macro reads as the server's.
inline void blend_composite(u8* d, int r, int g, int b, int a)
{
    if (d[3] == 255) {
        blend(d, r, g, b, a);
    } else if (d[3] == 0) {
        d[0] = static_cast<u8>(b);
        d[1] = static_cast<u8>(g);
        d[2] = static_cast<u8>(r);
        d[3] = static_cast<u8>(a);
    } else {
        const auto alpha_rest = static_cast<u8>(255 - a);
        const std::uint32_t alpha_temp = 65025u - alpha_rest * (255u - d[3]);
        const std::uint32_t alpha_dest = d[3] * static_cast<std::uint32_t>(alpha_rest);
        const std::uint32_t alpha_src = 255u * static_cast<std::uint32_t>(a);
        d[0] = static_cast<u8>((d[0] * alpha_dest + static_cast<std::uint32_t>(b) * alpha_src) / alpha_temp);
        d[1] = static_cast<u8>((d[1] * alpha_dest + static_cast<std::uint32_t>(g) * alpha_src) / alpha_temp);
        d[2] = static_cast<u8>((d[2] * alpha_dest + static_cast<std::uint32_t>(r) * alpha_src) / alpha_temp);
        d[3] = static_cast<u8>(alpha_temp / 255);
    }
}

// BLEND_COMPOSITE16, :173-177: alpha in 0..65025, reduced to 8 bits first.
inline void blend_composite16(u8* d, int r, int g, int b, int a)
{
    const auto a8 = static_cast<std::uint16_t>(a / 255);
    blend_composite(d, r, g, b, a8);
}

// BLEND_COMPOSITE16_SUBPIX, :180-186, and the opaque-destination branch of
// BLEND_COMPOSITE_SUBPIX, :132-166.
inline void blend_composite16_subpix(u8* d, int r, int g, int b, int a1, int a2, int a3)
{
    blend_subpix(d, r, g, b, static_cast<std::uint16_t>(a1 / 255),
                 static_cast<std::uint16_t>(a2 / 255),
                 static_cast<std::uint16_t>(a3 / 255));
}

// brightness_for, DrawingMode.h:189-196
inline u8 brightness_for(int red, int green, int blue)
{
    return static_cast<u8>((308 * red + 600 * green + 116 * blue) / 1024);
}

// blend_line32, drawing_support.h:39-67
inline void blend_line32(u8* buffer, int pixels, u8 r, u8 g, u8 b, u8 a)
{
    r = static_cast<u8>((r * a) >> 8);
    g = static_cast<u8>((g * a) >> 8);
    b = static_cast<u8>((b * a) >> 8);
    a = static_cast<u8>(255 - a);
    for (int i = 0; i < pixels; i++) {
        u8* s = buffer + i * 4;
        s[0] = static_cast<u8>(((s[0] * a) >> 8) + b);
        s[1] = static_cast<u8>(((s[1] * a) >> 8) + g);
        s[2] = static_cast<u8>(((s[2] * a) >> 8) + r);
        s[3] = 255;
    }
}

// compare() of DrawingModeSelect.h:30-51
inline bool select_compare(const u8* p, Color high, Color low, Color* result)
{
    if (p[2] == high.r && p[1] == high.g && p[0] == high.b) {
        *result = low;
        return true;
    }
    if (p[2] == low.r && p[1] == low.g && p[0] == low.b) {
        *result = high;
        return true;
    }
    return false;
}

// One destination pixel through one of the arithmetic modes' ASSIGN_x or
// BLEND_x (DrawingModeAdd/Subtract/Blend/Min/Max.h). They differ only here.
inline void arithmetic(DrawingMode mode, u8* p, Color c, int cover)
{
    int r = c.r;
    int g = c.g;
    int b = c.b;
    switch (mode) {
    case DrawingMode::add:
        r = std::min(255, p[2] + r);
        g = std::min(255, p[1] + g);
        b = std::min(255, p[0] + b);
        break;
    case DrawingMode::subtract:
        r = std::max(0, p[2] - r);
        g = std::max(0, p[1] - g);
        b = std::max(0, p[0] - b);
        break;
    case DrawingMode::blend:
        b = (p[0] + b) >> 1;
        g = (p[1] + g) >> 1;
        r = (p[2] + r) >> 1;
        break;
    case DrawingMode::min:
        if (!(brightness_for(r, g, b) < brightness_for(p[2], p[1], p[0])))
            return;
        break;
    case DrawingMode::max:
        if (!(brightness_for(r, g, b) > brightness_for(p[2], p[1], p[0])))
            return;
        break;
    default:
        break;
    }
    if (cover == 255)
        assign(p, r, g, b);
    else
        blend(p, r, g, b, cover);
}

// The subpixel variant of the above (Drawing*SUBPIX.h): BLEND_SUBPIX always,
// with no cover-255 shortcut. Returns whether it wrote (MIN/MAX may not).
inline bool arithmetic_subpix(DrawingMode mode, u8* p, Color c, int a1, int a2, int a3)
{
    int r = c.r;
    int g = c.g;
    int b = c.b;
    switch (mode) {
    case DrawingMode::add:
        r = std::min(255, p[2] + r);
        g = std::min(255, p[1] + g);
        b = std::min(255, p[0] + b);
        break;
    case DrawingMode::subtract:
        r = std::max(0, p[2] - r);
        g = std::max(0, p[1] - g);
        b = std::max(0, p[0] - b);
        break;
    case DrawingMode::blend:
        b = (p[0] + b) >> 1;
        g = (p[1] + g) >> 1;
        r = (p[2] + r) >> 1;
        break;
    case DrawingMode::min:
        if (!(brightness_for(r, g, b) < brightness_for(p[2], p[1], p[0])))
            return false;
        break;
    case DrawingMode::max:
        if (!(brightness_for(r, g, b) > brightness_for(p[2], p[1], p[0])))
            return false;
        break;
    default:
        break;
    }
    blend_subpix(p, r, g, b, a1, a2, a3);
    return true;
}


// The covers of one subpixel triple, as every *SUBPIX.h reads them: with RGB
// ordering subpixelL is 2 and subpixelR is 0 (e.g. DrawingModeCopySUBPIX.h:
// 30-32), and BLEND_SUBPIX applies a1 to blue, a3 to red -- so red takes the
// leftmost subpixel's cover.
// gSubpixelOrderingRGB is a PixelFormat argument rather than a constant only so
// the text tests can reach the BGR arm; every caller in the client passes the
// server default, subpixel_ordering_rgb.
constexpr int subpixel_l_for(bool ordering_rgb) { return ordering_rgb ? 2 : 0; }
constexpr int subpixel_m = 1;
constexpr int subpixel_r_for(bool ordering_rgb) { return ordering_rgb ? 0 : 2; }

// ---- PixelFormat, drawing_modes/PixelFormat.cpp ---------------------------

// The span functions PixelFormat::SetDrawingMode installs (PixelFormat.cpp:
// 158-364), chosen afresh on every call from the current mode and pattern --
// which is what _UpdateDrawingMode keeps them in step with.
class PixelFormat {
public:
    PixelFormat(Canvas canvas, const PatternHandler& pattern, DrawingMode mode,
                bool constant_alpha, std::uint32_t alpha_function,
                bool ordering_rgb = subpixel_ordering_rgb)
        : canvas_(canvas)
        , pattern_(pattern)
        , mode_(mode)
        , constant_(constant_alpha)
        // B_ALPHA_COMPOSITE is 1. The Porter-Duff functions after it are
        // AggCompOpAdapter wrappers around AGG's comp_op_rgba_* (PixelFormat.
        // cpp:331-356), which only offscreen bitmaps use (issue #22); their
        // non-subpixel spans are treated as B_ALPHA_OVERLAY here, as they were
        // before. Their subpixel span is a no-op; see blend_solid_hspan_subpix.
        , composite_(alpha_function == 1)
        , porter_duff_(!constant_alpha && alpha_function > 1)
        , subpixel_l_(subpixel_l_for(ordering_rgb))
        , subpixel_r_(subpixel_r_for(ordering_rgb))
    {
    }

    [[nodiscard]] int width() const { return canvas_.width; }
    [[nodiscard]] int height() const { return canvas_.height; }
    [[nodiscard]] u8* row(int y) const
    {
        return canvas_.bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas_.width) * 4;
    }

    void blend_pixel(int x, int y, Color c, int cover) const;
    void blend_hline(int x, int y, unsigned len, Color c, int cover) const;
    // Returns whether the span wrote any pixel (a mode that skips a pixel --
    // MIN, MAX, SELECT, a solid-low OVER, the Porter-Duff no-op -- does not).
    bool blend_solid_hspan_subpix(int x, int y, unsigned len, Color c, const u8* covers) const;
    void blend_color_hspan(int x, int y, unsigned len, const Color* colors,
                           const u8* covers, int cover) const;

private:
    Canvas canvas_;
    const PatternHandler& pattern_;
    DrawingMode mode_;
    bool constant_;
    bool composite_;
    bool porter_duff_;
    int subpixel_l_;
    int subpixel_r_;
};

void PixelFormat::blend_pixel(int x, int y, Color c, int cover) const
{
    u8* p = row(y) + x * 4;
    const PatternHandler& pat = pattern_;
    switch (mode_) {
    case DrawingMode::copy:
        if (pat.solid()) {
            // blend_pixel_copy_solid, DrawingModeCopySolid.h:15-25
            if (cover == 255)
                assign(p, c.r, c.g, c.b);
            else
                blend(p, c.r, c.g, c.b, cover);
        } else {
            // blend_pixel_copy, DrawingModeCopy.h:31-44
            const Color color = pat.color_at(x, y);
            if (cover == 255) {
                assign(p, color.r, color.g, color.b);
            } else if (cover != 0) {
                const Color l = pat.low;
                blend_from(p, l.r, l.g, l.b, color.r, color.g, color.b, cover);
            }
        }
        break;
    case DrawingMode::over:
        if (pat.solid()) {
            // blend_pixel_over_solid, DrawingModeOverSolid.h:15-28
            if (pat.solid_low())
                return;
            if (cover == 255)
                assign(p, c.r, c.g, c.b);
            else
                blend(p, c.r, c.g, c.b, cover);
        } else if (pat.is_high(x, y)) {
            // blend_pixel_over, DrawingModeOver.h:30-43
            const Color color = pat.high;
            if (cover == 255)
                assign(p, color.r, color.g, color.b);
            else
                blend(p, color.r, color.g, color.b, cover);
        }
        break;
    case DrawingMode::erase:
        // blend_pixel_erase, DrawingModeErase.h:30-43
        if (pat.is_high(x, y)) {
            const Color color = pat.low;
            if (cover == 255)
                assign(p, color.r, color.g, color.b);
            else
                blend(p, color.r, color.g, color.b, cover);
        }
        break;
    case DrawingMode::invert:
        // blend_pixel_invert, DrawingModeInvert.h:35-47
        if (pat.is_high(x, y)) {
            if (cover == 255)
                assign(p, 255 - p[2], 255 - p[1], 255 - p[0]);
            else
                blend(p, 255 - p[2], 255 - p[1], 255 - p[0], cover);
        }
        break;
    case DrawingMode::select:
        // blend_pixel_select, DrawingModeSelect.h:54-71
        if (pat.is_high(x, y)) {
            Color color;
            if (select_compare(p, pat.high, pat.low, &color)) {
                if (cover == 255)
                    assign(p, color.r, color.g, color.b);
                else
                    blend(p, color.r, color.g, color.b, cover);
            }
        }
        break;
    case DrawingMode::alpha: {
        if (constant_) {
            const int alpha = static_cast<std::uint16_t>(pat.high.a * cover);
            const Color color = (!composite_ && pat.solid()) ? c : pat.color_at(x, y);
            if (alpha == 255 * 255)
                assign(p, color.r, color.g, color.b);
            else if (composite_)
                blend_composite16(p, color.r, color.g, color.b, alpha); // alpha_cc
            else
                blend16(p, color.r, color.g, color.b, alpha); // alpha_co(_solid)
        } else {
            const Color color = pat.solid() ? c : pat.color_at(x, y);
            const int alpha = static_cast<std::uint16_t>(color.a * cover);
            if (alpha == 255 * 255)
                assign(p, color.r, color.g, color.b);
            else if (composite_)
                blend_composite16(p, color.r, color.g, color.b, alpha); // alpha_pc(_solid)
            else
                blend16(p, color.r, color.g, color.b, alpha); // alpha_po(_solid)
        }
        break;
    }
    default:
        // blend_pixel_add and friends: ColorAt, ASSIGN_x or BLEND_x.
        arithmetic(mode_, p, pat.color_at(x, y), cover);
        break;
    }
}

void PixelFormat::blend_hline(int x, int y, unsigned len, Color c, int cover) const
{
    u8* p = row(y) + x * 4;
    const PatternHandler& pat = pattern_;
    switch (mode_) {
    case DrawingMode::copy:
        if (pat.solid()) {
            // blend_hline_copy_solid, DrawingModeCopySolid.h:28-54
            if (cover == 255) {
                do {
                    assign(p, c.r, c.g, c.b);
                    p += 4;
                } while (--len);
            } else {
                do {
                    blend(p, c.r, c.g, c.b, cover);
                    p += 4;
                } while (--len);
            }
        } else {
            // blend_hline_copy, DrawingModeCopy.h:47-91
            if (cover == 255) {
                do {
                    const Color color = pat.is_high(x, y) ? pat.high : pat.low;
                    assign(p, color.r, color.g, color.b);
                    p += 4;
                    x++;
                } while (--len);
            } else if (cover != 0) {
                const Color l = pat.low;
                do {
                    const Color color = pat.color_at(x, y);
                    blend_from(p, l.r, l.g, l.b, color.r, color.g, color.b, cover);
                    x++;
                    p += 4;
                } while (--len);
            }
        }
        break;
    case DrawingMode::over:
        if (pat.solid()) {
            // blend_hline_over_solid, DrawingModeOverSolid.h:31-60
            if (pat.solid_low())
                return;
            if (cover == 255) {
                do {
                    assign(p, c.r, c.g, c.b);
                    p += 4;
                } while (--len);
            } else {
                do {
                    blend(p, c.r, c.g, c.b, cover);
                    p += 4;
                } while (--len);
            }
        } else {
            // blend_hline_over, DrawingModeOver.h:46-77
            const Color color = pat.high;
            do {
                if (pat.is_high(x, y)) {
                    if (cover == 255)
                        assign(p, color.r, color.g, color.b);
                    else
                        blend(p, color.r, color.g, color.b, cover);
                }
                p += 4;
                x++;
            } while (--len);
        }
        break;
    case DrawingMode::erase: {
        // blend_hline_erase, DrawingModeErase.h:46-77
        const Color color = pat.low;
        do {
            if (pat.is_high(x, y)) {
                if (cover == 255)
                    assign(p, color.r, color.g, color.b);
                else
                    blend(p, color.r, color.g, color.b, cover);
            }
            p += 4;
            x++;
        } while (--len);
        break;
    }
    case DrawingMode::invert:
        // blend_hline_invert, DrawingModeInvert.h:50-73
        do {
            if (pat.is_high(x, y)) {
                if (cover == 255)
                    assign(p, 255 - p[2], 255 - p[1], 255 - p[0]);
                else
                    blend(p, 255 - p[2], 255 - p[1], 255 - p[0], cover);
            }
            x++;
            p += 4;
        } while (--len);
        break;
    case DrawingMode::select: {
        // blend_hline_select, DrawingModeSelect.h:74-101
        Color color;
        do {
            if (pat.is_high(x, y) && select_compare(p, pat.high, pat.low, &color)) {
                if (cover == 255)
                    assign(p, color.r, color.g, color.b);
                else
                    blend(p, color.r, color.g, color.b, cover);
            }
            x++;
            p += 4;
        } while (--len);
        break;
    }
    case DrawingMode::alpha:
        if (constant_ && composite_) {
            // blend_hline_alpha_cc, DrawingModeAlphaCC.h:45-88
            const int alpha = static_cast<std::uint16_t>(pat.high.a * cover);
            if (alpha == 255 * 255) {
                do {
                    const Color color = pat.is_high(x, y) ? pat.high : pat.low;
                    assign(p, color.r, color.g, color.b);
                    p += 4;
                    x++;
                } while (--len);
            } else {
                do {
                    const Color color = pat.color_at(x, y);
                    blend_composite16(p, color.r, color.g, color.b, alpha);
                    x++;
                    p += 4;
                } while (--len);
            }
        } else if (constant_ && pat.solid()) {
            // blend_hline_alpha_co_solid, DrawingModeAlphaCOSolid.h:29-63
            int alpha = static_cast<std::uint16_t>(pat.high.a * cover);
            if (alpha == 255 * 255) {
                do {
                    assign(p, c.r, c.g, c.b);
                    p += 4;
                } while (--len);
            } else if (len < 4) {
                do {
                    blend16(p, c.r, c.g, c.b, alpha);
                    p += 4;
                } while (--len);
            } else {
                alpha = alpha >> 8;
                blend_line32(p, static_cast<int>(len), c.r, c.g, c.b, static_cast<u8>(alpha));
            }
        } else if (constant_) {
            // blend_hline_alpha_co, DrawingModeAlphaCO.h:45-88
            const int alpha = static_cast<std::uint16_t>(pat.high.a * cover);
            if (alpha == 255 * 255) {
                do {
                    const Color color = pat.is_high(x, y) ? pat.high : pat.low;
                    assign(p, color.r, color.g, color.b);
                    p += 4;
                    x++;
                } while (--len);
            } else {
                do {
                    const Color color = pat.color_at(x, y);
                    blend16(p, color.r, color.g, color.b, alpha);
                    x++;
                    p += 4;
                } while (--len);
            }
        } else if (composite_ && pat.solid()) {
            // blend_hline_alpha_pc_solid, DrawingModeAlphaPCSolid.h:45-67
            const int alpha = static_cast<std::uint16_t>(c.a * cover);
            if (alpha == 0)
                return;
            if (alpha == 255 * 255) {
                do {
                    assign(p, c.r, c.g, c.b);
                    p += 4;
                } while (--len);
                return;
            }
            do {
                blend_composite16(p, c.r, c.g, c.b, alpha);
                p += 4;
            } while (--len);
        } else if (composite_) {
            // blend_hline_alpha_pc, DrawingModeAlphaPC.h:45-64
            do {
                const Color color = pat.color_at(x, y);
                const int alpha = static_cast<std::uint16_t>(color.a * cover);
                if (alpha) {
                    if (alpha == 255 * 255)
                        assign(p, color.r, color.g, color.b);
                    else
                        blend_composite16(p, color.r, color.g, color.b, alpha);
                }
                x++;
                p += 4;
            } while (--len);
        } else if (pat.solid()) {
            // blend_hline_alpha_po_solid, DrawingModeAlphaPOSolid.h:29-63.
            // The short-span arm uses BLEND_ALPHA_CO, which is BLEND16 too.
            int alpha = static_cast<std::uint16_t>(c.a * cover);
            if (alpha == 255 * 255) {
                do {
                    assign(p, c.r, c.g, c.b);
                    p += 4;
                } while (--len);
            } else if (len < 4) {
                do {
                    blend16(p, c.r, c.g, c.b, alpha);
                    p += 4;
                } while (--len);
            } else {
                alpha = alpha >> 8;
                blend_line32(p, static_cast<int>(len), c.r, c.g, c.b, static_cast<u8>(alpha));
            }
        } else {
            // blend_hline_alpha_po, DrawingModeAlphaPO.h:45-64. It tests the
            // 16-bit alpha against 255, not 255 * 255 -- so it assigns where
            // color.alpha * cover happens to equal 255 (alpha 255 at cover 1,
            // alpha 85 at cover 3, ...) and blends a fully opaque run. Kept.
            do {
                const Color color = pat.color_at(x, y);
                const int alpha = static_cast<std::uint16_t>(color.a * cover);
                if (alpha) {
                    if (alpha == 255)
                        assign(p, color.r, color.g, color.b);
                    else
                        blend16(p, color.r, color.g, color.b, alpha);
                }
                x++;
                p += 4;
            } while (--len);
        }
        break;
    default:
        // blend_hline_add and friends, e.g. DrawingModeAdd.h:53-78
        do {
            arithmetic(mode_, p, pat.color_at(x, y), cover);
            x++;
            p += 4;
        } while (--len);
        break;
    }
}

bool PixelFormat::blend_solid_hspan_subpix(int x, int y, unsigned len, Color c,
                                           const u8* covers) const
{
    u8* p = row(y) + x * 4;
    const PatternHandler& pat = pattern_;
    // Every *SUBPIX.h loop is `do { ...; covers += 3; len -= 3; } while (len)`.
    const auto step = [&]() {
        covers += 3;
        p += 4;
        x++;
        len -= 3;
        return len != 0;
    };
    const int cl = subpixel_l_;
    const int cm = subpixel_m;
    const int cr = subpixel_r_;
    bool wrote = false;
    // BLEND_SUBPIX(p, r, g, b, covers[L], covers[M], covers[R]), the write
    // every mode but B_OP_ALPHA ends in.
    const auto blend_covers = [&](int r, int g, int b) {
        blend_subpix(p, r, g, b, covers[cl], covers[cm], covers[cr]);
        wrote = true;
    };
    switch (mode_) {
    case DrawingMode::copy:
        if (pat.solid()) {
            // blend_solid_hspan_copy_solid_subpix, DrawingModeCopySolidSUBPIX.h
            do {
                blend_covers(c.r, c.g, c.b);
            } while (step());
        } else {
            // blend_solid_hspan_copy_subpix, DrawingModeCopySUBPIX.h
            const Color l = pat.low;
            do {
                const Color color = pat.color_at(x, y);
                blend_from_subpix(p, l.r, l.g, l.b, color.r, color.g, color.b,
                                  covers[cl], covers[cm], covers[cr]);
                wrote = true;
            } while (step());
        }
        break;
    case DrawingMode::over:
        if (pat.solid()) {
            // blend_solid_hspan_over_solid_subpix, DrawingModeOverSolidSUBPIX.h
            if (pat.solid_low())
                return false;
            do {
                blend_covers(c.r, c.g, c.b);
            } while (step());
        } else {
            // blend_solid_hspan_over_subpix, DrawingModeOverSUBPIX.h
            const Color color = pat.high;
            do {
                if (pat.is_high(x, y))
                    blend_covers(color.r, color.g, color.b);
            } while (step());
        }
        break;
    case DrawingMode::erase: {
        // blend_solid_hspan_erase_subpix, DrawingModeEraseSUBPIX.h
        const Color color = pat.low;
        do {
            if (pat.is_high(x, y))
                blend_covers(color.r, color.g, color.b);
        } while (step());
        break;
    }
    case DrawingMode::invert:
        // blend_solid_hspan_invert_subpix, DrawingModeInvertSUBPIX.h
        do {
            if (pat.is_high(x, y))
                blend_covers(255 - p[2], 255 - p[1], 255 - p[0]);
        } while (step());
        break;
    case DrawingMode::select: {
        // blend_solid_hspan_select_subpix, DrawingModeSelectSUBPIX.h
        Color color;
        do {
            if (pat.is_high(x, y) && select_compare(p, pat.high, pat.low, &color))
                blend_covers(color.r, color.g, color.b);
        } while (step());
        break;
    }
    case DrawingMode::alpha: {
        // B_PIXEL_ALPHA with a Porter-Duff function: SetAggCompOpAdapter
        // (PixelFormat.cpp:331-356) installs AggCompOpAdapter's
        // blend_solid_hspan_subpix, which only prints "not implemented"
        // (AggCompOpAdapter.h:71-79). No pixel changes -- for every solid shape
        // and all subpixel text alike. (B_CONSTANT_ALPHA with such a function
        // installs nothing and keeps whatever spans the previous mode left,
        // which the wire cannot tell us; it stays overlay, as before.)
        if (porter_duff_)
            return false;
        // blend_solid_hspan_alpha_{co,co_solid,cc,po,po_solid,pc}_subpix
        // (PC has no solid subpixel variant, PixelFormat.cpp:319). Unlike
        // every other mode, these put covers[subpixelL] into *alphaRed* and
        // then pass alphaBlue as BLEND's first (blue) argument
        // (e.g. DrawingModeAlphaCOSUBPIX.h:38-43) -- so red and blue take the
        // opposite subpixels from the other modes. Reproduced as is.
        const bool solid = pat.solid() && !composite_;
        do {
            const Color color = solid ? c : pat.color_at(x, y);
            const int a = constant_ ? pat.high.a : color.a;
            const int alpha_red = static_cast<std::uint16_t>(a * covers[cl]);
            const int alpha_green = static_cast<std::uint16_t>(a * covers[cm]);
            const int alpha_blue = static_cast<std::uint16_t>(a * covers[cr]);
            if (composite_)
                blend_composite16_subpix(p, color.r, color.g, color.b,
                                         alpha_blue, alpha_green, alpha_red);
            else
                blend16_subpix(p, color.r, color.g, color.b, alpha_blue,
                               alpha_green, alpha_red);
            wrote = true;
        } while (step());
        break;
    }
    default:
        // blend_solid_hspan_add_subpix and friends
        do {
            wrote |= arithmetic_subpix(mode_, p, pat.color_at(x, y), covers[cl],
                                       covers[cm], covers[cr]);
        } while (step());
        break;
    }
    return wrote;
}

// Gradients always run under SolidPatternGuard (Painter.cpp:2300), so the
// pattern is B_SOLID_HIGH here and the solid variants are the ones installed
// (PixelFormat.cpp:208-224).
void PixelFormat::blend_color_hspan(int x, int y, unsigned len,
                                    const Color* colors, const u8* covers,
                                    int /*cover*/) const
{
    // render_scanline_aa with scanline_u8 always passes a covers array
    // (agg_renderer_scanline.h:151-175), so only the per-pixel arms of the
    // color_hspan functions are reachable.
    u8* p = row(y) + x * 4;
    const PatternHandler& pat = pattern_;
    switch (mode_) {
    case DrawingMode::copy:
        if (pat.solid()) {
            // blend_color_hspan_copy_solid, DrawingModeCopySolid.h:104-143:
            // it ignores the colour's alpha.
            do {
                if (*covers) {
                    if (*covers == 255)
                        assign(p, colors->r, colors->g, colors->b);
                    else
                        blend(p, colors->r, colors->g, colors->b, *covers);
                }
                covers++;
                p += 4;
                ++colors;
            } while (--len);
        } else {
            // blend_color_hspan_copy, DrawingModeCopy.h:145-185, blending
            // towards the low colour. Unreachable while SolidPatternGuard
            // holds; kept so the guard is what selects the solid variant,
            // as it is in app_server.
            const Color l = pat.low;
            do {
                if (*covers) {
                    if (*covers == 255)
                        assign(p, colors->r, colors->g, colors->b);
                    else
                        blend_from(p, l.r, l.g, l.b, colors->r, colors->g,
                                   colors->b, *covers);
                }
                covers++;
                p += 4;
                ++colors;
            } while (--len);
        }
        break;
    case DrawingMode::over:
    case DrawingMode::erase:
    case DrawingMode::invert:
    case DrawingMode::select: {
        // blend_color_hspan_over/erase/invert/select: skip transparent
        // colours, then the mode's ASSIGN or BLEND (DrawingModeOver.h:131-
        // 173, Erase.h:131-178, Invert.h:125-169, Select.h:159-204).
        Color color;
        do {
            if (*covers && colors->a > 0) {
                int r = colors->r;
                int g = colors->g;
                int b = colors->b;
                bool write = true;
                if (mode_ == DrawingMode::erase) {
                    r = pat.low.r;
                    g = pat.low.g;
                    b = pat.low.b;
                } else if (mode_ == DrawingMode::invert) {
                    r = 255 - p[2];
                    g = 255 - p[1];
                    b = 255 - p[0];
                } else if (mode_ == DrawingMode::select) {
                    write = select_compare(p, pat.high, pat.low, &color);
                    r = color.r;
                    g = color.g;
                    b = color.b;
                }
                if (write) {
                    if (*covers == 255)
                        assign(p, r, g, b);
                    else
                        blend(p, r, g, b, *covers);
                }
            }
            covers++;
            p += 4;
            ++colors;
        } while (--len);
        break;
    }
    case DrawingMode::alpha:
        // blend_color_hspan_alpha_{co,cc} (DrawingModeAlphaCO.h:142-183,
        // AlphaCC.h:140-181) scale by the high colour's alpha too;
        // _alpha_{po,pc_solid} (AlphaPO.h:116-156, AlphaPCSolid.h:112-152)
        // do not.
        do {
            const int alpha = constant_
                ? static_cast<std::uint16_t>(pat.high.a * colors->a * *covers / 255)
                : static_cast<std::uint16_t>(colors->a * *covers);
            if (alpha) {
                if (alpha == 255 * 255)
                    assign(p, colors->r, colors->g, colors->b);
                else if (composite_)
                    blend_composite16(p, colors->r, colors->g, colors->b, alpha);
                else
                    blend16(p, colors->r, colors->g, colors->b, alpha);
            }
            covers++;
            p += 4;
            ++colors;
        } while (--len);
        break;
    default:
        // blend_color_hspan_add and friends, e.g. DrawingModeAdd.h:128-170.
        // (DrawingModeBlend.h:126-154 tests `colors->a & 0xff`, the same.)
        do {
            if (*covers && colors->a > 0)
                arithmetic(mode_, p, *colors, *covers);
            covers++;
            p += 4;
            ++colors;
        } while (--len);
        break;
    }
}

// ---- renderer_region, agg_renderer_region.h, over renderer_base ---------

// Every span is cut against each rect of the clipping region in turn by
// renderer_base's clip box (agg_renderer_base.h:206-218, :275-318, :391-414),
// so a span crossing two rects reaches the pixel format as two shorter spans
// -- which matters, because some modes branch on the span length.
class RendererRegion {
public:
    RendererRegion(const PixelFormat& pixfmt, const ClipRegion& region)
        : ren_(pixfmt)
        , region_(region)
    {
    }

    void blend_hline(int x1, int y, int x2, Color c, int cover) const
    {
        if (x1 > x2)
            std::swap(x1, x2);
        for (const auto& box : region_.rects) {
            if (y > box.bottom || y < box.top || x1 > box.right || x2 < box.left)
                continue;
            const int a = std::max(x1, box.left);
            const int b = std::min(x2, box.right);
            ren_.blend_hline(a, y, static_cast<unsigned>(b - a + 1), c, cover);
        }
    }

    void blend_solid_hspan_subpix(int x, int y, int len, Color c, const u8* covers) const
    {
        for (const auto& box : region_.rects) {
            int sx = x;
            int slen = len;
            const u8* sc = covers;
            if (y > box.bottom || y < box.top)
                continue;
            if (sx < box.left) {
                slen -= 3 * (box.left - sx);
                if (slen <= 0)
                    continue;
                sc += 3 * (box.left - sx);
                sx = box.left;
            }
            if (sx + slen / 3 > box.right) {
                slen = 3 * (box.right - sx + 1);
                if (slen <= 0)
                    continue;
            }
            ren_.blend_solid_hspan_subpix(sx, y, static_cast<unsigned>(slen), c, sc);
        }
    }

    void blend_color_hspan(int x, int y, int len, const Color* colors,
                           const u8* covers, int cover) const
    {
        for (const auto& box : region_.rects) {
            int sx = x;
            int slen = len;
            const u8* sc = covers;
            const Color* scol = colors;
            if (y > box.bottom || y < box.top)
                continue;
            if (sx < box.left) {
                const int d = box.left - sx;
                slen -= d;
                if (slen <= 0)
                    continue;
                if (sc)
                    sc += d;
                scol += d;
                sx = box.left;
            }
            if (sx + slen > box.right) {
                slen = box.right - sx + 1;
                if (slen <= 0)
                    continue;
            }
            ren_.blend_color_hspan(sx, y, static_cast<unsigned>(slen), scol, sc, cover);
        }
    }

private:
    const PixelFormat& ren_;
    const ClipRegion& region_;
};

// render_scanline_subpix_solid, Painter/agg_renderer_scanline_subpix.h:21-49
class SubpixRenderer : public agg::ScanlineSink {
public:
    SubpixRenderer(const RendererRegion& ren, Color color) : ren_(ren), color_(color) {}
    void render(int y, const agg::Span* spans, unsigned count) override
    {
        for (unsigned i = 0; i < count; ++i) {
            const int x = spans[i].x;
            if (spans[i].len > 0)
                ren_.blend_solid_hspan_subpix(x, y, spans[i].len, color_, spans[i].covers);
            else
                ren_.blend_hline(x, y, x - (spans[i].len / 3) - 1, color_,
                                 *spans[i].covers);
        }
    }

private:
    const RendererRegion& ren_;
    Color color_;
};

// ---- gradient colour --------------------------------------------------------

// rgba8::gradient, agg_color_rgba.h:351-360, in its own unsigned arithmetic:
// calc_type is int32u, so a falling channel's negative difference wraps, is
// multiplied and shifted as an unsigned and is truncated back to eight bits
// -- which is what makes it come out right.
Color rgba8_gradient(Color self, Color c, double k)
{
    using u32 = std::uint32_t;
    // uround, agg_basics.h:192-195: unsigned(v + 0.5), which on arm64
    // (FCVTZU) takes a negative weight to 0 and saturates at the top.
    const double v = k * 256.0 + 0.5;
    const u32 ik = !(v > 0.0) ? 0u : v >= 4294967296.0 ? 0xffffffffu : static_cast<u32>(v);
    const auto channel = [ik](std::uint8_t from, std::uint8_t to) {
        return static_cast<std::uint8_t>(
            u32 {from} + (((u32 {to} - from) * ik) >> 8));
    };
    return {channel(self.r, c.r), channel(self.g, c.g), channel(self.b, c.b),
            channel(self.a, c.a)};
}

// The colour table span_gradient indexes: Painter::_MakeGradient(Array&,
// const BGradient&), Painter.cpp:2253-2281, into a pod_auto_array<rgba8,
// 256>. Kept as the server has it:
//
//   - every segment runs from (int)from->offset to (int)to->offset
//     inclusive, and its weight is (to->offset - j) / (dist + 1), so not even
//     the first entry of a segment is exactly the `from` colour -- the "dist
//     + 1" off-by-one -- and the last is the `to` colour only because the
//     weight there is 0;
//   - a segment of zero or negative length is skipped (`dist > 0`), so stops
//     out of order leave their part of the table to whatever wrote it before.
//
// Entries before the first stop and after the last are never written on the
// server: the array lives on the stack uninitialised (Painter.cpp:2304). They
// are the first and last stop's colours here, which is what a gradient that
// does not reach its own ends is meant to show. An offset outside 0..255
// writes outside the array on the server; here the writes stay in range.
std::array<Color, 256> make_gradient_array(const Gradient& gradient)
{
    std::array<Color, 256> array {};
    if (gradient.stops.empty())
        return array;
    const auto first = std::clamp(to_int32(gradient.stops.front().offset), 0, 255);
    const auto last = std::clamp(to_int32(gradient.stops.back().offset), 0, 255);
    for (int j = 0; j < first; ++j)
        array[static_cast<std::size_t>(j)] = gradient.stops.front().color;
    for (int j = last + 1; j < 256; ++j)
        array[static_cast<std::size_t>(j)] = gradient.stops.back().color;
    for (std::size_t i = 0; i + 1 < gradient.stops.size(); i++) {
        const GradientStop& from = gradient.stops[i];
        const GradientStop& to = gradient.stops[i + 1];
        const float dist = to.offset - from.offset;
        // TODO: Review this... offset should better be on [0..1]
        if (dist > 0) {
            const int begin = to_int32(from.offset);
            const int end = to_int32(to.offset);
            for (int j = std::max(begin, 0); j <= std::min(end, 255); j++) {
                const float f = static_cast<float>(to.offset - static_cast<float>(j))
                    / static_cast<float>(dist + 1);
                array[static_cast<std::size_t>(j)] = rgba8_gradient(to.color, from.color, f);
            }
        }
    }
    return array;
}

// render_scanline_aa with a span_gradient, agg_renderer_scanline.h:151-175:
// one colour per pixel of each span, from the span generator, then
// blend_color_hspan through the region.
class GradientRenderer : public agg::ScanlineSink {
public:
    GradientRenderer(const RendererRegion& ren, const Gradient& gradient,
                     const agg::TransAffine& gradient_transform,
                     agg::GradientShape shape, int gradient_stop)
        : ren_(ren)
        , colors_table_(make_gradient_array(gradient))
        , span_gradient_(gradient_transform, shape, 0, gradient_stop)
    {
    }

    void render(int y, const agg::Span* spans, unsigned count) override
    {
        for (unsigned i = 0; i < count; ++i) {
            const int x = spans[i].x;
            int len = spans[i].len;
            if (len < 0)
                len = -len;
            indices_.resize(static_cast<std::size_t>(len));
            colors_.resize(static_cast<std::size_t>(len));
            span_gradient_.generate(indices_.data(), x, y, static_cast<unsigned>(len));
            for (int k = 0; k < len; ++k) {
                colors_[static_cast<std::size_t>(k)]
                    = colors_table_[static_cast<std::size_t>(indices_[static_cast<std::size_t>(k)])];
            }
            ren_.blend_color_hspan(x, y, static_cast<unsigned>(len), colors_.data(),
                                   spans[i].len < 0 ? nullptr : spans[i].covers,
                                   *spans[i].covers);
        }
    }

private:
    const RendererRegion& ren_;
    std::array<Color, 256> colors_table_;
    agg::SpanGradient span_gradient_;
    std::vector<int> indices_;
    std::vector<Color> colors_;
};

// The other gradient table: Painter::_MakeGradient(const BGradient&, int32
// colorCount, uint32* colors, int32 arrayOffset, int32 arraySize),
// Painter.cpp:2174-2250, which only FillRectVerticalGradient uses. Its float
// arithmetic is the server's, double literals included: `colorCount *
// offset / 255 + 0.5` is a float plus a double, rounded back to float for
// floorf.
std::vector<Color> make_vertical_gradient(const Gradient& gradient, int color_count,
                                          int array_offset, int array_size)
{
    std::vector<Color> colors(static_cast<std::size_t>(std::max(array_size, 0)));
    if (gradient.stops.empty() || array_size <= 0)
        return colors;
    const auto round_index = [](int count, float offset) {
        const float scaled = static_cast<float>(count) * offset / 255;
        return to_int32(std::floor(static_cast<float>(scaled + 0.5)));
    };
    const auto mix = [](std::uint8_t a, float f, std::uint8_t b, float t) {
        return static_cast<std::uint8_t>(to_int32(
            std::floor(static_cast<float>(a * f + b * t + 0.5))));
    };
    const GradientStop* from = &gradient.stops.front();

    // current index into "colors" array
    int index = round_index(color_count, from->offset) + array_offset;
    if (index > array_size)
        index = array_size;
    // Make sure we fill the entire array in case the gradient is outside.
    for (int i = 0; i < index; i++)
        colors[static_cast<std::size_t>(i)] = from->color;

    // interpolate "from" to "to"
    for (std::size_t stop = 1; stop < gradient.stops.size(); stop++) {
        // find the step with the next offset
        const GradientStop* to = &gradient.stops[stop];

        // interpolate
        int offset = round_index(color_count - 1, to->offset);
        if (offset > color_count - 1)
            offset = color_count - 1;
        offset += array_offset;
        const int dist = offset - index;
        if (dist >= 0) {
            const int start_index = std::max(index, 0);
            const int stop_index = std::min(offset, array_size - 1);
            for (int i = start_index; i <= stop_index; i++) {
                const float f = static_cast<float>(offset - i) / static_cast<float>(dist + 1);
                const auto t = static_cast<float>(1.0 - f);
                colors[static_cast<std::size_t>(i)] = {
                    mix(from->color.r, f, to->color.r, t),
                    mix(from->color.g, f, to->color.g, t),
                    mix(from->color.b, f, to->color.b, t),
                    mix(from->color.a, f, to->color.a, t),
                };
            }
        }
        index = offset + 1;
        // the current "to" will be the "from" in the next interpolation
        from = to;
    }
    //  make sure we fill the entire array
    for (int i = std::max(index, 0); i < array_size; i++)
        colors[static_cast<std::size_t>(i)] = from->color;
    return colors;
}

// ---- Painter ------------------------------------------------------------------

// agg_line_cap_mode_for and agg_line_join_mode_for, Painter.cpp:1924-1954.
// The cap/join words are Haiku's raw enums (types.hpp, LineCap).
agg::LineCap agg_line_cap_mode_for(std::uint32_t mode)
{
    switch (mode) {
    case 3: // B_BUTT_CAP
        return agg::LineCap::butt;
    case 4: // B_SQUARE_CAP
        return agg::LineCap::square;
    case 0: // B_ROUND_CAP
        return agg::LineCap::round;
    default:
        return agg::LineCap::butt;
    }
}

agg::LineJoin agg_line_join_mode_for(std::uint32_t mode)
{
    switch (mode) {
    case 1: // B_MITER_JOIN
        return agg::LineJoin::miter;
    case 0: // B_ROUND_JOIN
        return agg::LineJoin::round;
    case 2: // B_BEVEL_JOIN
    case 3: // B_BUTT_JOIN
    case 4: // B_SQUARE_JOIN
        return agg::LineJoin::bevel;
    default:
        return agg::LineJoin::miter;
    }
}

constexpr std::uint32_t b_square_cap = 4;

class BitmapPainter;

class Painter {
    // Painter.h:346-348: the bitmap painter reads Painter's state directly.
    friend class BitmapPainter;

public:
    Painter(Canvas canvas, const DrawState& state)
        : canvas_(canvas)
        , state_(state)
        , region_(clip_region(canvas, state))
    {
        valid_clipping_ = region_.frame_valid();
        // SetTransform, Painter.cpp:371-383
        identity_ = state.transform.is_identity();
        if (!identity_) {
            transform_ = agg::TransAffine::translation(-state.x_offset, -state.y_offset);
            transform_.multiply({state.transform.sx, state.transform.shy,
                                 state.transform.shx, state.transform.sy,
                                 state.transform.tx, state.transform.ty});
            transform_.multiply(agg::TransAffine::translation(state.x_offset, state.y_offset));
        }
        pen_size_ = state.pen_size;
        // fSubpixelPrecise = state->SubPixelPrecise(), Painter.cpp:311
        subpixel_precise_ = state.subpixel_precise;
        mode_ = state.drawing_mode;
        pattern_.pattern = state.pattern;
        pattern_.high = state.high;
        pattern_.low = state.low;
        pattern_.x_offset = state.x_offset;
        pattern_.y_offset = state.y_offset;
    }

    // -- state the DrawingEngine wrappers change for one call ---------------
    [[nodiscard]] Color high_color() const { return pattern_.high; }
    [[nodiscard]] float pen_size() const { return pen_size_; }
    [[nodiscard]] bool identity() const { return identity_; }
    // CHECK_CLIPPING, Painter.cpp:84
    [[nodiscard]] bool valid_clipping() const { return valid_clipping_; }
    [[nodiscard]] DrawingMode drawing_mode() const { return mode_; }
    void set_high_color(Color c) { pattern_.high = c; }
    void set_drawing_mode(DrawingMode mode) { mode_ = mode; }
    [[nodiscard]] std::array<std::uint8_t, 8> pattern() const { return pattern_.pattern; }
    void set_pattern(const std::array<std::uint8_t, 8>& p) { pattern_.pattern = p; }
    void set_solid_high() { pattern_.pattern.fill(0xff); }

    // -- the rect helpers, Painter.h:378-435 ----------------------------------

    // _Clipped, Painter.cpp:1689-1696
    [[nodiscard]] BRect clipped(BRect rect) const
    {
        if (rect.IsValid()) {
            if (!region_.frame_valid())
                return {0, 0, -1, -1} /* frame of an empty region */;
            const IntBox f = region_.frame();
            return {std::max(rect.left, static_cast<float>(f.left)),
                    std::max(rect.top, static_cast<float>(f.top)),
                    std::min(rect.right, static_cast<float>(f.right)),
                    std::min(rect.bottom, static_cast<float>(f.bottom))};
        }
        return rect;
    }

    [[nodiscard]] BRect transform_and_clip_rect(BRect rect) const
    {
        rect.left = std::floor(rect.left);
        rect.top = std::floor(rect.top);
        rect.right = std::ceil(rect.right);
        rect.bottom = std::ceil(rect.bottom);
        if (!identity_)
            rect = transform_bounds(rect);
        return clipped(rect);
    }

    // TransformAlignAndClipRect, Painter.h:412-418
    [[nodiscard]] BRect transform_align_and_clip_rect(BRect rect) const
    {
        rect = align_rect(rect);
        if (!identity_)
            rect = transform_bounds(rect);
        return clipped(rect);
    }

    [[nodiscard]] BRect clip_rect(BRect rect) const
    {
        rect.left = std::floor(rect.left);
        rect.top = std::floor(rect.top);
        rect.right = std::ceil(rect.right);
        rect.bottom = std::ceil(rect.bottom);
        return clipped(rect);
    }

    // AlignRect, Painter.h:421-434: floor the left/top; floor the
    // right/bottom too, or ceil them for a B_SUBPIXEL_PRECISE view.
    [[nodiscard]] BRect align_rect(BRect rect) const
    {
        rect.left = std::floor(rect.left);
        rect.top = std::floor(rect.top);
        if (subpixel_precise_) {
            rect.right = std::ceil(rect.right);
            rect.bottom = std::ceil(rect.bottom);
        } else {
            rect.right = std::floor(rect.right);
            rect.bottom = std::floor(rect.bottom);
        }
        return rect;
    }

    // DrawTransaction(engine, bounds).IsDirty(), DrawingEngine.cpp:100-151:
    // the bounds, converted by BRegion::Set (floor/ceil, Region.cpp:537-541),
    // intersect the clipping region.
    [[nodiscard]] bool dirty(BRect bounds) const
    {
        const int l = to_int32(std::floor(bounds.left));
        const int t = to_int32(std::floor(bounds.top));
        const int r = to_int32(std::ceil(bounds.right));
        const int b = to_int32(std::ceil(bounds.bottom));
        if (!(l <= r && t <= b))
            return false;
        return std::any_of(region_.rects.begin(), region_.rects.end(), [&](const IntBox& box) {
            return std::max(l, box.left) <= std::min(r, box.right)
                && std::max(t, box.top) <= std::min(b, box.bottom);
        });
    }

    // DrawTransaction(engine), the whole clipping region.
    [[nodiscard]] bool dirty() const { return !region_.rects.empty(); }

    // -- drawing --------------------------------------------------------------

    // Painter::StrokeLine(BPoint, BPoint), Painter.cpp:504-566
    void stroke_line(BPoint a, BPoint b, const Gradient* gradient = nullptr)
    {
        if (!valid_clipping_)
            return;

        // "false" means not to do the pixel center offset, because it would
        // mess up our optimized versions
        align(&a, false);
        align(&b, false);

        if (gradient == nullptr) {
            // first, try an optimized version
            if (pen_size_ == 1.0f && identity_
                && (mode_ == DrawingMode::copy || mode_ == DrawingMode::over)) {
                if (pattern_.solid_high() && straight_line(a, b, pattern_.high))
                    return;
                if (pattern_.solid_low() && straight_line(a, b, pattern_.low))
                    return;
            }
        }

        path_.remove_all();

        if (a == b) {
            // special case dots
            if (gradient == nullptr && pen_size_ == 1.0f && !subpixel_precise_
                && identity_) {
                if (region_.contains(a)) {
                    const int dot_x = to_int32(a.x);
                    const int dot_y = to_int32(a.y);
                    pixel_format().blend_pixel(dot_x, dot_y, renderer_color(), 255);
                }
            } else {
                path_.move_to(a.x, a.y);
                path_.line_to(a.x + 1, a.y);
                path_.line_to(a.x + 1, a.y + 1);
                path_.line_to(a.x, a.y + 1);
                fill_path(path_.vertices(), gradient);
            }
        } else {
            // Do the pixel center offset here
            if (!subpixel_precise_ && std::fmod(pen_size_, 2.0f) != 0.0f) {
                align(&a, true);
                align(&b, true);
            }

            path_.move_to(a.x, a.y);
            path_.line_to(b.x, b.y);

            if (!subpixel_precise_ && pen_size_ == 1.0f) {
                // Tweak ends to "include" the pixel at the index, we need to
                // do this in order to produce results like R5, where
                // coordinates were inclusive
                stroke_path(path_.vertices(), b_square_cap, gradient);
            } else {
                stroke_path(path_.vertices(), state_.line_cap, gradient);
            }
        }
    }

    // Painter::StraightLine, Painter.cpp:612-680: a raw, opaque one-pixel
    // line, iterated over the clipping rects, ignoring mode and pattern.
    bool straight_line(BPoint a, BPoint b, Color c) const
    {
        if (!valid_clipping_)
            return false;

        if (a.x == b.x) {
            // vertical
            const int x = to_int32(a.x);
            const int y1 = to_int32(std::min(a.y, b.y));
            const int y2 = to_int32(std::max(a.y, b.y));
            for (const auto& box : region_.rects) {
                if (box.left <= x && box.right >= x) {
                    const int end = std::min(box.bottom, y2);
                    for (int i = std::max(box.top, y1); i <= end; i++)
                        assign(pixel(x, i), c.r, c.g, c.b);
                }
            }
            return true;
        }

        if (a.y == b.y) {
            // horizontal
            const int y = to_int32(a.y);
            if (y < 0 || y >= canvas_.height)
                return true;
            const int x1 = to_int32(std::min(a.x, b.x));
            const int x2 = to_int32(std::max(a.x, b.x));
            for (const auto& box : region_.rects) {
                if (box.top <= y && box.bottom >= y) {
                    const int end = std::min(box.right, x2);
                    for (int i = std::max(box.left, x1); i <= end; i++)
                        assign(pixel(i, y), c.r, c.g, c.b);
                }
            }
            return true;
        }
        return false;
    }

    // Painter::DrawPolygon, Painter.cpp:720-782
    void draw_polygon(std::vector<BPoint> p, bool filled, bool closed,
                      const Gradient* gradient)
    {
        if (!valid_clipping_ || p.empty())
            return;

        const bool center_offset = !filled && identity_
            && std::fmod(pen_size_, 2.0f) != 0.0f;

        path_.remove_all();

        align(&p[0], center_offset);
        path_.move_to(p[0].x, p[0].y);

        for (std::size_t i = 1; i < p.size(); i++) {
            align(&p[i], center_offset);
            path_.line_to(p[i].x, p[i].y);
        }

        if (closed)
            path_.close_polygon();

        if (filled)
            fill_path(path_.vertices(), gradient);
        else
            stroke_path(path_.vertices(), state_.line_cap, gradient);
    }

    // Painter::DrawBezier, Painter.cpp:786-832
    void draw_bezier(std::array<BPoint, 4> p, bool filled, const Gradient* gradient)
    {
        if (!valid_clipping_)
            return;

        path_.remove_all();

        for (auto& point : p)
            align(&point);

        path_.move_to(p[0].x, p[0].y);
        path_.curve4(p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);

        if (filled) {
            path_.close_polygon();
            fill_path(agg::conv_curve(path_.vertices()), gradient);
            return;
        }
        stroke_path(agg::conv_curve(path_.vertices()), state_.line_cap, gradient);
    }

    // Painter::DrawShape, Painter.cpp:836-868
    void draw_shape(const ShapeData& shape, bool filled, const Gradient* gradient)
    {
        if (!valid_clipping_)
            return;

        iterate_shape_data(shape);

        if (filled)
            fill_path(agg::conv_curve(path_.vertices()), gradient);
        else
            stroke_path(agg::conv_curve(path_.vertices()), state_.line_cap, gradient);
    }

    // Painter::StrokeRect(const BRect&), Painter.cpp:872-954
    void stroke_rect(BRect r, const Gradient* gradient)
    {
        if (!valid_clipping_)
            return;

        BPoint a {r.left, r.top};
        BPoint b {r.right, r.bottom};
        align(&a, false);
        align(&b, false);

        // first, try an optimized version
        if (gradient == nullptr && pen_size_ == 1.0f && identity_
            && (mode_ == DrawingMode::copy || mode_ == DrawingMode::over)) {
            if (pattern_.solid_high()) {
                stroke_rect(BRect {a.x, a.y, b.x, b.y}, pattern_.high);
                return;
            }
            if (pattern_.solid_low()) {
                stroke_rect(BRect {a.x, a.y, b.x, b.y}, pattern_.low);
                return;
            }
        }

        if (identity_ && std::fmod(pen_size_, 2.0f) != 0.0f) {
            // shift coords to center of pixels
            a.x += 0.5;
            a.y += 0.5;
            b.x += 0.5;
            b.y += 0.5;
        }

        path_.remove_all();
        path_.move_to(a.x, a.y);
        if (a.x == b.x || a.y == b.y) {
            // special case rects with one pixel height or width
            path_.line_to(b.x, b.y);
        } else {
            path_.line_to(b.x, a.y);
            path_.line_to(b.x, b.y);
            path_.line_to(a.x, b.y);
        }
        path_.close_polygon();

        stroke_path(path_.vertices(), state_.line_cap, gradient);
    }

    // Painter::StrokeRect(const BRect&, const rgb_color&), Painter.cpp:958-965
    void stroke_rect(BRect r, Color c) const
    {
        straight_line({r.left, r.top}, {r.right - 1, r.top}, c);
        straight_line({r.right, r.top}, {r.right, r.bottom - 1}, c);
        straight_line({r.right, r.bottom}, {r.left + 1, r.bottom}, c);
        straight_line({r.left, r.bottom}, {r.left, r.top + 1}, c);
    }

    // Painter::FillRect(const BRect&) and its gradient overload,
    // Painter.cpp:969-1071
    void fill_rect(BRect r, const Gradient* gradient)
    {
        if (!valid_clipping_)
            return;

        // support invalid rects
        BPoint a {std::min(r.left, r.right), std::min(r.top, r.bottom)};
        BPoint b {std::max(r.left, r.right), std::max(r.top, r.bottom)};
        align(&a, true, false);
        align(&b, true, false);

        if (gradient == nullptr) {
            // first, try an optimized version
            if ((mode_ == DrawingMode::copy || mode_ == DrawingMode::over) && identity_) {
                if (pattern_.solid_high()) {
                    fill_rect(BRect {a.x, a.y, b.x, b.y}, pattern_.high);
                    return;
                }
                if (pattern_.solid_low()) {
                    fill_rect(BRect {a.x, a.y, b.x, b.y}, pattern_.low);
                    return;
                }
            }
            if (mode_ == DrawingMode::alpha && state_.alpha_function == 0 && identity_) {
                if (pattern_.solid_high()) {
                    blend_rect32(BRect {a.x, a.y, b.x, b.y}, pattern_.high);
                    return;
                }
                if (pattern_.solid_low()) {
                    Color c = pattern_.low;
                    if (state_.constant_alpha)
                        c.a = pattern_.high.a;
                    blend_rect32(BRect {a.x, a.y, b.x, b.y}, c);
                    return;
                }
            }
        } else if (gradient->kind == Gradient::Kind::linear
                   && (mode_ == DrawingMode::copy || mode_ == DrawingMode::over)
                   && identity_ && gradient->start.x == gradient->end.x
                   // TODO: Remove this second check once the optimized method
                   // handled "upside down" gradients as well...
                   && gradient->start.y <= gradient->end.y) {
            // a vertical gradient
            fill_rect_vertical_gradient(BRect {a.x, a.y, b.x, b.y}, *gradient);
            return;
        }

        // account for stricter interpretation of coordinates in AGG
        // the rectangle ranges from the top-left (.0, .0)
        // to the bottom-right (.9999, .9999) corner of pixels
        b.x += 1.0;
        b.y += 1.0;

        path_.remove_all();
        path_.move_to(a.x, a.y);
        path_.line_to(b.x, a.y);
        path_.line_to(b.x, b.y);
        path_.line_to(a.x, b.y);
        path_.close_polygon();

        fill_path(path_.vertices(), gradient);
    }

    // Painter::FillRect(const BRect&, const rgb_color&), Painter.cpp:1075-1111:
    // gfxset32 over each clipping rect.
    void fill_rect(BRect r, Color c) const
    {
        if (!valid_clipping_)
            return;
        const int left = to_int32(r.left);
        const int top = to_int32(r.top);
        const int right = to_int32(r.right);
        const int bottom = to_int32(r.bottom);
        for (const auto& box : region_.rects) {
            const int x1 = std::max(box.left, left);
            const int x2 = std::min(box.right, right);
            if (x1 <= x2) {
                const int y2 = std::min(box.bottom, bottom);
                for (int y1 = std::max(box.top, top); y1 <= y2; y1++) {
                    for (int x = x1; x <= x2; x++)
                        assign(pixel(x, y1), c.r, c.g, c.b);
                }
            }
        }
    }

    // Painter::FillRectVerticalGradient, Painter.cpp:1115-1164: a raw fill,
    // one colour per row from _MakeGradient's colorCount table, which starts
    // at the gradient's (int32) start row and ends at its end row.
    void fill_rect_vertical_gradient(BRect r, const Gradient& gradient) const
    {
        if (!valid_clipping_)
            return;

        // Make sure the color array is no larger than the screen height.
        // (BRect::operator&, Rect.cpp, against the region's Frame().)
        const IntBox frame = region_.frame();
        r = {std::max(r.left, static_cast<float>(frame.left)),
             std::max(r.top, static_cast<float>(frame.top)),
             std::min(r.right, static_cast<float>(frame.right)),
             std::min(r.bottom, static_cast<float>(frame.bottom))};

        // BRect::IntegerHeight, Rect.h:240-243
        const int gradient_array_size = to_int32(std::ceil(r.bottom - r.top)) + 1;
        const int gradient_top = to_int32(gradient.start.y);
        const int gradient_bottom = to_int32(gradient.end.y);
        const int color_count = gradient_bottom - gradient_top + 1;
        if (color_count < 0) {
            // Gradient is upside down. That's currently not supported by
            // this method.
            return;
        }
        // The server sizes a stack array by gradient_array_size unchecked; an
        // empty intersection makes that zero or negative, and then no row
        // below is in range either.
        if (gradient_array_size <= 0)
            return;

        const std::vector<Color> gradient_array = make_vertical_gradient(
            gradient, color_count, gradient_top - to_int32(r.top), gradient_array_size);

        const int left = to_int32(r.left);
        const int top = to_int32(r.top);
        const int right = to_int32(r.right);
        const int bottom = to_int32(r.bottom);
        // fill rects, iterate over clipping boxes
        for (const auto& box : region_.rects) {
            const int x1 = std::max(box.left, left);
            const int x2 = std::min(box.right, right);
            if (x1 <= x2) {
                const int y2 = std::min(box.bottom, bottom);
                for (int y1 = std::max(box.top, top); y1 <= y2; y1++) {
                    const int row = y1 - top;
                    if (row < 0 || row >= gradient_array_size)
                        continue; // past the server's array
                    const Color c = gradient_array[static_cast<std::size_t>(row)];
                    for (int x = x1; x <= x2; x++)
                        assign(pixel(x, y1), c.r, c.g, c.b);
                }
            }
        }
    }

    // Painter::StrokeRoundRect, Painter.cpp:1196-1233
    void stroke_round_rect(BRect r, float x_radius, float y_radius,
                           const Gradient* gradient)
    {
        if (!valid_clipping_)
            return;

        BPoint lt {r.left, r.top};
        BPoint rb {r.right, r.bottom};
        const bool center_offset = std::fmod(pen_size_, 2.0f) != 0.0f;
        align(&lt, center_offset);
        align(&rb, center_offset);

        stroke_path(agg::rounded_rect(lt.x, lt.y, rb.x, rb.y, x_radius, y_radius),
                    state_.line_cap, gradient);
    }

    // Painter::FillRoundRect, Painter.cpp:1237-1284
    void fill_round_rect(BRect r, float x_radius, float y_radius,
                         const Gradient* gradient)
    {
        if (!valid_clipping_)
            return;

        BPoint lt {r.left, r.top};
        BPoint rb {r.right, r.bottom};
        align(&lt, false);
        align(&rb, false);

        // account for stricter interpretation of coordinates in AGG
        // the rectangle ranges from the top-left (.0, .0)
        // to the bottom-right (.9999, .9999) corner of pixels
        rb.x += 1.0;
        rb.y += 1.0;

        fill_path(agg::rounded_rect(lt.x, lt.y, rb.x, rb.y, x_radius, y_radius),
                  gradient);
    }

    // Painter::AlignEllipseRect, Painter.cpp:1288-1302: a B_SUBPIXEL_PRECISE
    // view's rect is left exactly as it is.
    void align_ellipse_rect(BRect* rect, bool filled) const
    {
        if (subpixel_precise_)
            return;
        // align rect to pixels: align_rect_to_pixels, drawing_support.cpp:5-12
        rect->OffsetTo(std::round(rect->left), std::round(rect->top));
        rect->right = std::round(rect->right);
        rect->bottom = std::round(rect->bottom);
        // account for "pixel index" versus "pixel area"
        rect->right++;
        rect->bottom++;
        if (!filled && std::fmod(pen_size_, 2.0f) != 0.0f) {
            // align the stroke
            rect->InsetBy(0.5, 0.5);
        }
    }

    // Painter::DrawEllipse, Painter.cpp:1306-1356
    void draw_ellipse(BRect r, bool fill, const Gradient* gradient)
    {
        if (!valid_clipping_)
            return;

        align_ellipse_rect(&r, fill);

        const float x_radius = static_cast<float>(r.Width() / 2.0);
        const float y_radius = static_cast<float>(r.Height() / 2.0);
        const BPoint center {r.left + x_radius, r.top + y_radius};

        auto divisions = static_cast<std::int32_t>(agg::saturating_int(
            static_cast<double>(x_radius + y_radius + 2 * pen_size_)
            * std::numbers::pi / 2));
        if (divisions < 12)
            divisions = 12;
        if (divisions > 4096)
            divisions = 4096;

        const VertexList path = agg::ellipse(center.x, center.y, x_radius, y_radius,
                                             static_cast<unsigned>(divisions));

        if (fill)
            fill_path(path, gradient);
        else
            stroke_path(path, state_.line_cap, gradient);
    }

    // Painter::StrokeArc, Painter.cpp:1360-1398
    void stroke_arc(BPoint center, float x_radius, float y_radius, float angle,
                    float span, const Gradient* gradient)
    {
        if (!valid_clipping_)
            return;

        align(&center);

        const double angle_rad = (angle * std::numbers::pi) / 180.0;
        const double span_rad = (span * std::numbers::pi) / 180.0;
        const VertexList arc = agg::bezier_arc(center.x, center.y, x_radius,
                                               y_radius, -angle_rad, -span_rad);

        stroke_path(agg::conv_curve(arc, 2.0), state_.line_cap, gradient);
    }

    // Painter::FillArc, Painter.cpp:1402-1472
    void fill_arc(BPoint center, float x_radius, float y_radius, float angle,
                  float span, const Gradient* gradient)
    {
        if (!valid_clipping_)
            return;

        align(&center);

        const double angle_rad = (angle * std::numbers::pi) / 180.0;
        const double span_rad = (span * std::numbers::pi) / 180.0;
        const VertexList segmented_arc = agg::conv_curve(
            agg::bezier_arc(center.x, center.y, x_radius, y_radius, -angle_rad,
                            -span_rad));

        path_.remove_all();

        // build a new path by starting at the center point,
        // then traversing the arc, then going back to the center
        path_.move_to(center.x, center.y);
        for (const auto& v : segmented_arc)
            path_.line_to(v.x, v.y);
        path_.close_polygon();

        fill_path(path_.vertices(), gradient);
    }

    // Painter::_DrawTriangle, Painter.cpp:1737-1782
    void draw_triangle(BPoint pt1, BPoint pt2, BPoint pt3, bool fill,
                       const Gradient* gradient)
    {
        if (!valid_clipping_)
            return;

        align(&pt1);
        align(&pt2);
        align(&pt3);

        path_.remove_all();
        path_.move_to(pt1.x, pt1.y);
        path_.line_to(pt2.x, pt2.y);
        path_.line_to(pt3.x, pt3.y);
        path_.close_polygon();

        if (fill)
            fill_path(path_.vertices(), gradient);
        else
            stroke_path(path_.vertices(), state_.line_cap, gradient);
    }

private:
    // _Align, Painter.cpp:1647-1685. Rounding truncates toward zero; the
    // two-argument form rounds only when the view is not B_SUBPIXEL_PRECISE
    // (:1665-1668), while the explicit form (FillRect's) always does.
    static float align(float coord, bool round, bool center_offset)
    {
        if (round)
            coord = static_cast<float>(to_int32(coord));
        // This code is supposed to move coordinates to the center of pixels,
        // as AGG considers (0,0) to be the "upper left corner" of a pixel,
        // but BViews are less strict on those details
        if (center_offset)
            coord = static_cast<float>(coord + 0.5);
        return coord;
    }
    static void align(BPoint* point, bool round, bool center_offset)
    {
        point->x = align(point->x, round, center_offset);
        point->y = align(point->y, round, center_offset);
    }
    void align(BPoint* point, bool center_offset = true) const
    {
        align(point, !subpixel_precise_, center_offset);
    }

    // _IterateShapeData, Painter.cpp:1785-1849. The server walks the point
    // list unchecked; a message off the wire is not trusted to be consistent,
    // so this stops at the end of the points instead.
    void iterate_shape_data(const ShapeData& shape)
    {
        path_.remove_all();
        std::size_t next = 0;
        const auto& points = shape.points;
        const float scale = shape.scale;
        const auto have = [&](std::size_t n) { return next + n <= points.size(); };
        const auto mx = [&](std::size_t i) { return points[i].x * scale + shape.offset.x; };
        const auto my = [&](std::size_t i) { return points[i].y * scale + shape.offset.y; };
        for (const std::uint32_t word : shape.ops) {
            const std::uint32_t op = word & 0xFF000000;
            if ((op & 0x80000000) != 0) { // OP_MOVETO
                if (!have(1))
                    return;
                path_.move_to(mx(next), my(next));
                next++;
            }

            if ((op & 0x10000000) != 0) { // OP_LINETO
                std::int32_t count = static_cast<std::int32_t>(word & 0x00FFFFFF);
                while (count--) {
                    if (!have(1))
                        return;
                    path_.line_to(mx(next), my(next));
                    next++;
                }
            }

            if ((op & 0x20000000) != 0) { // OP_BEZIERTO
                std::int32_t count = static_cast<std::int32_t>(word & 0x00FFFFFF);
                // The server loops `while (count)`, counting down by three --
                // forever, for a count that is not a multiple of three.
                while (count >= 3) {
                    if (!have(3))
                        return;
                    path_.curve4(mx(next), my(next), mx(next + 1), my(next + 1),
                                 mx(next + 2), my(next + 2));
                    next += 3;
                    count -= 3;
                }
            }

            constexpr std::uint32_t large_cw = 0x01000000;
            constexpr std::uint32_t large_ccw = 0x02000000;
            constexpr std::uint32_t small_cw = 0x04000000;
            constexpr std::uint32_t small_ccw = 0x08000000;
            if ((op & (large_cw | large_ccw | small_cw | small_ccw)) != 0) {
                std::int32_t count = static_cast<std::int32_t>(word & 0x00FFFFFF);
                while (count > 0) {
                    if (!have(3))
                        return;
                    // The radii are scaled but not offset, the angle is
                    // neither, and both flags are bit tests (:1833-1840).
                    path_.arc_to(points[next].x * scale, points[next].y * scale,
                                 points[next + 1].x,
                                 (op & (large_cw | large_ccw)) != 0,
                                 (op & (small_cw | large_cw)) != 0,
                                 mx(next + 2), my(next + 2));
                    next += 3;
                    count -= 3;
                }
            }

            if ((op & 0x40000000) != 0) // OP_CLOSE
                path_.close_polygon();
        }
    }

    // _BlendRect32, Painter.cpp:1871-1902
    void blend_rect32(BRect r, Color c) const
    {
        if (!valid_clipping_)
            return;
        const int left = to_int32(r.left);
        const int top = to_int32(r.top);
        const int right = to_int32(r.right);
        const int bottom = to_int32(r.bottom);
        for (const auto& box : region_.rects) {
            const int x1 = std::max(box.left, left);
            const int x2 = std::min(box.right, right);
            if (x1 <= x2) {
                const int y2 = std::min(box.bottom, bottom);
                for (int y1 = std::max(box.top, top); y1 <= y2; y1++)
                    blend_line32(pixel(x1, y1), x2 - x1 + 1, c.r, c.g, c.b, c.a);
            }
        }
    }

    // _StrokePath, Painter.cpp:1957-2014
    void stroke_path(const VertexList& path, std::uint32_t cap_mode,
                     const Gradient* gradient)
    {
        agg::StrokeStyle style;
        style.width = pen_size_;
        style.cap = agg_line_cap_mode_for(cap_mode);
        style.join = agg_line_join_mode_for(state_.line_join);
        style.miter_limit = state_.miter_limit;
        if (identity_) {
            rasterize_path(agg::conv_stroke(path, style), gradient);
            return;
        }
        style.approximation_scale = transform_.scale();
        rasterize_path(agg::conv_transform(agg::conv_stroke(path, style), transform_),
                       gradient);
    }

    // _FillPath, Painter.cpp:2017-2027 and :2056-2066
    void fill_path(const VertexList& path, const Gradient* gradient)
    {
        if (identity_)
            rasterize_path(path, gradient);
        else
            rasterize_path(agg::conv_transform(path, transform_), gradient);
    }

    // _RasterizePath, Painter.cpp:2030-2053 (solid) and :2284-2322 (gradient)
    void rasterize_path(const VertexList& path, const Gradient* gradient)
    {
        const IntBox cb = region_.frame();
        if (gradient != nullptr) {
            // The gradient function, its transform and its extent per type,
            // Painter.cpp:2072-2142. The extent, gradientStop, is an int
            // (Painter.h:343): radial gradients pass their radius truncated,
            // the rest leave it at 100 -- so diamond and conic always span
            // 100 pixels, and radial focus never sees its focal point.
            agg::TransAffine gradient_transform;
            agg::GradientShape shape = agg::GradientShape::x;
            int gradient_stop = 100;
            switch (gradient->kind) {
            case Gradient::Kind::linear:
                shape = agg::GradientShape::x;
                calc_linear_gradient_transform(to_bpoint(gradient->start),
                                               to_bpoint(gradient->end),
                                               gradient_transform);
                break;
            case Gradient::Kind::radial:
                shape = agg::GradientShape::radial;
                calc_radial_gradient_transform(to_bpoint(gradient->center),
                                               gradient_transform);
                gradient_stop = to_int32(gradient->radius);
                break;
            case Gradient::Kind::radial_focus:
                shape = agg::GradientShape::radial_focus;
                calc_radial_gradient_transform(to_bpoint(gradient->center),
                                               gradient_transform);
                gradient_stop = to_int32(gradient->radius);
                break;
            case Gradient::Kind::diamond:
                shape = agg::GradientShape::diamond;
                calc_radial_gradient_transform(to_bpoint(gradient->center),
                                               gradient_transform);
                break;
            case Gradient::Kind::conic:
                shape = agg::GradientShape::conic;
                calc_radial_gradient_transform(to_bpoint(gradient->center),
                                               gradient_transform);
                break;
            default:
                // TYPE_NONE/unknown: nothing is rasterised at all.
                return;
            }

            // SolidPatternGuard, Painter.cpp:181-199 and :2300
            const auto saved = pattern_.pattern;
            pattern_.pattern.fill(0xff);
            agg::Rasterizer rasterizer(false);
            rasterizer.clip_box(cb.left, cb.top, cb.right + 1, cb.bottom + 1);
            rasterizer.add_path(path);
            const PixelFormat pixfmt = pixel_format();
            const RendererRegion ren(pixfmt, region_);
            GradientRenderer renderer(ren, *gradient, gradient_transform, shape,
                                      gradient_stop);
            rasterizer.render_unpacked(renderer);
            pattern_.pattern = saved;
            return;
        }
        // ConstrainClipping gives both rasterisers the region's frame,
        // right/bottom exclusive (Painter.cpp:363-367). gSubpixelAntialiasing
        // is on (see the top of this file), so a solid fill or stroke always
        // takes the subpixel rasteriser, scanline and renderer
        // (Painter.cpp:2041-2045); the grey-scale fRasterizer/fPackedScanline/
        // fRenderer path below it is never reached and is not reproduced.
        agg::Rasterizer rasterizer(true);
        rasterizer.clip_box(cb.left, cb.top, cb.right + 1, cb.bottom + 1);
        rasterizer.add_path(path);
        const PixelFormat pixfmt = pixel_format();
        const RendererRegion ren(pixfmt, region_);
        SubpixRenderer renderer(ren, renderer_color());
        rasterizer.render_packed(renderer);
    }

    // _CalcLinearGradientTransform, Painter.cpp:2147-2160, gradient_d2 100.
    // dx and dy are floats, and so are the sqrt and atan2 of them: in C++
    // <math.h> brings in the float overloads.
    void calc_linear_gradient_transform(BPoint start, BPoint end,
                                        agg::TransAffine& matrix) const
    {
        constexpr float gradient_d2 = 100.0f;
        const float dx = end.x - start.x;
        const float dy = end.y - start.y;

        matrix = {};
        matrix.multiply(agg::TransAffine::scaling(std::sqrt(dx * dx + dy * dy) / gradient_d2));
        matrix.multiply(agg::TransAffine::rotation(std::atan2(dy, dx)));
        matrix.multiply(agg::TransAffine::translation(start.x, start.y));
        matrix.multiply(transform_);
        matrix.invert();
    }

    // _CalcRadialGradientTransform, Painter.cpp:2163-2171
    void calc_radial_gradient_transform(BPoint center, agg::TransAffine& matrix) const
    {
        matrix = {};
        matrix.multiply(agg::TransAffine::translation(center.x, center.y));
        matrix.multiply(transform_);
        matrix.invert();
    }

    // Transformable::TransformBounds, Transformable.cpp:244-263
    [[nodiscard]] BRect transform_bounds(BRect bounds) const
    {
        if (!bounds.IsValid())
            return bounds;
        std::array<BPoint, 4> corners {{{bounds.left, bounds.top},
                                        {bounds.right, bounds.top},
                                        {bounds.left, bounds.bottom},
                                        {bounds.right, bounds.bottom}}};
        for (auto& p : corners) {
            double x = p.x;
            double y = p.y;
            transform_.transform(&x, &y);
            p.x = static_cast<float>(x);
            p.y = static_cast<float>(y);
        }
        const auto min4 = [](float a, float b, float c, float d) {
            return std::min(a, std::min(b, std::min(c, d)));
        };
        const auto max4 = [](float a, float b, float c, float d) {
            return std::max(a, std::max(b, std::max(c, d)));
        };
        return {std::floor(min4(corners[0].x, corners[1].x, corners[2].x, corners[3].x)),
                std::floor(min4(corners[0].y, corners[1].y, corners[2].y, corners[3].y)),
                std::ceil(max4(corners[0].x, corners[1].x, corners[2].x, corners[3].x)),
                std::ceil(max4(corners[0].y, corners[1].y, corners[2].y, corners[3].y))};
    }

    // The colour _SetRendererColor last gave the renderers: the high colour
    // while the pattern is B_SOLID_HIGH, the low colour while it is
    // B_SOLID_LOW (SetHighColor/SetLowColor/SetPattern, Painter.cpp:
    // 387-478). Only the *Solid drawing modes read it, and those are only
    // installed for a solid pattern.
    [[nodiscard]] Color renderer_color() const
    {
        return pattern_.solid_low() ? pattern_.low : pattern_.high;
    }

    [[nodiscard]] PixelFormat pixel_format() const
    {
        return PixelFormat(canvas_, pattern_, mode_, state_.constant_alpha,
                           state_.alpha_function);
    }

    [[nodiscard]] u8* pixel(int x, int y) const
    {
        return canvas_.bgra
            + (static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas_.width)
               + static_cast<std::size_t>(x)) * 4;
    }

    Canvas canvas_;
    const DrawState& state_;
    ClipRegion region_;
    bool valid_clipping_ = false;
    bool identity_ = true;
    agg::TransAffine transform_;
    float pen_size_ = 1;
    bool subpixel_precise_ = false;
    DrawingMode mode_ = DrawingMode::copy;
    PatternHandler pattern_;
    agg::PathStorage path_;
};

// ---- BitmapPainter, Painter/bitmap_painter/ -------------------------------
//
// Painter::BitmapPainter (BitmapPainter.cpp) and the drawing routines it
// dispatches to (DrawBitmapNoScale.h, DrawBitmapNearestNeighbor.h,
// DrawBitmapBilinear.h, DrawBitmapGeneric.h), each with its own integer and
// float arithmetic, because each rounds a fractional rect its own way -- and
// B_SUBPIXEL_PRECISE is what lets a fractional rect reach them at all:
// _DetermineTransform rounds both rects to whole pixels unless the view is
// precise (BitmapPainter.cpp:191-194).
//
// The bitmap arrives as B_RGBA32 already: decode_bitmap (session.cpp) converts
// every colour space as _ConvertColorSpace does for the drawing mode
// (BitmapPainter.cpp:258-329), B_TRANSPARENT_MAGIC_RGBA32 to alpha 0 and a
// B_RGB32 fourth byte kept as alpha under B_OP_ALPHA included. So the B_CMAP8
// and B_RGB32 shortcuts at the top of Draw (:79-102) are not taken; for the
// pixels they
// see they write what the B_RGBA32 paths below do (CMap8Copy copies the
// colour like Bgr32Copy; CMap8Over and Bgr32Over write every pixel that is
// not transparent, which is Bgr32Alpha for alpha 0 or 255).
//
// Two deliberate departures, both only for rects no BView produces: where the
// server's pointer arithmetic would run off the end of the bitmap a byte reads
// as 0 here, and a pixel whose weight index would fall outside the server's
// stack array of weights is left untouched.

constexpr std::uint32_t b_tile_bitmap = 0x00000003;            // InterfaceDefs.h:310
constexpr std::uint32_t b_filter_bitmap_bilinear = 0x00000100; // :321

// (uint16)value on the server's arm64: FCVTZU converts to 32 bits, saturating
// (NaN and negatives give 0), and the narrowing keeps the low 16.
std::uint16_t to_uint16(double value)
{
    if (!(value > 0))
        return 0;
    if (value >= 4294967295.0)
        return 0xffff;
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(value));
}

// align_rect_to_pixels, drawing_support.cpp:6-12. OffsetTo keeps the width,
// so the right and bottom edges it then rounds are the *moved* ones.
void align_rect_to_pixels(BRect* rect)
{
    rect->OffsetTo(std::round(rect->left), std::round(rect->top));
    rect->right = std::round(rect->right);
    rect->bottom = std::round(rect->bottom);
}

// BRect::Intersects, Rect.cpp:251-258
bool intersects(const BRect& a, const BRect& b)
{
    if (!a.IsValid() || !b.IsValid())
        return false;
    return !(b.left > a.right || b.right < a.left || b.top > a.bottom
             || b.bottom < a.top);
}

// BRect::IntegerWidth/IntegerHeight, Rect.h:226-236: (int32)ceil(right - left).
std::int64_t integer_extent(float from, float to)
{
    return to_int32(std::ceil(to - from));
}

// DrawBitmapBilinear.h:30-33
struct FilterInfo {
    std::uint16_t index = 0;
    std::uint16_t weight = 0;
};

class BitmapPainter {
public:
    // BitmapPainter::BitmapPainter, BitmapPainter.cpp:33-56
    BitmapPainter(Painter& painter, BitmapData bitmap, std::uint32_t options)
        : painter_(painter)
        , bitmap_(bitmap)
        , stride_(static_cast<std::size_t>(std::max(bitmap.width, 0)) * 4)
        , size_(stride_ * static_cast<std::size_t>(std::max(bitmap.height, 0)))
        , options_(options)
        , valid_(bitmap.bgra != nullptr && bitmap.width > 0 && bitmap.height > 0)
        , bitmap_bounds_ {0, 0, static_cast<float>(bitmap.width - 1),
                          static_cast<float>(bitmap.height - 1)}
    {
    }

    // BitmapPainter::Draw, BitmapPainter.cpp:59-171
    void draw(const BRect& source_rect, const BRect& destination_rect)
    {
        if (!valid_ || !determine_transform(source_rect, destination_rect))
            return;

        const DrawingMode mode = painter_.mode_;
        const bool pixel_alpha_overlay = mode == DrawingMode::alpha
            && !painter_.state_.constant_alpha
            && painter_.state_.alpha_function == 0 /* B_ALPHA_OVERLAY */;
        const bool affine = !painter_.identity_;
        const bool bilinear = (options_ & b_filter_bitmap_bilinear) != 0;
        // _HasAlphaMask (:244-248) is always false: the protocol has no
        // ClipToPicture, so fMaskedUnpackedScanline stays NULL.
        if ((options_ & b_tile_bitmap) == 0) {
            if (!has_scale() && !affine) {
                if (mode == DrawingMode::copy) {
                    draw_no_scale(false); // DrawBitmapNoScale<Bgr32Copy>
                    return;
                }
                if (mode == DrawingMode::over || pixel_alpha_overlay) {
                    draw_no_scale(true); // DrawBitmapNoScale<Bgr32Alpha>
                    return;
                }
            }
            if (mode == DrawingMode::copy && !affine) {
                if (bilinear)
                    draw_bilinear(false); // <ColorTypeRgb, DrawModeCopy>
                else
                    draw_nearest_neighbor_copy();
                return;
            }
            if (pixel_alpha_overlay && !affine && bilinear) {
                draw_bilinear(true); // <ColorTypeRgba, DrawModeAlphaOverlay>
                return;
            }
        }
        draw_generic((options_ & b_tile_bitmap) != 0);
    }

private:
    // _DetermineTransform, BitmapPainter.cpp:174-232. The scale is a float
    // quotient stored in a double, and each constrained edge is moved in
    // double and stored back into the rect's float.
    bool determine_transform(BRect source, const BRect& destination)
    {
        const bool tile = (options_ & b_tile_bitmap) != 0;
        if (!painter_.valid_clipping_ || !source.IsValid()
            || (!tile && !intersects(source, bitmap_bounds_))
            || !destination.IsValid()) {
            return false;
        }

        destination_ = destination;
        if (!painter_.subpixel_precise_) {
            align_rect_to_pixels(&source);
            align_rect_to_pixels(&destination_);
        }

        if (!tile) {
            scale_x_ = (destination_.Width() + 1) / (source.Width() + 1);
            scale_y_ = (destination_.Height() + 1) / (source.Height() + 1);
            if (scale_x_ == 0.0 || scale_y_ == 0.0)
                return false;
            if (source.left < bitmap_bounds_.left) {
                const float diff = bitmap_bounds_.left - source.left;
                destination_.left = static_cast<float>(destination_.left + diff * scale_x_);
                source.left = bitmap_bounds_.left;
            }
            if (source.top < bitmap_bounds_.top) {
                const float diff = bitmap_bounds_.top - source.top;
                destination_.top = static_cast<float>(destination_.top + diff * scale_y_);
                source.top = bitmap_bounds_.top;
            }
            if (source.right > bitmap_bounds_.right) {
                const float diff = source.right - bitmap_bounds_.right;
                destination_.right = static_cast<float>(destination_.right - diff * scale_x_);
                source.right = bitmap_bounds_.right;
            }
            if (source.bottom > bitmap_bounds_.bottom) {
                const float diff = source.bottom - bitmap_bounds_.bottom;
                destination_.bottom
                    = static_cast<float>(destination_.bottom - diff * scale_y_);
                source.bottom = bitmap_bounds_.bottom;
            }
        } else {
            scale_x_ = 1.0;
            scale_y_ = 1.0;
        }

        offset_.x = destination_.left - source.left;
        offset_.y = destination_.top - source.top;
        return true;
    }

    // _HasScale, :232-236
    [[nodiscard]] bool has_scale() const { return scale_x_ != 1.0 || scale_y_ != 1.0; }

    // A byte of the bitmap at a flat offset, as the server's pointer
    // arithmetic reaches it: rows are contiguous, so one pixel past a row's
    // end is the next row's first.
    [[nodiscard]] u8 byte(std::size_t offset) const
    {
        return offset < size_ ? bitmap_.bgra[offset] : u8 {0};
    }

    static void store(u8* d, int b, int g, int r)
    {
        d[0] = static_cast<u8>(b);
        d[1] = static_cast<u8>(g);
        d[2] = static_cast<u8>(r);
        d[3] = 255;
    }

    // DrawBitmapNoScale::Draw, DrawBitmapNoScale.h:20-80, with Bgr32Copy's
    // memcpy (:128-134) or Bgr32Alpha's blend (:152-174) for each row.
    void draw_no_scale(bool alpha)
    {
        // The offset is an IntPoint, which truncates (IntPoint.h:70-75).
        const std::int64_t offset_x = to_int32(offset_.x);
        const std::int64_t offset_y = to_int32(offset_.y);
        const std::int32_t left = to_int32(destination_.left);
        const std::int32_t top = to_int32(destination_.top);
        const std::int32_t right = to_int32(destination_.right);
        const std::int32_t bottom = to_int32(destination_.bottom);
        for (const auto& box : painter_.region_.rects) {
            const std::int32_t l = std::max(box.left, left);
            const std::int32_t r = std::min(box.right, right);
            if (l > r)
                continue;
            const std::int32_t t = std::max(box.top, top);
            const std::int32_t b = std::min(box.bottom, bottom);
            for (std::int32_t y = t; y <= b; ++y) {
                const std::int64_t sy = y - offset_y;
                for (std::int32_t x = l; x <= r; ++x) {
                    const std::int64_t sx = x - offset_x;
                    if (sx < 0 || sx >= bitmap_.width || sy < 0 || sy >= bitmap_.height)
                        continue;
                    const u8* s = bitmap_.bgra + static_cast<std::size_t>(sy) * stride_
                        + static_cast<std::size_t>(sx) * 4;
                    u8* d = painter_.pixel(x, y);
                    if (!alpha || s[3] == 255) {
                        store(d, s[0], s[1], s[2]);
                    } else {
                        store(d, ((s[0] - d[0]) * s[3] + (d[0] << 8)) >> 8,
                              ((s[1] - d[1]) * s[3] + (d[1] << 8)) >> 8,
                              ((s[2] - d[2]) * s[3] + (d[2] << 8)) >> 8);
                    }
                }
            }
        }
    }

    // What the nearest-neighbour and bilinear copies share: how many weights
    // to compute, and the offset that compensates for computing fewer than
    // the destination rect spans (DrawBitmapNearestNeighbor.h:22-44,
    // DrawBitmapBilinear.h:493-516).
    struct WeightRange {
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t offset_x = 0;
        std::uint32_t offset_y = 0;
    };

    [[nodiscard]] WeightRange weight_range() const
    {
        const IntBox frame = painter_.region_.frame();
        WeightRange range {
            static_cast<std::uint32_t>(integer_extent(destination_.left, destination_.right) + 1),
            static_cast<std::uint32_t>(integer_extent(destination_.top, destination_.bottom) + 1)};
        if (static_cast<std::int64_t>(frame.right - frame.left) + 1
            < static_cast<std::int32_t>(range.width)) {
            range.width = static_cast<std::uint32_t>(frame.right - frame.left + 1);
        }
        if (static_cast<std::int64_t>(frame.bottom - frame.top) + 1
            < static_cast<std::int32_t>(range.height)) {
            range.height = static_cast<std::uint32_t>(frame.bottom - frame.top + 1);
        }
        if (static_cast<float>(frame.left) > destination_.left) {
            range.offset_x = static_cast<std::uint32_t>(
                to_int32(static_cast<float>(frame.left) - destination_.left));
        }
        if (static_cast<float>(frame.top) > destination_.top) {
            range.offset_y = static_cast<std::uint32_t>(
                to_int32(static_cast<float>(frame.top) - destination_.top));
        }
        return range;
    }

    // The clip-box loop both scaled copies run (DrawBitmapNearestNeighbor.h:
    // 91-130, DrawBitmapBilinear.h:56-86): for each box, the first
    // destination pixel and the weight indices of the span it covers.
    template <typename Body>
    void for_each_clip_box(const WeightRange& range, Body body) const
    {
        const std::int32_t left = to_int32(destination_.left);
        const std::int32_t top = to_int32(destination_.top);
        const std::int32_t right = to_int32(destination_.right);
        const std::int32_t bottom = to_int32(destination_.bottom);
        const auto ox = static_cast<std::int32_t>(range.offset_x);
        const auto oy = static_cast<std::int32_t>(range.offset_y);
        for (const auto& box : painter_.region_.rects) {
            const std::int32_t x1 = std::max(box.left, left);
            const std::int32_t x2 = std::min(box.right, right);
            if (x1 > x2)
                continue;
            const std::int32_t y1 = std::max(box.top, top);
            const std::int32_t y2 = std::min(box.bottom, bottom);
            if (y1 > y2)
                continue;
            body(x1, y1, x1 - left - ox, x2 - left - ox, y1 - (top + oy), y2 - (top + oy));
        }
    }

    // DrawBitmapNearestNeighborCopy::Draw, DrawBitmapNearestNeighbor.h:15-136
    void draw_nearest_neighbor_copy()
    {
        const WeightRange range = weight_range();
        const auto src_width = static_cast<std::uint32_t>(bitmap_.width);
        const auto src_height = static_cast<std::uint32_t>(bitmap_.height);
        std::vector<std::uint16_t> x_indices(range.width);
        std::vector<std::uint16_t> y_indices(range.height);
        const std::int32_t x_shift = to_int32(destination_.left - offset_.x);
        const std::int32_t y_shift = to_int32(destination_.top - offset_.y);
        for (std::uint32_t i = 0; i < range.width; i++) {
            auto index = to_uint16((i + range.offset_x) * src_width / (src_width * scale_x_));
            index = static_cast<std::uint16_t>(index + x_shift);
            x_indices[i] = static_cast<std::uint16_t>(index * 4);
        }
        for (std::uint32_t i = 0; i < range.height; i++) {
            const auto index
                = to_uint16((i + range.offset_y) * src_height / (src_height * scale_y_));
            y_indices[i] = static_cast<std::uint16_t>(index + y_shift);
        }
        for_each_clip_box(range, [&](std::int32_t dx, std::int32_t dy, std::int32_t xl,
                                     std::int32_t xr, std::int32_t y1, std::int32_t y2) {
            for (std::int32_t y = y1; y <= y2; ++y, ++dy) {
                if (y < 0 || static_cast<std::uint32_t>(y) >= range.height)
                    continue;
                const std::size_t row = y_indices[static_cast<std::size_t>(y)] * stride_;
                std::int32_t px = dx;
                for (std::int32_t x = xl; x <= xr; ++x, ++px) {
                    if (x < 0 || static_cast<std::uint32_t>(x) >= range.width)
                        continue;
                    const std::size_t s = row + x_indices[static_cast<std::size_t>(x)];
                    store(painter_.pixel(px, dy), byte(s), byte(s + 1), byte(s + 2));
                }
            }
        });
    }

    // DrawBitmapBilinear::Draw, DrawBitmapBilinear.h:482-648, drawing with
    // BilinearDefault<ColorTypeRgb, DrawModeCopy> or <ColorTypeRgba,
    // DrawModeAlphaOverlay> (:217-327) -- or, for a copy at exactly 1.5, 2,
    // 2.5 or 3 times, BilinearLowFilterRatio (:330-418). gSIMDFlags is 0 off
    // x86 (Painter.cpp:117-120), so BilinearSimd is never chosen.
    void draw_bilinear(bool alpha_overlay)
    {
        const WeightRange range = weight_range();
        const auto src_width = static_cast<std::uint32_t>(bitmap_.width);
        const auto src_height = static_cast<std::uint32_t>(bitmap_.height);
        std::vector<FilterInfo> weights_x(range.width);
        std::vector<FilterInfo> weights_y(range.height);
        const std::int32_t x_shift = to_int32(destination_.left - offset_.x);
        const std::int32_t y_shift = to_int32(destination_.top - offset_.y);
        // `float index = (i + offset) * (srcWidth - 1) / (srcWidth * scale - 1)`:
        // an unsigned product over a double, kept in a float (:536-578).
        const auto weigh = [](std::uint32_t numerator, double denominator) {
            const auto index = static_cast<float>(numerator / denominator);
            FilterInfo info;
            info.index = to_uint16(index);
            info.weight = static_cast<std::uint16_t>(
                255 - to_uint16((index - static_cast<float>(info.index)) * 255));
            return info;
        };
        for (std::uint32_t i = 0; i < range.width; i++) {
            FilterInfo& w = weights_x[i];
            w = weigh((i + range.offset_x) * (src_width - 1), src_width * scale_x_ - 1);
            w.index = static_cast<std::uint16_t>(w.index + x_shift);
            w.index = static_cast<std::uint16_t>(w.index * 4);
        }
        for (std::uint32_t i = 0; i < range.height; i++) {
            FilterInfo& w = weights_y[i];
            w = weigh((i + range.offset_y) * (src_height - 1), src_height * scale_y_ - 1);
            w.index = static_cast<std::uint16_t>(w.index + y_shift);
        }

        const bool low_filter_ratio = !alpha_overlay && scale_x_ == scale_y_
            && (scale_x_ == 1.5 || scale_x_ == 2.0 || scale_x_ == 2.5 || scale_x_ == 3.0);
        const bool single_row = bitmap_.height <= 1; // fSource->height() > 1
        const auto wx = [&](std::int32_t x) -> const FilterInfo* {
            return x >= 0 && static_cast<std::uint32_t>(x) < range.width
                ? &weights_x[static_cast<std::size_t>(x)] : nullptr;
        };
        const auto wy = [&](std::int32_t y) -> const FilterInfo* {
            return y >= 0 && static_cast<std::uint32_t>(y) < range.height
                ? &weights_y[static_cast<std::size_t>(y)] : nullptr;
        };
        // DrawModeCopy::Blend (:192-201) and DrawModeAlphaOverlay::Blend
        // (:204-225); t[] is B, G, R, A, as the bytes lie.
        const auto blend = [&](std::int32_t x, std::int32_t y, const std::uint32_t* t) {
            u8* d = painter_.pixel(x, y);
            const auto t0 = static_cast<u8>(t[0]);
            const auto t1 = static_cast<u8>(t[1]);
            const auto t2 = static_cast<u8>(t[2]);
            const auto t3 = static_cast<u8>(t[3]);
            if (!alpha_overlay || t3 == 255) {
                store(d, t0, t1, t2);
            } else {
                store(d, ((t0 - d[0]) * t3 + (d[0] << 8)) >> 8,
                      ((t1 - d[1]) * t3 + (d[1] << 8)) >> 8,
                      ((t2 - d[2]) * t3 + (d[2] << 8)) >> 8);
            }
        };
        const int channels = alpha_overlay ? 4 : 3;
        // ColorType::Interpolate, ::InterpolateLastColumn and
        // ::InterpolateLastRow (:104-188).
        const auto interpolate = [&](std::uint32_t* t, std::size_t s, unsigned w_left,
                                     unsigned w_top, unsigned w_right, unsigned w_bottom) {
            for (int c = 0; c < channels; ++c) {
                t[c] = (byte(s + c) * w_left + byte(s + 4 + c) * w_right) * w_top;
                t[c] += (byte(s + stride_ + c) * w_left + byte(s + stride_ + 4 + c) * w_right)
                    * w_bottom;
                t[c] >>= 16;
            }
        };
        const auto last_column = [&](std::uint32_t* t, std::size_t s, unsigned w_top,
                                     unsigned w_bottom) {
            for (int c = 0; c < channels; ++c)
                t[c] = (byte(s + c) * w_top + byte(s + stride_ + c) * w_bottom) >> 8;
        };
        const auto last_row = [&](std::uint32_t* t, std::size_t s, unsigned w_left,
                                  unsigned w_right) {
            for (int c = 0; c < channels; ++c)
                t[c] = (byte(s + c) * w_left + byte(s + 4 + c) * w_right) >> 8;
        };

        for_each_clip_box(range, [&](std::int32_t dx, std::int32_t dy, std::int32_t xl,
                                     std::int32_t xr, std::int32_t y1, std::int32_t y2) {
            if (low_filter_ratio) {
                // BilinearLowFilterRatio::DrawToClipRect, :332-417
                for (; y1 <= y2; y1++, dy++) {
                    const FilterInfo* fy = wy(y1);
                    if (fy == nullptr)
                        continue;
                    const unsigned w_top = fy->weight;
                    const unsigned w_bottom = 255 - fy->weight;
                    const std::size_t src = fy->index * stride_;
                    std::int32_t px = dx;
                    for (std::int32_t x = xl; x <= xr; x++, px++) {
                        const FilterInfo* fx = wx(x);
                        if (fx == nullptr)
                            continue;
                        const std::size_t s = src + fx->index;
                        std::uint32_t t[4] {};
                        if (w_top == 255) {
                            if (fx->weight == 255) {
                                t[0] = byte(s);
                                t[1] = byte(s + 1);
                                t[2] = byte(s + 2);
                            } else {
                                last_row(t, s, fx->weight, 255 - fx->weight);
                            }
                        } else if (fx->weight == 255) {
                            last_column(t, s, w_top, w_bottom);
                        } else {
                            interpolate(t, s, fx->weight, w_top, 255 - fx->weight, w_bottom);
                        }
                        blend(px, dy, t);
                    }
                }
                return;
            }
            // BilinearDefault::DrawToClipRect, :220-326: the last row and
            // column are peeled off when they hit a source pixel exactly.
            const FilterInfo* last_y = wy(y2);
            const FilterInfo* last_x = wx(xr);
            std::int32_t y_max = y2;
            if (last_y != nullptr && last_y->weight == 255)
                y_max--;
            std::int32_t x_index_max = xr;
            if (last_x != nullptr && last_x->weight == 255)
                x_index_max--;
            for (; y1 <= y_max; y1++, dy++) {
                const FilterInfo* fy = wy(y1);
                if (fy == nullptr)
                    continue;
                const unsigned w_top = fy->weight;
                const unsigned w_bottom = 255 - fy->weight;
                const std::size_t src = fy->index * stride_;
                std::int32_t px = dx;
                for (std::int32_t x = xl; x <= x_index_max; x++, px++) {
                    const FilterInfo* fx = wx(x);
                    if (fx == nullptr)
                        continue;
                    const std::size_t s = src + fx->index;
                    std::uint32_t t[4] {};
                    if (!single_row)
                        interpolate(t, s, fx->weight, w_top, 255 - fx->weight, w_bottom);
                    else
                        last_row(t, s, fx->weight, 255 - fx->weight);
                    blend(px, dy, t);
                }
                if (x_index_max < xr && !single_row && last_x != nullptr) {
                    std::uint32_t t[4] {};
                    last_column(t, src + last_x->index, w_top, w_bottom);
                    blend(px, dy, t);
                }
            }
            if (y_max < y2 && last_y != nullptr) {
                const std::size_t src = last_y->index * stride_;
                std::int32_t px = dx;
                for (std::int32_t x = xl; x <= x_index_max; x++, px++) {
                    const FilterInfo* fx = wx(x);
                    if (fx == nullptr)
                        continue;
                    std::uint32_t t[4] {};
                    last_row(t, src + fx->index, fx->weight, 255 - fx->weight);
                    blend(px, dy, t);
                }
                if (x_index_max < xr && last_x != nullptr) {
                    const std::size_t s = src + last_x->index;
                    const std::uint32_t t[4] {byte(s), byte(s + 1), byte(s + 2), byte(s + 3)};
                    blend(px, dy, t);
                }
            }
        });
    }

    // The source pixel at (x, y) through image_accessor_clone, which clamps
    // to the bitmap's edges, or image_accessor_wrap with wrap_mode_repeat,
    // whose `(unsigned(v) + add) % size` keeps its own unsigned arithmetic
    // (agg_image_accessors.h:152-303).
    [[nodiscard]] const u8* source_pixel(int x, int y, bool tile) const
    {
        if (tile) {
            const auto wrap = [](int v, int size) {
                const auto n = static_cast<std::uint32_t>(size);
                const std::uint32_t add = n * (0x3FFFFFFFu / n);
                return static_cast<int>((static_cast<std::uint32_t>(v) + add) % n);
            };
            x = wrap(x, bitmap_.width);
            y = wrap(y, bitmap_.height);
        } else {
            x = std::clamp(x, 0, bitmap_.width - 1);
            y = std::clamp(y, 0, bitmap_.height - 1);
        }
        return bitmap_.bgra + static_cast<std::size_t>(y) * stride_
            + static_cast<std::size_t>(x) * 4;
    }

    // render_scanline_aa with span_image_filter_rgba_nn or _bilinear
    // (agg_span_image_filter_rgba.h:36-185) over span_interpolator_linear:
    // each pixel centre (filter_dx_dbl, 0.5) is mapped back into the bitmap
    // in 1/256 pixel, and its colour goes through blend_color_hspan with the
    // pixel's cover.
    class Renderer : public agg::ScanlineSink {
    public:
        Renderer(const BitmapPainter& owner, const RendererRegion& ren,
                 const agg::TransAffine& matrix, bool tile, bool bilinear)
            : owner_(owner)
            , ren_(ren)
            , interpolator_(matrix)
            , tile_(tile)
            , bilinear_(bilinear)
        {
        }

        void render(int y, const agg::Span* spans, unsigned count) override
        {
            for (unsigned i = 0; i < count; ++i) {
                const int x = spans[i].x;
                const int len = spans[i].len < 0 ? -spans[i].len : spans[i].len;
                colors_.resize(static_cast<std::size_t>(len));
                interpolator_.begin(x + 0.5, y + 0.5, static_cast<unsigned>(len));
                for (auto& color : colors_) {
                    int sx = 0;
                    int sy = 0;
                    interpolator_.coordinates(&sx, &sy);
                    color = bilinear_ ? bilinear(sx, sy) : nearest(sx, sy);
                    ++interpolator_;
                }
                ren_.blend_color_hspan(x, y, len, colors_.data(),
                                       spans[i].len < 0 ? nullptr : spans[i].covers,
                                       *spans[i].covers);
            }
        }

    private:
        [[nodiscard]] Color nearest(int x, int y) const
        {
            const u8* p = owner_.source_pixel(x >> 8, y >> 8, tile_);
            return {p[2], p[1], p[0], p[3]};
        }

        [[nodiscard]] Color bilinear(int x_hr, int y_hr) const
        {
            x_hr -= 128; // filter_dx_int: image_subpixel_scale / 2
            y_hr -= 128;
            const int x_lr = x_hr >> 8;
            const int y_lr = y_hr >> 8;
            const auto fx = static_cast<std::uint32_t>(x_hr & 255);
            const auto fy = static_cast<std::uint32_t>(y_hr & 255);
            // Starts at image_subpixel_scale^2 / 2: the sum is rounded.
            std::uint32_t fg[4] {32768, 32768, 32768, 32768};
            const auto add = [&](const u8* p, std::uint32_t weight) {
                for (int c = 0; c < 4; ++c)
                    fg[c] += weight * p[c];
            };
            add(owner_.source_pixel(x_lr, y_lr, tile_), (256 - fx) * (256 - fy));
            add(owner_.source_pixel(x_lr + 1, y_lr, tile_), fx * (256 - fy));
            add(owner_.source_pixel(x_lr, y_lr + 1, tile_), (256 - fx) * fy);
            add(owner_.source_pixel(x_lr + 1, y_lr + 1, tile_), fx * fy);
            return {static_cast<u8>(fg[2] >> 16), static_cast<u8>(fg[1] >> 16),
                    static_cast<u8>(fg[0] >> 16), static_cast<u8>(fg[3] >> 16)};
        }

        const BitmapPainter& owner_;
        const RendererRegion& ren_;
        agg::SpanInterpolatorLinear interpolator_;
        bool tile_;
        bool bilinear_;
        std::vector<Color> colors_;
    };

    // DrawBitmapGeneric<Fill or Tile>::Draw, DrawBitmapGeneric.h:33-127. The
    // translations are float differences, as BPoint and BRect hold them.
    void draw_generic(bool tile)
    {
        const agg::TransAffine& transform = painter_.transform_;
        agg::TransAffine image_matrix = agg::TransAffine::translation(
            offset_.x - destination_.left, offset_.y - destination_.top);
        image_matrix.multiply({scale_x_, 0, 0, scale_y_, 0, 0});
        image_matrix.multiply(
            agg::TransAffine::translation(destination_.left, destination_.top));
        image_matrix.multiply(transform);
        image_matrix.invert();

        BRect rect = destination_;
        const IntBox frame = painter_.region_.frame();
        if (painter_.identity_) {
            rect = {std::max(rect.left, static_cast<float>(frame.left)),
                    std::max(rect.top, static_cast<float>(frame.top)),
                    std::min(rect.right, static_cast<float>(frame.right)),
                    std::min(rect.bottom, static_cast<float>(frame.bottom))};
        }
        // "convert to pixel coords (versus pixel indices)"
        rect.right++;
        rect.bottom++;

        agg::PathStorage& path = painter_.path_;
        path.remove_all();
        path.move_to(rect.left, rect.top);
        path.line_to(rect.right, rect.top);
        path.line_to(rect.right, rect.bottom);
        path.line_to(rect.left, rect.bottom);
        path.close_polygon();

        // fRasterizer, clipped to the region's frame (Painter.cpp:363-367).
        agg::Rasterizer rasterizer(false);
        rasterizer.clip_box(frame.left, frame.top, frame.right + 1, frame.bottom + 1);
        rasterizer.add_path(agg::conv_transform(path.vertices(), transform));
        const PixelFormat pixfmt = painter_.pixel_format();
        const RendererRegion ren(pixfmt, painter_.region_);
        Renderer renderer(*this, ren, image_matrix, tile,
                          (options_ & b_filter_bitmap_bilinear) != 0);
        rasterizer.render_unpacked(renderer);
    }

    Painter& painter_;
    BitmapData bitmap_;
    std::size_t stride_;
    std::size_t size_;
    std::uint32_t options_;
    bool valid_;
    BRect bitmap_bounds_;
    BRect destination_;
    double scale_x_ = 1;
    double scale_y_ = 1;
    BPoint offset_;
};

// ---- DrawingEngine helpers, DrawingEngine.cpp:42-64 -----------------------

void make_rect_valid(BRect& rect)
{
    if (rect.left > rect.right)
        std::swap(rect.left, rect.right);
    if (rect.top > rect.bottom)
        std::swap(rect.top, rect.bottom);
}

void extend_by_stroke_width(BRect& rect, float pen_size)
{
    // "- 0.5" because if stroke width == 1, we don't need to extend
    const float inset = -std::ceil(static_cast<float>(pen_size / 2.0 - 0.5));
    rect.InsetBy(inset, inset);
}

std::vector<BPoint> to_bpoints(std::span<const Point> points)
{
    std::vector<BPoint> out;
    out.reserve(points.size());
    for (const auto p : points)
        out.push_back(to_bpoint(p));
    return out;
}

} // namespace

// Each function below is the DrawingEngine method of the same name
// (DrawingEngine.cpp:643-1231), with its DrawTransaction reduced to the
// IsDirty() test that decides whether anything is drawn.

void fill_rect(Canvas canvas, const DrawState& state, Rect rect, const Gradient* gradient)
{
    // DrawingEngine::FillRect(BRect), :921-936, and (BRect, BGradient), :955-969
    Painter painter(canvas, state);
    BRect r = to_brect(rect);
    make_rect_valid(r);
    r = painter.align_rect(r);
    if (!painter.dirty(painter.transform_and_clip_rect(r)))
        return;
    painter.fill_rect(r, gradient);
}

void fill_rect_color(Canvas canvas, const DrawState& state, Rect rect, Color color)
{
    // DrawingEngine::FillRect(BRect, rgb_color), :856-869. ClipRect floors the
    // left/top and *ceils* the right/bottom before Painter::FillRect truncates
    // and fills inclusively, so a fractional right edge reaches one pixel
    // further here than through FillRect(BRect), which floors it (AlignRect).
    Painter painter(canvas, state);
    BRect r = to_brect(rect);
    make_rect_valid(r);
    r = painter.clip_rect(r);
    if (!painter.dirty(r))
        return;
    painter.fill_rect(r, color);
}

void fill_region(Canvas canvas, const DrawState& state, std::span<const Rect> rects,
                 const Gradient* gradient)
{
    // DrawingEngine::FillRegion(BRegion&), :971-985, and the gradient
    // overload, :987-1001: Painter::FillRect for each rect, no AlignRect.
    if (rects.empty())
        return;
    Painter painter(canvas, state);
    BRect frame = to_brect(rects.front());
    for (const auto& rect : rects) {
        frame.left = std::min(frame.left, rect.left);
        frame.top = std::min(frame.top, rect.top);
        frame.right = std::max(frame.right, rect.right);
        frame.bottom = std::max(frame.bottom, rect.bottom);
    }
    if (!painter.dirty(painter.transform_and_clip_rect(frame)))
        return;
    for (const auto& rect : rects)
        painter.fill_rect(to_brect(rect), gradient);
}

void stroke_rect(Canvas canvas, const DrawState& state, Rect rect, const Gradient* gradient)
{
    // DrawingEngine::StrokeRect(BRect), :904-919, and the gradient overload,
    // :938-953
    Painter painter(canvas, state);
    BRect r = to_brect(rect);
    make_rect_valid(r);
    BRect bounds = r;
    extend_by_stroke_width(bounds, painter.pen_size());
    if (!painter.dirty(painter.transform_and_clip_rect(bounds)))
        return;
    painter.stroke_rect(r, gradient);
}

void stroke_rect_color(Canvas canvas, const DrawState& state, Rect rect, Color color)
{
    // DrawingEngine::StrokeRect(BRect, rgb_color), :842-854
    Painter painter(canvas, state);
    BRect r = to_brect(rect);
    make_rect_valid(r);
    if (!painter.dirty(painter.clip_rect(r)))
        return;
    painter.stroke_rect(r, color);
}

void stroke_line(Canvas canvas, const DrawState& state, Point from, Point to,
                 const Gradient* gradient)
{
    // DrawingEngine::StrokeLine(BPoint, BPoint), :1149-1163, and the gradient
    // overload, :1165-1179
    Painter painter(canvas, state);
    BRect touched {from.x, from.y, to.x, to.y};
    make_rect_valid(touched);
    extend_by_stroke_width(touched, painter.pen_size());
    if (!painter.dirty(painter.transform_and_clip_rect(touched)))
        return;
    painter.stroke_line(to_bpoint(from), to_bpoint(to), gradient);
}

void stroke_line_color(Canvas canvas, const DrawState& state, Point from, Point to,
                       Color color)
{
    // DrawingEngine::StrokeLine(BPoint, BPoint, rgb_color), :816-839. Its
    // transaction is never asked IsDirty(), so it always draws. When the line
    // is neither horizontal nor vertical it strokes with the *current* pen and
    // pattern in B_OP_OVER -- so under B_SOLID_LOW it draws nothing at all
    // (DrawingModeOverSolid.h:36-37).
    Painter painter(canvas, state);
    const BPoint start = to_bpoint(from);
    const BPoint end = to_bpoint(to);
    if (!painter.straight_line(start, end, color)) {
        painter.set_high_color(color);
        painter.set_drawing_mode(DrawingMode::over);
        painter.stroke_line(start, end);
    }
}

void stroke_point_color(Canvas canvas, const DrawState& state, Point point, Color color)
{
    // DrawingEngine::StrokePoint, :806-811
    stroke_line_color(canvas, state, point, point, color);
}

void stroke_line_array(Canvas canvas, const DrawState& state,
                       std::span<const ArrayLine> lines)
{
    // DrawingEngine::StrokeLineArray, :1181-1231: B_SOLID_HIGH and each
    // line's colour as the high colour, in the current mode and pen.
    if (lines.empty())
        return;
    Painter painter(canvas, state);
    BRect touched {std::min(lines[0].from.x, lines[0].to.x),
                   std::min(lines[0].from.y, lines[0].to.y),
                   std::max(lines[0].from.x, lines[0].to.x),
                   std::max(lines[0].from.y, lines[0].to.y)};
    for (std::size_t i = 1; i < lines.size(); i++) {
        touched.left = std::min(touched.left, std::min(lines[i].from.x, lines[i].to.x));
        touched.top = std::min(touched.top, std::min(lines[i].from.y, lines[i].to.y));
        touched.right = std::max(touched.right, std::max(lines[i].from.x, lines[i].to.x));
        touched.bottom = std::max(touched.bottom, std::max(lines[i].from.y, lines[i].to.y));
    }
    extend_by_stroke_width(touched, painter.pen_size());
    if (!painter.dirty(painter.transform_and_clip_rect(touched)))
        return;
    painter.set_solid_high();
    for (const auto& line : lines) {
        painter.set_high_color(line.color);
        painter.stroke_line(to_bpoint(line.from), to_bpoint(line.to));
    }
}

void draw_ellipse(Canvas canvas, const DrawState& state, Rect rect, bool filled,
                  const Gradient* gradient)
{
    // DrawingEngine::DrawEllipse, :719-767
    Painter painter(canvas, state);
    BRect r = to_brect(rect);
    make_rect_valid(r);
    BRect bounds = r;
    painter.align_ellipse_rect(&bounds, filled);
    if (!filled)
        extend_by_stroke_width(bounds, painter.pen_size());
    bounds.left = std::floor(bounds.left);
    bounds.top = std::floor(bounds.top);
    bounds.right = std::ceil(bounds.right);
    bounds.bottom = std::ceil(bounds.bottom);
    if (!painter.dirty(painter.transform_and_clip_rect(bounds)))
        return;
    painter.draw_ellipse(r, filled, gradient);
}

void draw_arc(Canvas canvas, const DrawState& state, Rect rect, float angle,
              float span, bool filled, const Gradient* gradient)
{
    // DrawingEngine::DrawArc, :643-693. The gradient overload aligns the rect
    // as filled even for a stroke and does not extend the bounds.
    Painter painter(canvas, state);
    BRect r = to_brect(rect);
    make_rect_valid(r);
    painter.align_ellipse_rect(&r, gradient != nullptr ? true : filled);
    BRect bounds = r;
    if (!filled && gradient == nullptr)
        extend_by_stroke_width(bounds, painter.pen_size());
    if (!painter.dirty(painter.transform_and_clip_rect(bounds)))
        return;

    const float x_radius = static_cast<float>(r.Width() / 2.0);
    const float y_radius = static_cast<float>(r.Height() / 2.0);
    const BPoint center {r.left + x_radius, r.top + y_radius};

    if (filled)
        painter.fill_arc(center, x_radius, y_radius, angle, span, gradient);
    else
        painter.stroke_arc(center, x_radius, y_radius, angle, span, gradient);
}

void draw_round_rect(Canvas canvas, const DrawState& state, Rect rect,
                     float x_radius, float y_radius, bool filled,
                     const Gradient* gradient)
{
    // DrawingEngine::DrawRoundRect, :1003-1054. For a stroke the pen
    // extension is applied to the rect itself, not just to the dirty bounds,
    // so Painter strokes a rect grown by ceil(pen / 2 - 0.5) on every side:
    // a 2px stroke lands a pixel outside the rect asked for. Kept.
    Painter painter(canvas, state);
    BRect r = to_brect(rect);
    make_rect_valid(r);
    if (!filled)
        extend_by_stroke_width(r, painter.pen_size());
    BRect bounds = painter.transform_and_clip_rect(r);
    bounds.left = std::floor(bounds.left);
    bounds.top = std::floor(bounds.top);
    bounds.right = std::ceil(bounds.right);
    bounds.bottom = std::ceil(bounds.bottom);
    if (!painter.dirty(bounds))
        return;
    if (filled)
        painter.fill_round_rect(r, x_radius, y_radius, gradient);
    else
        painter.stroke_round_rect(r, x_radius, y_radius, gradient);
}

void draw_polygon(Canvas canvas, const DrawState& state, std::span<const Point> points,
                  Rect bounds, bool filled, bool closed, const Gradient* gradient)
{
    // DrawingEngine::DrawPolygon, :769-801
    Painter painter(canvas, state);
    BRect b = to_brect(bounds);
    make_rect_valid(b);
    if (!filled)
        extend_by_stroke_width(b, painter.pen_size());
    if (!painter.dirty(painter.transform_and_clip_rect(b)))
        return;
    painter.draw_polygon(to_bpoints(points), filled, closed, gradient);
}

void draw_bezier(Canvas canvas, const DrawState& state,
                 std::span<const Point, 4> points, bool filled,
                 const Gradient* gradient)
{
    // DrawingEngine::DrawBezier, :695-717: the transaction is the whole
    // clipping region.
    Painter painter(canvas, state);
    if (!painter.dirty())
        return;
    painter.draw_bezier({to_bpoint(points[0]), to_bpoint(points[1]),
                         to_bpoint(points[2]), to_bpoint(points[3])},
                        filled, gradient);
}

void draw_triangle(Canvas canvas, const DrawState& state,
                   std::span<const Point, 3> points, Rect bounds, bool filled,
                   const Gradient* gradient)
{
    // DrawingEngine::DrawTriangle, :1110-1147
    Painter painter(canvas, state);
    BRect b = to_brect(bounds);
    if (!filled)
        extend_by_stroke_width(b, painter.pen_size());
    if (!painter.dirty(painter.transform_and_clip_rect(b)))
        return;
    painter.draw_triangle(to_bpoint(points[0]), to_bpoint(points[1]),
                          to_bpoint(points[2]), filled, gradient);
}

void draw_shape(Canvas canvas, const DrawState& state, const ShapeData& shape,
                bool filled, const Gradient* gradient)
{
    // DrawingEngine::DrawShape, :1056-1108: the transaction is the whole
    // clipping region (the bounds check is commented out upstream).
    Painter painter(canvas, state);
    if (!painter.dirty())
        return;
    painter.draw_shape(shape, filled, gradient);
}

// One pixel of subpixel (LCD) text. app_server draws every glyph under
// SolidPatternGuard (Painter.cpp:1492, :1513): the pattern is B_SOLID_HIGH and
// the renderer colour is the high colour (Painter.cpp:466-476, :1720-1726), and
// the glyph scanline is handed to fBlendSolidHSpanSubpix -- the same span the
// solid shapes above end in (agg_renderer_scanline_subpix.h:21-49).
bool blend_text_subpix(Canvas canvas, const DrawState& state, int x, int y,
                       Color color, const std::uint8_t covers[3], bool ordering_rgb)
{
    PatternHandler pattern;
    pattern.pattern.fill(0xff);
    pattern.high = color;
    pattern.low = state.low;
    pattern.x_offset = state.x_offset;
    pattern.y_offset = state.y_offset;
    const PixelFormat format(canvas, pattern, state.drawing_mode, state.constant_alpha,
                             state.alpha_function, ordering_rgb);
    return format.blend_solid_hspan_subpix(x, y, 3, color, covers);
}

void draw_bitmap(Canvas canvas, const DrawState& state, BitmapData bitmap,
                 Rect bitmap_rect, Rect view_rect, std::uint32_t options)
{
    // DrawingEngine::DrawBitmap, DrawingEngine.cpp:633-641, then
    // Painter::DrawBitmap, Painter.cpp:1567-1579: nothing is drawn unless the
    // view rect is dirty, the clipping valid, and the view rect still valid
    // once transformed, aligned and clipped.
    Painter painter(canvas, state);
    const BRect view = to_brect(view_rect);
    if (!painter.dirty(painter.transform_and_clip_rect(view)))
        return;
    if (!painter.valid_clipping())
        return;
    if (!painter.transform_align_and_clip_rect(view).IsValid())
        return;
    BitmapPainter(painter, bitmap, options).draw(to_brect(bitmap_rect), view);
}

} // namespace haiku_remote::painter
