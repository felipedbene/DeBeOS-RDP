// Render-fidelity tests: shapes against app_server's own Painter, and the
// three RP_SET_FONT attributes that used to be decoded and dropped.
//
// The shape expectations are golden frames produced by app_server's sources
// (tools/painter_oracle), not by the code under test. The text expectations
// are relations between two renders of the same string (rotated versus not),
// which collapse to equality the moment the transform stops being applied.

#include "haiku_remote/session.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_FONT_FORMATS_H
#include FT_MULTIPLE_MASTERS_H

#include "painter_scenes.hpp"
#include "text_blend_cases.hpp"
#include "test_fonts.hpp"
#include "text_scenes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

struct InkBox {
    int left = 0;
    int top = 0;
    int right = -1;
    int bottom = -1;
    int count = 0;

    [[nodiscard]] int width() const { return right - left + 1; }
    [[nodiscard]] int height() const { return bottom - top + 1; }
};

// Everything darker than mid-grey counts as ink; the surfaces below start
// white and are drawn on in black.
InkBox measure_ink(const Surface& surface)
{
    InkBox box;
    bool first = true;
    for (int y = 0; y < surface.height(); ++y) {
        for (int x = 0; x < surface.width(); ++x) {
            if (surface.pixel(x, y).r >= 200)
                continue;
            ++box.count;
            if (first) {
                box.left = box.right = x;
                box.top = box.bottom = y;
                first = false;
                continue;
            }
            box.left = std::min(box.left, x);
            box.right = std::max(box.right, x);
            box.top = std::min(box.top, y);
            box.bottom = std::max(box.bottom, y);
        }
    }
    return box;
}

// First and last row holding any pixel that is not pure white.
std::pair<int, int> touched_rows(const Surface& surface)
{
    int first = -1;
    int last = -1;
    for (int y = 0; y < surface.height(); ++y) {
        for (int x = 0; x < surface.width(); ++x) {
            const auto pixel = surface.pixel(x, y);
            if (pixel.r == 255 && pixel.g == 255 && pixel.b == 255)
                continue;
            if (first < 0)
                first = y;
            last = y;
            break;
        }
    }
    return {first, last};
}

// -- Shapes: app_server's Painter, pixel for pixel ------------------------
//
// Every scene in painter_scenes.hpp is one DrawingEngine call. The expected
// frames in painter_goldens.inc were produced by tools/painter_oracle, which
// compiles app_server's own DrawingEngine, Painter, PixelFormat, drawing modes
// and AGG sources on the host and runs the same scenes through them -- they
// are not recordings of this client's output, so a defect here cannot agree
// with itself. Each scene is checked three ways, all exact: the FNV-1a hash of
// every RGB byte, the number of pixels the call changed, and twelve pixels
// spread through those that changed, so a failure says where as well as that.
// `make painter-oracle-check` regenerates the goldens from a Haiku tree and,
// built with --full, gives a per-pixel diff.

struct PainterSample {
    int x;
    int y;
    std::uint32_t rgb;
};

struct PainterGolden {
    const char* name;
    int width;
    int height;
    std::uint64_t hash;
    std::size_t changed;
    std::vector<PainterSample> samples;
    const char* full;
};

const std::vector<PainterGolden>& painter_goldens()
{
    static const std::vector<PainterGolden> goldens = {
// make painter-oracle-check points PAINTER_GOLDENS at a --full build of the
// goldens, whose frames turn a mismatch into a per-pixel report.
#ifdef PAINTER_GOLDENS
#include PAINTER_GOLDENS
#else
#include "painter_goldens.inc"
#endif
    };
    return goldens;
}

// FNV-1a 64, as tools/painter_oracle/oracle.cpp computes it.
std::uint64_t fnv1a(const std::vector<std::uint8_t>& rgb)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : rgb) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

Color scene_color(const std::uint8_t (&c)[4])
{
    return {c[0], c[1], c[2], c[3]};
}

DrawState scene_state(const painter_scenes::Scene& scene)
{
    DrawState state;
    state.high = scene_color(scene.high);
    state.low = scene_color(scene.low);
    state.pen_size = scene.pen;
    state.drawing_mode = static_cast<DrawingMode>(scene.mode);
    state.constant_alpha = scene.source_alpha == painter_scenes::constant_alpha;
    state.alpha_function = scene.alpha_function;
    std::copy(std::begin(scene.pattern), std::end(scene.pattern),
              state.pattern.begin());
    state.line_cap = scene.cap;
    state.line_join = scene.join;
    state.miter_limit = scene.miter;
    state.transform = {scene.transform[0], scene.transform[1], scene.transform[2],
                       scene.transform[3], scene.transform[4], scene.transform[5]};
    state.x_offset = scene.x_offset;
    state.y_offset = scene.y_offset;
    state.subpixel_precise = scene.subpixel_precise;
    if (!scene.clip.empty()) {
        state.clipping_set = true;
        for (const auto& r : scene.clip) {
            state.clip_rects.push_back({static_cast<float>(r.left),
                                        static_cast<float>(r.top),
                                        static_cast<float>(r.right),
                                        static_cast<float>(r.bottom)});
        }
    }
    return state;
}

Rect bounds_of_points(const std::vector<painter_scenes::P>& points)
{
    Rect bounds {points[0].x, points[0].y, points[0].x, points[0].y};
    for (const auto& p : points) {
        bounds.left = std::min(bounds.left, p.x);
        bounds.top = std::min(bounds.top, p.y);
        bounds.right = std::max(bounds.right, p.x);
        bounds.bottom = std::max(bounds.bottom, p.y);
    }
    return bounds;
}

// The client's render of one scene, through the Surface method the session
// calls for the corresponding RP_* message.
std::vector<std::uint8_t> render_scene(const painter_scenes::Scene& scene)
{
    using painter_scenes::Op;
    Surface surface(scene.width, scene.height);
    for (int y = 0; y < scene.height; ++y) {
        for (int x = 0; x < scene.width; ++x) {
            std::uint8_t rgb[3];
            painter_scenes::background(scene, x, y, rgb);
            surface.set_pixel(x, y, {rgb[0], rgb[1], rgb[2], 255});
        }
    }
    const DrawState state = scene_state(scene);
    Gradient gradient;
    gradient.kind = static_cast<Gradient::Kind>(scene.gradient_kind);
    gradient.start = {scene.gradient_start.x, scene.gradient_start.y};
    gradient.end = {scene.gradient_end.x, scene.gradient_end.y};
    gradient.center = {scene.gradient_center.x, scene.gradient_center.y};
    gradient.focal = {scene.gradient_focal.x, scene.gradient_focal.y};
    gradient.radius = scene.gradient_radius;
    gradient.angle = scene.gradient_angle;
    if (scene.gradient_stops.empty()) {
        gradient.stops = {{scene_color(scene.gradient_color), 0},
                          {scene_color(scene.gradient_color), 255}};
    } else {
        for (const auto& stop : scene.gradient_stops)
            gradient.stops.push_back({scene_color(stop.color), stop.offset});
    }
    const Gradient* g = scene.gradient ? &gradient : nullptr;
    const Rect rect {scene.rect[0], scene.rect[1], scene.rect[2], scene.rect[3]};
    std::vector<Point> points;
    for (const auto& p : scene.points)
        points.push_back({p.x, p.y});
    const Color color = scene_color(scene.color);

    switch (scene.op) {
    case Op::fill_rect:
        surface.fill_rect(rect, state, g);
        break;
    case Op::fill_rect_color:
        surface.fill_rect_color(rect, color, &state);
        break;
    case Op::stroke_rect:
        surface.stroke_rect(rect, state, g);
        break;
    case Op::stroke_rect_color:
        surface.stroke_rect(rect, color, &state);
        break;
    case Op::stroke_line:
        surface.stroke_line(points[0], points[1], state, g);
        break;
    case Op::stroke_line_color:
        surface.stroke_line_color(points[0], points[1], color, state);
        break;
    case Op::stroke_point_color:
        surface.stroke_point_color(points[0], color, state);
        break;
    case Op::stroke_line_array: {
        std::vector<ColoredLine> lines;
        for (const auto& line : scene.lines) {
            lines.push_back({{line.from.x, line.from.y}, {line.to.x, line.to.y},
                             scene_color(line.color)});
        }
        surface.stroke_line_array(lines, state);
        break;
    }
    case Op::fill_ellipse:
        surface.fill_ellipse(rect, state, g);
        break;
    case Op::stroke_ellipse:
        surface.stroke_ellipse(rect, state, g);
        break;
    case Op::fill_arc:
        surface.fill_arc(rect, scene.a, scene.b, state, g);
        break;
    case Op::stroke_arc:
        surface.stroke_arc(rect, scene.a, scene.b, state, g);
        break;
    case Op::fill_round_rect:
        surface.fill_round_rect(rect, scene.a, scene.b, state, g);
        break;
    case Op::stroke_round_rect:
        surface.stroke_round_rect(rect, scene.a, scene.b, state, g);
        break;
    case Op::fill_polygon:
    case Op::stroke_polygon:
        surface.draw_polygon(points, bounds_of_points(scene.points),
                             scene.op == Op::fill_polygon, scene.closed, state, g);
        break;
    case Op::fill_bezier:
    case Op::stroke_bezier: {
        const std::array<Point, 4> bezier {points[0], points[1], points[2], points[3]};
        surface.draw_bezier(bezier, scene.op == Op::fill_bezier, state, g);
        break;
    }
    case Op::fill_triangle:
    case Op::stroke_triangle: {
        const std::array<Point, 3> triangle {points[0], points[1], points[2]};
        surface.draw_triangle(triangle, rect, scene.op == Op::fill_triangle, state, g);
        break;
    }
    case Op::fill_shape:
    case Op::stroke_shape:
        surface.draw_shape(scene.shape_ops, points,
                           {scene.shape_offset.x, scene.shape_offset.y},
                           scene.shape_scale, scene.op == Op::fill_shape, state, g);
        break;
    case Op::fill_region: {
        std::vector<Rect> region;
        for (const auto& r : scene.region) {
            region.push_back({static_cast<float>(r.left), static_cast<float>(r.top),
                              static_cast<float>(r.right), static_cast<float>(r.bottom)});
        }
        surface.fill_region(region, state, g);
        break;
    }
    case Op::draw_bitmap: {
        // The bitmap as RemoteMessage::AddBitmap sends it (RemoteMessage.cpp:
        // 169-190: width, height, bytesPerRow, colorSpace, flags, bitsLength,
        // bits; both 32-bit spaces are BGRA in memory), decoded by the
        // client's own decode_bitmap for the scene's mode -- so the colour
        // space handling is under test, not assumed.
        std::vector<std::uint8_t> wire;
        const auto put32 = [&wire](std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
                wire.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        };
        const auto row_bytes = static_cast<std::uint32_t>(scene.bitmap_width * 4);
        put32(static_cast<std::uint32_t>(scene.bitmap_width));
        put32(static_cast<std::uint32_t>(scene.bitmap_height));
        put32(row_bytes);
        put32(scene.bitmap_color_space);
        put32(0);
        put32(row_bytes * static_cast<std::uint32_t>(scene.bitmap_height));
        for (int y = 0; y < scene.bitmap_height; ++y) {
            for (int x = 0; x < scene.bitmap_width; ++x) {
                std::uint8_t rgba[4];
                painter_scenes::bitmap_pixel(x, y, rgba, scene.bitmap_color_space);
                wire.insert(wire.end(), {rgba[2], rgba[1], rgba[0], rgba[3]});
            }
        }
        Reader reader(wire);
        const Bitmap bitmap = decode_bitmap(
            reader, static_cast<DrawingMode>(scene.mode), {});
        surface.draw_bitmap(bitmap,
                            {scene.source[0], scene.source[1], scene.source[2],
                             scene.source[3]},
                            rect, state, scene.bitmap_options);
        break;
    }
    }

    std::vector<std::uint8_t> rgb;
    rgb.reserve(static_cast<std::size_t>(scene.width * scene.height * 3));
    for (int y = 0; y < scene.height; ++y) {
        for (int x = 0; x < scene.width; ++x) {
            const auto p = surface.pixel(x, y);
            rgb.push_back(p.r);
            rgb.push_back(p.g);
            rgb.push_back(p.b);
        }
    }
    return rgb;
}

// The --full run-length frame, when the goldens were built with it.
std::vector<std::uint8_t> decode_full(const char* text)
{
    std::vector<std::uint8_t> rgb;
    std::string_view rest(text);
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        auto token = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
        std::size_t run = 1;
        if (const auto star = token.find('*'); star != std::string_view::npos) {
            run = std::stoul(std::string(token.substr(0, star)));
            token = token.substr(star + 1);
        }
        const auto value = std::stoul(std::string(token), nullptr, 16);
        for (std::size_t i = 0; i < run; ++i) {
            rgb.push_back(static_cast<std::uint8_t>(value >> 16));
            rgb.push_back(static_cast<std::uint8_t>(value >> 8));
            rgb.push_back(static_cast<std::uint8_t>(value));
        }
    }
    return rgb;
}

// Subpixel text against app_server's own spans: every case in
// text_blend_cases.hpp, whose golden was produced by tools/painter_oracle
// driving the server's Painter setters and PixelFormat subpixel span
// (text_blend.cpp). Both the pure one-pixel view and the production entry
// point, Surface::paint_subpixel_coverage (RGB only -- the wire carries no
// subpixel order, so the surface always uses the server default), are held to
// it, pixel for pixel.
const std::vector<std::uint32_t>& text_blend_goldens()
{
    static const std::vector<std::uint32_t> goldens = {
#include "text_blend_goldens.inc"
    };
    return goldens;
}

void test_subpixel_text_matches_app_servers_spans()
{
    using namespace text_blend_cases;
    const auto& goldens = text_blend_goldens();
    std::size_t index = 0;
    std::size_t matching = 0;
    std::size_t surface_matching = 0;
    std::size_t surface_cases = 0;
    std::size_t first_bad = SIZE_MAX;
    std::size_t first_surface_bad = SIZE_MAX;
    for_each_case([&](const Case& c) {
        const std::size_t i = index++;
        if (i >= goldens.size())
            return;
        DrawState state;
        state.drawing_mode = static_cast<DrawingMode>(c.mode.mode);
        state.constant_alpha = c.mode.source_alpha == 1;
        state.alpha_function = c.mode.alpha_function;
        state.high = {c.high.r, c.high.g, c.high.b, c.high.a};
        state.low = {low.r, low.g, low.b, low.a};
        const Color destination {c.destination.r, c.destination.g,
                                 c.destination.b, c.destination.a};
        const std::uint32_t want = goldens[i];
        const auto rgb = [](Color p) {
            return (static_cast<std::uint32_t>(p.r) << 16)
                | (static_cast<std::uint32_t>(p.g) << 8) | p.b;
        };
        const Color got = blend_subpixel_text(destination, state, c.covers.c[0],
                                              c.covers.c[1], c.covers.c[2],
                                              c.ordering_rgb)
                              .value_or(destination);
        if (rgb(got) == want)
            ++matching;
        else if (first_bad == SIZE_MAX)
            first_bad = i;
        if (c.ordering_rgb) {
            ++surface_cases;
            Surface surface(1, 1);
            surface.set_pixel(0, 0, destination);
            surface.paint_subpixel_coverage(0, 0, state.high, state, c.covers.c[0],
                                            c.covers.c[1], c.covers.c[2]);
            if (rgb(surface.pixel(0, 0)) == want)
                ++surface_matching;
            else if (first_surface_bad == SIZE_MAX)
                first_surface_bad = i;
        }
    });
    check(index == goldens.size(),
          "text_blend_goldens.inc has one value per case (regenerate it with "
          "make painter-goldens)");
    check(matching == goldens.size(),
          "subpixel text blends every case exactly as app_server's spans ("
              + std::to_string(matching) + " of " + std::to_string(goldens.size())
              + "; first mismatch at case " + std::to_string(first_bad) + ")");
    check(surface_matching == surface_cases,
          "Surface::paint_subpixel_coverage blends every RGB case exactly as"
          " app_server's spans (" + std::to_string(surface_matching) + " of "
              + std::to_string(surface_cases) + "; first mismatch at case "
              + std::to_string(first_surface_bad) + ")");
    std::cout << "painter: " << matching << " of " << goldens.size()
              << " subpixel text-blend cases match app_server's spans\n";
}

void test_shapes_match_app_servers_painter()
{
    const auto scenes = painter_scenes::scenes();
    const auto& goldens = painter_goldens();
    check(scenes.size() == goldens.size(),
          "painter_goldens.inc has one entry per scene (regenerate it with "
          "make painter-goldens)");
    const std::size_t count = std::min(scenes.size(), goldens.size());
    int matching = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& scene = scenes[i];
        const auto& golden = goldens[i];
        const std::string name = scene.name;
        check(name == golden.name && scene.width == golden.width
                  && scene.height == golden.height,
              "golden " + std::to_string(i) + " is for scene " + name);
        const auto rgb = render_scene(scene);

        std::size_t changed = 0;
        for (int y = 0; y < scene.height; ++y) {
            for (int x = 0; x < scene.width; ++x) {
                std::uint8_t bg[3];
                painter_scenes::background(scene, x, y, bg);
                const auto at = static_cast<std::size_t>((y * scene.width + x) * 3);
                if (std::memcmp(bg, &rgb[at], 3) != 0)
                    ++changed;
            }
        }
        bool samples_ok = true;
        std::ostringstream detail;
        for (const auto& sample : golden.samples) {
            const auto at = static_cast<std::size_t>((sample.y * scene.width + sample.x) * 3);
            const std::uint32_t got = (std::uint32_t {rgb[at]} << 16)
                | (std::uint32_t {rgb[at + 1]} << 8) | rgb[at + 2];
            if (got != sample.rgb) {
                samples_ok = false;
                detail << " (" << sample.x << "," << sample.y << ") is " << std::hex
                       << got << " expected " << sample.rgb << std::dec;
            }
        }
        const bool hash_ok = fnv1a(rgb) == golden.hash;
        check(samples_ok, name + ": sampled pixels match app_server" + detail.str());
        check(changed == golden.changed,
              name + ": changes " + std::to_string(changed) + " pixels, app_server "
                  + std::to_string(golden.changed));
        check(hash_ok, name + ": every pixel matches app_server (FNV-1a)");
        if (golden.full != nullptr && golden.full[0] != '\0') {
            const auto expected = decode_full(golden.full);
            int reported = 0;
            for (std::size_t p = 0; p * 3 < expected.size() && p * 3 < rgb.size(); ++p) {
                if (std::memcmp(&expected[p * 3], &rgb[p * 3], 3) == 0)
                    continue;
                if (reported++ < 8) {
                    std::cerr << "  " << name << " (" << p % scene.width << ","
                              << p / scene.width << "): client " << std::hex
                              << int(rgb[p * 3]) << "," << int(rgb[p * 3 + 1]) << ","
                              << int(rgb[p * 3 + 2]) << " server " << int(expected[p * 3])
                              << "," << int(expected[p * 3 + 1]) << ","
                              << int(expected[p * 3 + 2]) << std::dec << '\n';
                }
            }
        }
        if (samples_ok && hash_ok && changed == golden.changed)
            ++matching;
    }
    std::cout << "painter: " << matching << " of " << count
              << " scenes match app_server pixel for pixel\n";
}

// -- Text: app_server's AGGTextRenderer and FreeType, pixel for pixel -------
//
// Every scene in text_scenes.hpp is one Painter::DrawString. text_goldens.inc
// was produced by tools/painter_oracle --text, which runs the server's own
// AGGTextRenderer, GlyphLayoutEngine, FontCacheEntry, FontEngine and Painter
// with glyphs from the vendored FreeType 2.6.3 (third_party/freetype), on the
// faces in tests/fonts. The client draws the same string through TextEngine
// the way the session does for RP_DRAW_STRING, with the font identity block
// the server would send, and every pixel must be equal.

const std::vector<PainterGolden>& text_goldens()
{
    static const std::vector<PainterGolden> goldens = {
// make painter-oracle-check points TEXT_GOLDENS at a --text-full build.
#ifdef TEXT_GOLDENS
#include TEXT_GOLDENS
#else
#include "text_goldens.inc"
#endif
    };
    return goldens;
}

std::filesystem::path test_font_dir() { return test_fonts::dir(); }

// The identity block RemoteMessage::AddFontIdentity sends for the scene's
// font: FreeType's names, the 'head' checksum and revision (none for Type 1),
// ServerFont::Hinting() (ServerFont.h:206-217) and the desktop's subpixel
// setting.
std::shared_ptr<FontIdentity> test_face_identity(int index)
{
    const auto& face = text_scenes::faces()[static_cast<std::size_t>(index)];
    auto identity = std::make_shared<FontIdentity>();
    identity->family = face.family;
    identity->style = face.style;
    identity->path = "/boot/system/data/fonts/" + std::string(face.file);
    FT_Library library = nullptr;
    FT_Face ft = nullptr;
    if (FT_Init_FreeType(&library) == 0
        && FT_New_Face(library, (test_font_dir() / face.file).string().c_str(), 0,
                       &ft) == 0) {
        if (const auto* head = static_cast<const TT_Header*>(
                FT_Get_Sfnt_Table(ft, FT_SFNT_HEAD))) {
            identity->checksum_adjustment
                = static_cast<std::uint32_t>(head->CheckSum_Adjust);
            identity->font_revision = static_cast<std::uint32_t>(head->Font_Revision);
        }
        FT_Done_Face(ft);
    }
    if (library != nullptr)
        FT_Done_FreeType(library);
    return identity;
}

std::shared_ptr<const FontIdentity> scene_identity(const text_scenes::Scene& scene)
{
    auto identity = test_face_identity(scene.face);
    const auto& face = text_scenes::faces()[static_cast<std::size_t>(scene.face)];
    identity->hinting = scene.hinting == text_scenes::hinting_on
        || (scene.hinting == text_scenes::hinting_monospaced_only && face.fixed);
    identity->subpixel = scene.subpixel;
    return identity;
}

// Every committed test face, registered with the engines the way a fetched
// font is. Idempotent.
void add_test_fonts()
{
    std::vector<std::string> files;
    for (const auto& face : text_scenes::faces())
        files.push_back((test_font_dir() / face.file).string());
    TextEngine::add_font_files(files);
}

std::vector<std::uint8_t> render_text_scene(TextEngine& text,
                                            const text_scenes::Scene& scene)
{
    Surface surface(scene.width, scene.height);
    for (int y = 0; y < scene.height; ++y) {
        for (int x = 0; x < scene.width; ++x) {
            std::uint8_t rgb[3];
            text_scenes::background(x, y, rgb);
            surface.set_pixel(x, y, {rgb[0], rgb[1], rgb[2], 255});
        }
    }
    DrawState state;
    state.high = scene_color(scene.high);
    state.low = scene_color(scene.low);
    state.drawing_mode = static_cast<DrawingMode>(scene.mode);
    state.constant_alpha = scene.source_alpha == 1;
    state.alpha_function = scene.alpha_function;
    state.transform = {scene.transform[0], scene.transform[1], scene.transform[2],
                       scene.transform[3], scene.transform[4], scene.transform[5]};
    state.font.size = scene.size;
    state.font.flags = scene.flags;
    state.font.spacing = scene.spacing;
    state.font.face = scene.face_bits;
    state.font.rotation = scene.rotation;
    state.font.shear = scene.shear;
    state.font.false_bold_width = scene.false_bold;
    state.font.identity = scene_identity(scene);
    // The session's RP_DRAW_STRING: Painter::DrawString rounds the baseline
    // (Painter.cpp:1485-1488) unless the view is B_SUBPIXEL_PRECISE.
    const Point baseline {std::round(scene.x), std::round(scene.y)};
    text.draw(scene.text, baseline, state, surface);
    std::vector<std::uint8_t> rgb;
    rgb.reserve(static_cast<std::size_t>(scene.width * scene.height * 3));
    for (int y = 0; y < scene.height; ++y) {
        for (int x = 0; x < scene.width; ++x) {
            const auto c = surface.pixel(x, y);
            rgb.push_back(c.r);
            rgb.push_back(c.g);
            rgb.push_back(c.b);
        }
    }
    return rgb;
}

void test_text_matches_app_servers_renderer()
{
#ifdef HAIKU_REMOTE_SYSTEM_FREETYPE
    std::cout << "SKIP: text goldens (built with HAIKU_REMOTE_FREETYPE=system, whose"
                 " glyphs are not the server's)\n";
#else
    const auto scenes = text_scenes::scenes();
    const auto& goldens = text_goldens();
    check(scenes.size() == goldens.size(),
          "text_goldens.inc has one entry per scene (regenerate it with "
          "make painter-goldens)");
    add_test_fonts();
    TextEngine text;
    const std::size_t count = std::min(scenes.size(), goldens.size());
    int matching = 0;
    std::size_t differing_pixels = 0;
    bool full = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& scene = scenes[i];
        const auto& golden = goldens[i];
        const std::string name = scene.name;
        check(name == golden.name && scene.width == golden.width
                  && scene.height == golden.height,
              "text golden " + std::to_string(i) + " is for scene " + name);
        Font font;
        font.identity = scene_identity(scene);
        const auto face = text.selected_face(font);
        const auto expected_file = test_font_dir()
            / text_scenes::faces()[static_cast<std::size_t>(scene.face)].file;
        // An sfnt face is found by its 'head' checksum; a Type 1 face has none
        // and is found by name.
        check((face.identical || (font.identity->checksum_adjustment == 0 && face.named))
                  && std::filesystem::path(face.path) == expected_file,
              name + ": drawn from the scene's own font file (got " + face.path + ")");
        const auto rgb = render_text_scene(text, scene);
        std::size_t changed = 0;
        for (int y = 0; y < scene.height; ++y) {
            for (int x = 0; x < scene.width; ++x) {
                std::uint8_t bg[3];
                text_scenes::background(x, y, bg);
                const auto at = static_cast<std::size_t>((y * scene.width + x) * 3);
                if (std::memcmp(bg, &rgb[at], 3) != 0)
                    ++changed;
            }
        }
        bool samples_ok = true;
        std::ostringstream detail;
        for (const auto& sample : golden.samples) {
            const auto at
                = static_cast<std::size_t>((sample.y * scene.width + sample.x) * 3);
            const std::uint32_t got = (std::uint32_t {rgb[at]} << 16)
                | (std::uint32_t {rgb[at + 1]} << 8) | rgb[at + 2];
            if (got != sample.rgb) {
                samples_ok = false;
                detail << " (" << sample.x << "," << sample.y << ") is " << std::hex
                       << got << " expected " << sample.rgb << std::dec;
            }
        }
        const bool hash_ok = fnv1a(rgb) == golden.hash;
        check(samples_ok, name + ": sampled pixels match app_server" + detail.str());
        check(changed == golden.changed,
              name + ": changes " + std::to_string(changed) + " pixels, app_server "
                  + std::to_string(golden.changed));
        check(hash_ok, name + ": every pixel matches app_server (FNV-1a)");
        if (golden.full != nullptr && golden.full[0] != '\0') {
            full = true;
            const auto expected = decode_full(golden.full);
            int reported = 0;
            std::size_t differing = 0;
            for (std::size_t p = 0; p * 3 < expected.size() && p * 3 < rgb.size(); ++p) {
                if (std::memcmp(&expected[p * 3], &rgb[p * 3], 3) == 0)
                    continue;
                ++differing;
                if (reported++ < 6) {
                    std::cerr << "  " << name << " (" << p % scene.width << ","
                              << p / scene.width << "): client " << std::hex
                              << int(rgb[p * 3]) << "," << int(rgb[p * 3 + 1]) << ","
                              << int(rgb[p * 3 + 2]) << " server "
                              << int(expected[p * 3]) << "," << int(expected[p * 3 + 1])
                              << "," << int(expected[p * 3 + 2]) << std::dec << '\n';
                }
            }
            if (differing != 0)
                std::cerr << "  " << name << ": " << differing << " pixels differ\n";
            differing_pixels += differing;
        }
        if (samples_ok && hash_ok && changed == golden.changed)
            ++matching;
    }
    std::cout << "text: " << matching << " of " << count
              << " scenes match app_server pixel for pixel";
    if (full)
        std::cout << " (" << differing_pixels << " pixels differ in all)";
    std::cout << '\n';
#endif
}

// One text scene, by name, against app_server's own render of it
// (text_goldens.inc): every pixel equal, and the string's whole ink inside the
// canvas -- a scene whose ink ran off an edge would say nothing about the part
// that did. `detail` explains a failure.
#ifndef HAIKU_REMOTE_SYSTEM_FREETYPE
bool scene_matches_app_server(std::string_view name, std::string& detail)
{
    const auto scenes = text_scenes::scenes();
    const auto& goldens = text_goldens();
    for (std::size_t i = 0; i < scenes.size() && i < goldens.size(); ++i) {
        const auto& scene = scenes[i];
        if (name != scene.name)
            continue;
        if (std::string_view(goldens[i].name) != name) {
            detail = "text_goldens.inc is out of step with text_scenes.hpp";
            return false;
        }
        add_test_fonts();
        TextEngine text;
        const auto rgb = render_text_scene(text, scene);
        std::size_t changed = 0;
        int left = scene.width;
        int right = -1;
        int top = scene.height;
        int bottom = -1;
        for (int y = 0; y < scene.height; ++y) {
            for (int x = 0; x < scene.width; ++x) {
                std::uint8_t bg[3];
                text_scenes::background(x, y, bg);
                const auto at = static_cast<std::size_t>((y * scene.width + x) * 3);
                if (std::memcmp(bg, &rgb[at], 3) == 0)
                    continue;
                ++changed;
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
        detail = "changes " + std::to_string(changed) + " pixels, app_server "
            + std::to_string(goldens[i].changed) + "; ink box x " + std::to_string(left)
            + ".." + std::to_string(right) + ", y " + std::to_string(top) + ".."
            + std::to_string(bottom) + " on " + std::to_string(scene.width) + "x"
            + std::to_string(scene.height);
        const bool inside = left > 0 && top > 0 && right < scene.width - 1
            && bottom < scene.height - 1;
        return inside && changed == goldens[i].changed
            && fnv1a(rgb) == goldens[i].hash;
    }
    detail = "no text scene is called " + std::string(name);
    return false;
}
#endif

// The check form of scene_matches_app_server. A HAIKU_REMOTE_FREETYPE=system
// build skips it, as it skips the text goldens.
void check_scene_is_app_servers(std::string_view name, const std::string& what)
{
#ifdef HAIKU_REMOTE_SYSTEM_FREETYPE
    (void)name;
    std::cout << "SKIP: " << what << " (built with HAIKU_REMOTE_FREETYPE=system,"
                 " whose glyphs are not the server's)\n";
#else
    std::string detail;
    const bool ok = scene_matches_app_server(name, detail);
    check(ok, what + " (" + detail + ")");
#endif
}

void test_stroke_defaults_are_app_servers()
{
    // DrawState.cpp:62-64: B_BUTT_CAP, B_MITER_JOIN, B_DEFAULT_MITER_LIMIT.
    // The numbers are Haiku's (InterfaceDefs.h:279-294): BUTT is 3, MITER 1.
    const DrawState state;
    check(state.line_cap == 3, "the default cap is B_BUTT_CAP (3)");
    check(state.line_join == 1, "the default join is B_MITER_JOIN (1)");
    check(state.miter_limit == 10.0f, "the default miter limit is 10");
}

// -- Font attributes ------------------------------------------------------

constexpr const char* sample_text = "Hamburgefonstiv";
constexpr float sample_size = 18;

struct TextRender {
    Surface surface {260, 260};
    InkBox ink;
    float advance = 0;
};

TextRender render_text(TextEngine& engine, Point baseline, float rotation,
                       float shear, float false_bold_width,
                       bool leave_shear_defaulted = false)
{
    TextRender result;
    result.surface.clear({255, 255, 255, 255});
    DrawState state;
    state.high = {0, 0, 0, 255};
    state.font.size = sample_size;
    state.font.rotation = rotation;
    if (!leave_shear_defaulted)
        state.font.shear = shear;
    state.font.false_bold_width = false_bold_width;
    result.advance = engine.draw(sample_text, baseline, state, result.surface);
    result.ink = measure_ink(result.surface);
    return result;
}

void test_rotation_turns_a_horizontal_label_vertical()
{
    TextEngine engine;
    check(engine.available(), "FreeType is available for the font tests");

    const Point plain_baseline {20, 100};
    const auto plain = render_text(engine, plain_baseline, 0, 90, 0);
    check(plain.ink.count > 0, "an unrotated string inks the surface");
    check(plain.ink.width() > 3 * plain.ink.height(),
          "an unrotated string is wide and short");

    // Baseline picked so a 90-degree (counter-clockwise) string stays on the
    // surface: it marches upwards from its baseline.
    const Point rotated_baseline {100, 170};
    const auto rotated = render_text(engine, rotated_baseline, 90, 90, 0);
    check(rotated.ink.count > 0, "a rotated string inks the surface");
    check(rotated.ink.height() > 3 * rotated.ink.width(),
          "a string rotated 90 degrees is tall and narrow");

    // The shape of the box is not on its own enough: rotating only the
    // *baseline* and leaving each glyph upright produces a tall narrow column
    // too (measured 16x149 against the correct 19x147 -- indistinguishable by
    // aspect ratio). So map the unrotated ink box through the rotation by hand
    // and demand the rotated render land on it.
    //
    // A point at baseline-relative FreeType offset (u, v), v pointing up, lands
    // at screen (bx + u, by - v) unrotated. Rotated 90 degrees
    // counter-clockwise it becomes (bx - v, by - u) -- so a plain *screen*
    // offset (sx, sy) maps to a rotated screen offset (sy, -sx).
    const int plain_low_x = plain.ink.left - static_cast<int>(plain_baseline.x);
    const int plain_high_x = plain.ink.right - static_cast<int>(plain_baseline.x);
    const int plain_low_y = plain.ink.top - static_cast<int>(plain_baseline.y);
    const int plain_high_y =
        plain.ink.bottom - static_cast<int>(plain_baseline.y);
    const int expected_left = static_cast<int>(rotated_baseline.x) + plain_low_y;
    const int expected_right =
        static_cast<int>(rotated_baseline.x) + plain_high_y;
    const int expected_top =
        static_cast<int>(rotated_baseline.y) - plain_high_x;
    const int expected_bottom =
        static_cast<int>(rotated_baseline.y) - plain_low_x;
    // One pixel of slack per edge for the floor() of a fractional pen position.
    constexpr int slack = 2;
    check(std::abs(rotated.ink.left - expected_left) <= slack
              && std::abs(rotated.ink.right - expected_right) <= slack,
          "the rotated ink sits where rotating the glyph outlines puts it (x)");
    check(std::abs(rotated.ink.top - expected_top) <= slack
              && std::abs(rotated.ink.bottom - expected_bottom) <= slack,
          "the rotated ink sits where rotating the glyph outlines puts it (y)");
}

void test_string_width_ignores_rotation()
{
    // ServerFont::StringWidth uses StringWidthConsumer, whose Finish(x, y)
    // keeps a bare x with no embedded transform -- unlike AGGTextRenderer's
    // StringRenderer::Finish, which does transform the reported pen position.
    // The RP_STRING_WIDTH reply must match the server's own fallback, so
    // rotation must not move it.
    TextEngine engine;
    Font upright;
    upright.size = sample_size;
    Font rotated = upright;
    rotated.rotation = 90;
    Font sheared = upright;
    sheared.shear = 60;
    Font emboldened = upright;
    emboldened.false_bold_width = 1.5f;

    const float reference = engine.width(sample_text, upright);
    check(reference > 0, "an upright string has a positive width");
    check(engine.width(sample_text, rotated) == reference,
          "RP_STRING_WIDTH is unchanged by rotation, as on the server");
    check(engine.width(sample_text, sheared) == reference,
          "RP_STRING_WIDTH is unchanged by shear, as on the server");
    check(engine.width(sample_text, emboldened) == reference,
          "false bold changes glyph weight, not the advance");
}

void test_neutral_shear_is_ninety_degrees()
{
    TextEngine engine;
    // Haiku's neutral shear is 90, not 0: app_server shears by (90 - Shear()).
    const auto neutral = render_text(engine, {20, 100}, 0, neutral_font_shear,
                                     0);
    const auto defaulted = render_text(engine, {20, 100}, 0, 0, 0, true);
    check(neutral.ink.count > 0, "a neutrally sheared string inks the surface");
    check(std::equal(neutral.surface.pixels().begin(),
                     neutral.surface.pixels().end(),
                     defaulted.surface.pixels().begin()),
          "shear 90 is pixel-identical to a default-constructed Font");

    // And shear is genuinely read: a slanted one must differ.
    const auto slanted = render_text(engine, {20, 100}, 0, 60, 0);
    check(!std::equal(neutral.surface.pixels().begin(),
                      neutral.surface.pixels().end(),
                      slanted.surface.pixels().begin()),
          "shear 60 renders differently from neutral shear");
    check(slanted.ink.left < neutral.ink.left,
          "a positive slant leans the glyphs leftwards at the top");
}

void test_false_bold_thickens_the_glyphs()
{
    TextEngine engine;
    const auto plain = render_text(engine, {20, 100}, 0, neutral_font_shear, 0);
    const auto bold = render_text(engine, {20, 100}, 0, neutral_font_shear,
                                  1.5f);
    check(plain.ink.count > 0, "the unemboldened string inks the surface");
    check(bold.ink.count > plain.ink.count,
          "false_bold_width > 0 inks more pixels at the same size");
    check(bold.advance == plain.advance,
          "false_bold_width leaves the reported advance alone");
}

// --- RP_DRAW_BITMAP options: tiling and bilinear filtering ------------------

// A deliberately asymmetric 4x4 subject: red rises along x, green along y, so a
// transposed, mirrored, offset or stretched render cannot pass by symmetry.
Color tile_color(int x, int y)
{
    return {static_cast<std::uint8_t>(10 + 20 * x),
            static_cast<std::uint8_t>(40 + 30 * y), 200, 255};
}

template <typename Sampler>
Bitmap make_bitmap(int width, int height, Sampler at)
{
    Bitmap bitmap;
    bitmap.width = width;
    bitmap.height = height;
    bitmap.bgra.resize(static_cast<std::size_t>(width * height * 4));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto color = at(x, y);
            const auto offset = static_cast<std::size_t>((y * width + x) * 4);
            bitmap.bgra[offset + 0] = color.b;
            bitmap.bgra[offset + 1] = color.g;
            bitmap.bgra[offset + 2] = color.r;
            bitmap.bgra[offset + 3] = color.a;
        }
    }
    return bitmap;
}

Color tile_subject_at(int x, int y) { return tile_color(x, y); }

// 2x2 greyscale checker: black on the main diagonal, white off it.
Color checker_at(int x, int y)
{
    const auto value = static_cast<std::uint8_t>((x == y) ? 0 : 255);
    return {value, value, value, 255};
}

Surface draw(const Bitmap& bitmap, Rect source, Rect destination,
             std::uint32_t options, bool subpixel_precise = false)
{
    Surface surface(24, 24);
    surface.clear({255, 255, 255, 255});
    DrawState state;
    state.subpixel_precise = subpixel_precise;
    surface.draw_bitmap(bitmap, source, destination, state, options);
    return surface;
}

void test_tiled_bitmap_repeats_instead_of_stretching()
{
    const auto subject = make_bitmap(4, 4, tile_subject_at);
    const Rect source {0, 0, 3, 3};
    const Rect destination {2, 3, 13, 14}; // 12x12: three tiles each way
    const auto tiled = draw(subject, source, destination, tile_bitmap);
    const auto stretched = draw(subject, source, destination, 0);

    // Both expectations are independent geometric predictions, not values read
    // back from the renderer. A tiled draw does not scale -- app_server pins
    // scaleX/scaleY to 1 and wraps (BitmapPainter.cpp:196-227,
    // DrawBitmapGeneric.h:26-30) -- so destination pixel (i, j) must be source
    // pixel (i % 4, j % 4). A single stretched copy maps it to (i/3, j/3)
    // instead, which is what this client drew while the options word was
    // discarded.
    bool tiles = true;
    bool stretches = true;
    for (int j = 0; j < 12; ++j) {
        for (int i = 0; i < 12; ++i) {
            tiles = tiles
                && tiled.pixel(2 + i, 3 + j) == tile_color(i % 4, j % 4);
            stretches = stretches
                && stretched.pixel(2 + i, 3 + j) == tile_color(i / 3, j / 3);
        }
    }
    check(tiles, "a tiled draw repeats the bitmap at its own size across the rect");
    check(stretches,
          "without the tiling bits the same draw is one stretched copy");

    // The two halves the bug report asks for, kept separate because either one
    // alone passes on the defect. First: the pattern repeats -- three tile
    // origins along the top edge agree with each other and with source (0, 0).
    check(tiled.pixel(2, 3) == tile_color(0, 0)
              && tiled.pixel(6, 3) == tile_color(0, 0)
              && tiled.pixel(10, 3) == tile_color(0, 0),
          "the three tile origins along the top edge are the same pixel");
    // Second: that repetition is not what a stretched copy puts there. Without
    // this, "did anything draw?" would pass on the stretched output.
    check(tiled.pixel(6, 3) != stretched.pixel(6, 3)
              && tiled.pixel(2, 7) != stretched.pixel(2, 7),
          "a mid-rect tile origin differs from the stretched copy's pixel");
}

void test_either_tiling_bit_wraps_both_axes()
{
    // A quirk, matched deliberately: the server's only tiled accessor is
    // agg::image_accessor_wrap<..., wrap_mode_repeat, wrap_mode_repeat>, chosen
    // by `(fOptions & B_TILE_BITMAP) != 0`, so B_TILE_BITMAP_X alone still
    // repeats vertically (BitmapPainter.cpp:166-169, DrawBitmapGeneric.h:26-30).
    const auto subject = make_bitmap(4, 4, tile_subject_at);
    const Rect source {0, 0, 3, 3};
    const Rect destination {2, 3, 13, 14};
    const auto both = draw(subject, source, destination, tile_bitmap);
    const auto x_only = draw(subject, source, destination, tile_bitmap_x);
    const auto y_only = draw(subject, source, destination, tile_bitmap_y);
    const auto neither = draw(subject, source, destination, 0);
    check(std::equal(both.pixels().begin(), both.pixels().end(),
                     x_only.pixels().begin()),
          "B_TILE_BITMAP_X alone tiles in both axes, as on the server");
    check(std::equal(both.pixels().begin(), both.pixels().end(),
                     y_only.pixels().begin()),
          "B_TILE_BITMAP_Y alone tiles in both axes, as on the server");
    // Without this, all three renders being *equally* untiled would pass.
    check(!std::equal(both.pixels().begin(), both.pixels().end(),
                      neither.pixels().begin()),
          "one tiling bit is enough to change the output");
}

// Golden grid: a 2x2 black/white checker magnified to 9x9 with
// B_FILTER_BITMAP_BILINEAR. Transcribed from app_server's own arithmetic --
// DrawBitmapBilinear.h:543-578 (corner-aligned index, weight
// `255 - (uint16)(frac * 255)`), :104-124 (the >> 16 accumulate) and :230-325
// (the exact >> 8 last column, last row and verbatim corner) -- and *not* read
// back from this client.
//
// Two properties of it are worth stating out loud, because they look like bugs
// and are the server's behaviour:
//   * the weights are 255-based but the shift is 16, and 255 * 255 is 65025 not
//     65536, so the interior darkens by ~0.4% -- the half-way blend of 0 and 255
//     lands on 126, not 127;
//   * the last column and last row escape that (they shift by 8), so the top
//     right pixel of a white source edge reads 254.
// The geometry is chosen so every weight is an exact binary fraction (the
// destination is 9 wide, so the index step is 1/8): the golden cannot be a
// float-rounding accident.
constexpr int magnified = 9;
constexpr std::uint8_t bilinear_golden[magnified][magnified] = {
    {   0,  30,  62,  94, 126, 157, 189, 221, 254, },
    {  30,  54,  78, 102, 126, 150, 174, 198, 223, },
    {  62,  78,  94, 110, 126, 142, 158, 174, 191, },
    {  94, 102, 110, 118, 126, 134, 142, 150, 159, },
    { 126, 126, 126, 126, 126, 126, 126, 126, 127, },
    { 157, 150, 142, 134, 126, 118, 110, 103,  95, },
    { 189, 174, 158, 142, 126, 110,  95,  79,  63, },
    { 221, 198, 174, 150, 126, 103,  79,  55,  31, },
    { 254, 223, 191, 159, 127,  95,  63,  31,   0, },
};

void test_bilinear_magnification_interpolates()
{
    const auto checker = make_bitmap(2, 2, checker_at);
    const Rect source {0, 0, 1, 1};
    const Rect destination {0, 0, magnified - 1, magnified - 1};
    const auto smooth = draw(checker, source, destination,
                             filter_bitmap_bilinear);
    const auto sharp = draw(checker, source, destination, 0);

    bool golden = true;
    int smooth_intermediate = 0;
    int sharp_intermediate = 0;
    for (int y = 0; y < magnified; ++y) {
        for (int x = 0; x < magnified; ++x) {
            const auto expected = bilinear_golden[y][x];
            golden = golden
                && smooth.pixel(x, y) == Color {expected, expected, expected,
                                                255};
            const auto drawn = smooth.pixel(x, y).r;
            if (drawn != 0 && drawn != 255)
                ++smooth_intermediate;
            const auto nearest = sharp.pixel(x, y).r;
            if (nearest != 0 && nearest != 255)
                ++sharp_intermediate;
        }
    }
    check(golden,
          "a bilinear magnification matches app_server's own filter weights");
    // The discriminating property: nearest-neighbour can only ever emit the two
    // colours that are in the bitmap, so no amount of drawing produces a value
    // between them. This check cannot pass on a client that drops the filter
    // bit, whatever else it gets right.
    check(sharp_intermediate == 0,
          "nearest-neighbour magnification emits no intermediate values");
    check(smooth_intermediate == 79,
          "bilinear magnification emits intermediate values at the boundaries");
    check(smooth.pixel(4, 0).r == 126 && smooth.pixel(0, 4).r == 126,
          "the half-way pixel of a black-to-white edge is a mid grey");
}

void test_bilinear_tiling_wraps_the_far_edge()
{
    // Tiling and filtering compose, and the composition has a property nothing
    // else here has: at a tile seam the second sample must come from the far
    // edge of the bitmap, not from a clamped repeat of the near one.
    //
    // A half-pixel destination origin is what makes the seam land between two
    // sample points -- in a B_SUBPIXEL_PRECISE view, the only one in which a
    // fractional rect survives _DetermineTransform (BitmapPainter.cpp:191-194).
    // The pixel centre x + 0.5, less the half-pixel origin, less the filter's
    // own half pixel, is source x - 0.5 for every column (DrawBitmapGeneric.h:
    // 52-60, agg_span_image_filter_rgba.h:115-131), so each pixel of columns
    // 1..8 is an even blend of two horizontally adjacent, wrapped, source
    // pixels: 128/256 of black plus 128/256 of white, rounded by the filter's
    // half-unit start, is (128 * 256 * 255 + 32768) >> 16 = 128 -- while a
    // clamping sampler would read the last column twice and give 255 there.
    const auto pair = make_bitmap(2, 2, [](int x, int) {
        const auto value = static_cast<std::uint8_t>(x == 0 ? 0 : 255);
        return Color {value, value, value, 255};
    });
    const Rect source {0, 0, 1, 1};
    const Rect destination {0.5f, 0, 8.5f, 3};
    const auto tiled = draw(pair, source, destination,
                            tile_bitmap | filter_bitmap_bilinear, true);
    bool blended = true;
    for (int y = 0; y <= 3; ++y) {
        for (int x = 1; x <= 8; ++x)
            blended = blended && tiled.pixel(x, y) == Color {128, 128, 128, 255};
    }
    check(blended,
          "a tiled bilinear draw blends across the seam with the wrapped pixel");

    // Without B_SUBPIXEL_PRECISE the same draw is snapped to whole pixels
    // first: roundf(0.5) is 1 (drawing_support.cpp:6-12), so it is exactly the
    // draw at x = 1, which samples pixel centres and does not blur at all.
    const auto snapped = draw(pair, source, destination,
                              tile_bitmap | filter_bitmap_bilinear);
    const auto whole = draw(pair, source, {1, 0, 9, 3},
                            tile_bitmap | filter_bitmap_bilinear);
    check(std::equal(snapped.pixels().begin(), snapped.pixels().end(),
                     whole.pixels().begin(), whole.pixels().end()),
          "outside a B_SUBPIXEL_PRECISE view a half-pixel bitmap rect is rounded");
    check(snapped.pixel(1, 0) == Color {0, 0, 0, 255}
              && snapped.pixel(2, 0) == Color {255, 255, 255, 255},
          "the rounded tiled draw starts its tile grid at x = 1, unblurred");

    // And unit-scale tiling on whole pixels must *not* blur: the sample points
    // land on source pixel centres, so the filter is an identity there.
    const Rect aligned {0, 0, 7, 3};
    // Hold both results in named locals: taking begin() from one draw()
    // temporary and end() from a *second* draw() temporary mixes iterators from
    // two different vectors (UB; the length came out of a cross-allocation
    // pointer difference and only "passed" by heap-layout luck).
    const auto aligned_plain = draw(pair, source, aligned, tile_bitmap);
    const auto aligned_filtered =
        draw(pair, source, aligned, tile_bitmap | filter_bitmap_bilinear);
    check(std::equal(aligned_plain.pixels().begin(), aligned_plain.pixels().end(),
                     aligned_filtered.pixels().begin(),
                     aligned_filtered.pixels().end()),
          "pixel-aligned tiling is unchanged by the filter bit");
}

// -- View transforms (RP_SET_TRANSFORM) -----------------------------------
//
// These are the conformance tests for issue #21: text under a non-identity view
// transform used to be rasterised at the untransformed size and then have its
// *pixels* forward-mapped, which under a scale leaves the gaps between the
// mapped pixels unwritten -- a lattice of dots with roughly the same ink count
// as the unscaled glyph. A count-based assertion is what catches that; "did
// text draw?" does not.
//
// The expected values below are not derived from the transform code. Three
// independent sources are used:
//
//   * an independent parameterisation of the same geometry -- rasterising an
//     outline scaled 2x must ink very nearly what the same outline inks at twice
//     the ppem, which reaches the rasteriser through FT_Set_Pixel_Sizes and no
//     matrix at all. Summed over sub-pixel placements (see
//     sweep_scaled_against_doubled), within 0.4% on every one of 140 faces;
//   * arithmetic done by hand on the mapped baseline and the *identity* ink box;
//   * app_server's own render of the same string at the same size and scale
//     (the "noto 18 ... view scaled" text scenes, tools/painter_oracle),
//     compared pixel for pixel: the lattice -- the forward scatter doubled the
//     box and left the count alone -- cannot match it.

constexpr int transform_surface = 520;

struct TransformRender {
    Surface surface {transform_surface, transform_surface};
    InkBox ink;
};

// The face the two 1% sweep checks draw with: the DeBeOS image's own Noto Sans
// Regular, committed in tests/fonts and named by its identity block, so that
// what they measure is the transform and not whatever the host has installed.
// They used to take the host's trait choice -- Noto Sans VF on Linux, Arial on
// Windows, Helvetica on macOS -- and a 1% bound measured on the first two
// failed on the third (see sweep_scaled_against_doubled for why). Hinting and
// subpixel are DesktopSettings' defaults (DesktopSettings.cpp:88-89), which is
// what an identity-less font gets too.
//
// The other view-transform checks draw by trait, and trait selection here sees
// only tests/fonts (test_fonts::pin() in main), so they draw with this same
// face on every host. Their "filled, not a lattice" claims are the server's
// own pixels for the same string, size and scale (the "noto 18 ... view
// scaled" text scenes), not a density ratio.
constexpr int transform_face = 0;  // text_scenes::faces(): NotoSans-Regular.ttf

std::shared_ptr<const FontIdentity> transform_identity()
{
    static const std::shared_ptr<const FontIdentity> identity = [] {
        add_test_fonts();
        return std::shared_ptr<const FontIdentity>(test_face_identity(transform_face));
    }();
    return identity;
}

// `identity` null draws with the host's trait choice.
Font transform_font(float size, bool mono,
                    std::shared_ptr<const FontIdentity> identity = nullptr)
{
    Font font;
    font.size = size;
    if (mono)
        font.flags = 1;
    font.identity = std::move(identity);
    return font;
}

// `mono` picks B_DISABLE_ANTIALIASING, which routes the glyph through
// Surface::paint_coverage instead of Surface::paint_subpixel_coverage. Both
// carried the forward scatter, so both are exercised.
TransformRender render_with_transform(TextEngine& engine, Point baseline,
                                      Transform transform, float size,
                                      bool mono = false,
                                      bool disable_hinting = false,
                                      std::string_view text = sample_text,
                                      std::shared_ptr<const FontIdentity> identity
                                      = nullptr)
{
    TransformRender result;
    result.surface.clear({255, 255, 255, 255});
    DrawState state;
    state.high = {0, 0, 0, 255};
    state.font = transform_font(size, mono, std::move(identity));
    state.transform = transform;
    engine.draw(text, baseline, state, result.surface, nullptr, disable_hinting);
    result.ink = measure_ink(result.surface);
    return result;
}

const Point transform_baseline {40, 120};
constexpr Transform scale_two {2, 0, 0, 2, 0, 0};

// "An unhinted 2x view scale inks within 1% of the same string at 2x size",
// made a statement about the outlines rather than about where they happen to
// fall on the pixel grid.
//
// Both arms are one unhinted outline at 36 px scale, but not one *set of
// coordinates*: the 2x arm is an 18 px outline rounded to 1/64 px and doubled,
// so every point sits on a 1/32 px grid, while the 36 px arm is rounded to
// 1/64 px. Up to 1/64 px apart, then. A thresholded raster -- monochrome above
// all, where AGG's gamma_threshold(0.5) (AGGTextRenderer.cpp:96-107) keeps a
// pixel only if half of it is covered -- turns that into whole runs: a straight
// stem edge sitting on a pixel centre in one arm and 1/64 px off it in the
// other flips an entire column of the stem. Measured over 140 Linux faces at
// 18 px with this renderer, phase 0 only: 8 beyond 1% monochrome (Lohit
// Devanagari's Latin 4.9%, Thai TlwgMono 2.6%) and 4 beyond it anti-aliased.
// The 1% bound was set from Noto, DejaVu and Arial, which happen to sit well on
// the grid at this size; the macOS host's face did not.
//
// So the comparison is summed over 64 sub-pixel placements of the same
// string, the x offsets taking every 1/64 px residue once and the y offsets too
// (37 is odd, so 37k mod 64 is a permutation). Summed that way, the pixel
// centres a span [a, b] on the 1/64 grid covers add up to exactly 64(b - a) + 1
// wherever the span sits, because the shifted centres enumerate the whole 1/64
// grid once: luck with the grid cancels and what is left is the two outlines'
// geometry. Measured over the same 140 faces with the sweep: at most 0.38%
// monochrome and 0.32% anti-aliased, so the 1% bound -- against a lattice's
// ~75% short -- holds with room on any face, not only on the committed one
// these checks now draw with.
//
// Phase 0 alone -- what the checks used to compare -- is kept for the
// diagnostics, so a failure still shows how far off a single placement was.
constexpr int phase_sweep = 64;

struct SweptInk {
    long scaled = 0;        // 18 px, view-scaled 2x, summed over the sweep
    long doubled = 0;       // 36 px, untransformed, summed over the sweep
    int scaled_once = 0;    // phase 0 alone
    int doubled_once = 0;

    [[nodiscard]] bool within_one_percent() const
    {
        return doubled > 0 && std::abs(scaled - doubled) * 100 <= doubled;
    }
};

SweptInk sweep_scaled_against_doubled(
    TextEngine& engine, bool mono, std::string_view text = sample_text,
    const std::shared_ptr<const FontIdentity>& identity = transform_identity())
{
    SweptInk result;
    for (int k = 0; k < phase_sweep; ++k) {
        // Device-space offsets, exact in float: k/64 and its halves.
        const float dx = static_cast<float>(k) / phase_sweep;
        const float dy = static_cast<float>((37 * k) % phase_sweep) / phase_sweep;
        const int scaled = render_with_transform(
            engine, {transform_baseline.x + dx / 2, transform_baseline.y + dy / 2},
            scale_two, sample_size, mono, true, text, identity).ink.count;
        const int doubled = render_with_transform(
            engine, {transform_baseline.x + dx, transform_baseline.y + dy}, {},
            2 * sample_size, mono, true, text, identity).ink.count;
        result.scaled += scaled;
        result.doubled += doubled;
        if (k == 0) {
            result.scaled_once = scaled;
            result.doubled_once = doubled;
        }
    }
    return result;
}

std::string percent_apart(long a, long b)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << std::showpos
        << (b == 0 ? 0.0
                   : 100.0 * static_cast<double>(a - b) / static_cast<double>(b))
        << '%';
    return out.str();
}

std::string describe_sweep(const SweptInk& ink)
{
    return "swept over " + std::to_string(phase_sweep) + " phases: 2x "
        + std::to_string(ink.scaled) + " vs 2x-size " + std::to_string(ink.doubled)
        + " (" + percent_apart(ink.scaled, ink.doubled) + "); phase 0 alone: "
        + std::to_string(ink.scaled_once) + " vs "
        + std::to_string(ink.doubled_once) + " ("
        + percent_apart(ink.scaled_once, ink.doubled_once) + ")";
}

std::string tag_name(FT_ULong tag)
{
    std::string name;
    for (int shift = 24; shift >= 0; shift -= 8)
        name += static_cast<char>((tag >> shift) & 0xffu);
    return name;
}

// Everything about the face behind a text check that could make it a property
// of the host rather than of the client: which file and face index, what
// FreeType makes of it, whether it is variable and at which instance, which
// outline tables it carries, and whether every sample glyph is in it. Printed
// by --font-report and appended to a failing sweep check, so a failure on a
// host nobody here can reach explains itself.
//
// Only API the vendored FreeType 2.6.3 has, so it builds against either.
std::string face_facts(TextEngine& engine, const Font& font)
{
    std::ostringstream out;
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0)
        return "face: FreeType did not initialise\n";
    FT_Int major = 0;
    FT_Int minor = 0;
    FT_Int patch = 0;
    FT_Library_Version(library, &major, &minor, &patch);
    out << "  FreeType " << major << '.' << minor << '.' << patch
#ifdef HAIKU_REMOTE_SYSTEM_FREETYPE
        << " (the host's, HAIKU_REMOTE_FREETYPE=system)"
#else
        << " (vendored)"
#endif
        << '\n';

    const auto chosen = engine.selected_face(font);
    out << "  face: " << (chosen.path.empty() ? "(none)" : chosen.path)
        << ", face index " << (chosen.index & 0xffff) << ", named instance "
        << (chosen.index >> 16)
        << (font.identity == nullptr ? ", chosen by trait"
                                     : (chosen.identical ? ", the identity's own file"
                                                         : ", NOT the identity's file"))
        << (chosen.exact ? "" : " (a style substitution)") << '\n';
    if (font.identity == nullptr) {
        if (const char* env = std::getenv(font.spacing == fixed_spacing
                                              ? "HAIKU_REMOTE_MONO_FONT"
                                              : "HAIKU_REMOTE_FONT")) {
            out << "  (an environment override is set: " << env << ")\n";
        }
        const auto candidates = engine.candidate_files(font);
        out << "  candidates, best first:";
        for (std::size_t i = 0; i < candidates.size() && i < 6; ++i)
            out << ' ' << candidates[i];
        out << (candidates.size() > 6 ? " ...\n" : "\n");
    }

    FT_Face face = nullptr;
    if (chosen.path.empty()
        || FT_New_Face(library, chosen.path.c_str(), chosen.index, &face) != 0) {
        out << "  FreeType cannot open it independently of the engine\n";
        FT_Done_FreeType(library);
        return out.str();
    }
    out << "  '" << (face->family_name ? face->family_name : "?") << "' '"
        << (face->style_name ? face->style_name : "?") << "', "
        << FT_Get_Font_Format(face) << ", " << face->num_faces
        << " face(s) in the file, " << face->units_per_EM << " units/em"
        << (FT_IS_SCALABLE(face) ? "" : ", NOT scalable")
        << (FT_HAS_FIXED_SIZES(face) ? ", has bitmap strikes" : "") << '\n';

    out << "  tables:";
    for (const char* tag : {"glyf", "CFF ", "CFF2", "hvgl", "gvar", "fvar",
                            "sbix", "EBDT", "bdat", "COLR"}) {
        FT_ULong length = 0;
        if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG(tag[0], tag[1], tag[2], tag[3]),
                               0, nullptr, &length)
            == 0) {
            out << " '" << tag << "' " << length;
        }
    }
    out << '\n';

    FT_MM_Var* master = nullptr;
    if (FT_HAS_MULTIPLE_MASTERS(face) && FT_Get_MM_Var(face, &master) == 0) {
        // FreeType moves no axis by itself -- not even 'opsz' with the ppem --
        // so a face draws at the default or at the named instance it was
        // opened as, at every size. app_server opens a variable face only as a
        // named instance (GlobalFontManager.cpp:726-737).
        const long instance = chosen.index >> 16;
        out << "  variable: " << master->num_axis << " axes, "
            << master->num_namedstyles << " named instances; drawn at "
            << (instance == 0 ? std::string("the default instance")
                              : "named instance " + std::to_string(instance))
            << '\n';
        for (FT_UInt axis = 0; axis < master->num_axis; ++axis) {
            const auto& a = master->axis[axis];
            const FT_Fixed used = instance > 0
                    && static_cast<FT_UInt>(instance) <= master->num_namedstyles
                ? master->namedstyle[instance - 1].coords[axis]
                : a.def;
            out << "    '" << tag_name(a.tag) << "' " << a.minimum / 65536.0
                << ".." << a.maximum / 65536.0 << ", default " << a.def / 65536.0
                << ", drawn at " << used / 65536.0
                << (a.tag == FT_MAKE_TAG('o', 'p', 's', 'z')
                        ? " -- optical size, fixed: both arms draw this instance"
                        : "")
                << '\n';
        }
        std::free(master);
    } else {
        out << "  not variable\n";
    }

    std::string unmapped;
    for (const char code : std::string_view(sample_text)) {
        if (FT_Get_Char_Index(face, static_cast<unsigned char>(code)) == 0)
            unmapped += code;
    }
    out << "  sample glyphs: "
        << (unmapped.empty() ? std::string("all in the face")
                             : "NOT in the face, drawn from a fallback: '" + unmapped
                                 + "'")
        << '\n';
    // What FreeType hands back for 'H' at both sizes: an embedded bitmap is not
    // an outline, cannot be transformed, and would wreck the 2x arm.
    for (const FT_UInt ppem : {static_cast<FT_UInt>(sample_size),
                               static_cast<FT_UInt>(2 * sample_size)}) {
        const char* format = "load failed";
        if (FT_Set_Pixel_Sizes(face, ppem, ppem) == 0
            && FT_Load_Char(face, 'H', FT_LOAD_NO_HINTING) == 0) {
            format = face->glyph->format == FT_GLYPH_FORMAT_OUTLINE
                ? "an outline"
                : "NOT an outline (a bitmap strike?)";
        }
        out << "  'H' at " << ppem << " ppem, unhinted: " << format << '\n';
    }
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return out.str();
}

// One of the two sweep checks, with the facts attached when it fails.
void check_swept(TextEngine& engine, const SweptInk& ink, bool mono,
                 std::string_view what)
{
    const bool ok = ink.within_one_percent();
    std::string message(what);
    if (!ok) {
        message += "\n  " + describe_sweep(ink) + "\n"
            + face_facts(engine,
                         transform_font(sample_size, mono, transform_identity()))
            + "  (build/haiku-remote-render-tests --font-report says more)";
    }
    check(ok, message);
}

void test_a_scaled_view_transform_fills_the_glyphs()
{
    TextEngine engine;
    const auto plain = render_with_transform(engine, transform_baseline, {},
                                             sample_size);
    const auto scaled = render_with_transform(engine, transform_baseline,
                                              scale_two, sample_size);
    check(plain.ink.count > 0, "an untransformed string inks the surface");
    check(scaled.ink.count > 0, "a scaled string inks the surface");

    // The assertion from the issue. A lattice inks about *the same* number of
    // pixels as the unscaled glyph, so the failure mode is count ~= 1x; a filled
    // 2x glyph is ~4x, short of exactly 4 only because the anti-aliased
    // perimeter grows linearly while the interior grows quadratically. On the
    // committed Noto Sans it is 2426 against 793, 3.06x: v35 hinting snaps the
    // identity arm's stems to whole pixels, so more of its perimeter passes the
    // ink threshold. Where the two renders stand exactly is the comparison
    // with app_server's own pixels below; this pair only separates ~1x from
    // ~4x.
    check(scaled.ink.count > 3 * plain.ink.count,
          "{sx=2, sy=2} inks more than 3x the identity count, not ~1x");
    check(scaled.ink.count < 5 * plain.ink.count,
          "{sx=2, sy=2} inks less than 5x the identity count");

    // Independent prediction: same outline, twice the ppem, no matrix involved.
    //
    // Like for like, though. app_server hints a transformed glyph at the
    // font's own ppem and only then transforms the outline (FontEngine::
    // PrepareGlyph, FontEngine.cpp:486-506; AGGTextRenderer.cpp:384-391), and
    // so does this client -- so the hinted 2x arm above is an 18 px hinted
    // outline doubled, which no 36 px render reproduces: how far apart the two
    // land is a property of the face's hinting, not of the transform. Measured:
    // DejaVu Sans 6% apart even with server-correct hinting; Arial (Windows)
    // 22% in monochrome while the client still hinted it bilevel. This check used
    // to compare against the hinted arm and so passed only on lightly hinted
    // faces. Here both arms load one *unhinted* outline (disable_hinting), so
    // they differ only by FreeType's 26.6 rounding of it, which the sweep
    // isolates from the grid (sweep_scaled_against_doubled). The bound is 1%,
    // against a lattice's ~75% short.
    // A name the engine cannot resolve falls back to the trait face without a
    // word to the test, which would put the host's face back under the bound.
    const auto pinned = engine.selected_face(
        transform_font(sample_size, false, transform_identity()));
    check(pinned.identical
              && std::filesystem::path(pinned.path)
                  == test_font_dir()
                      / text_scenes::faces()[transform_face].file,
          "the 2x sweeps draw with the committed Noto Sans (got " + pinned.path
              + ")");
    const auto swept = sweep_scaled_against_doubled(engine, false);
    check(swept.doubled_once > 0, "the double-size reference inks");
    check_swept(engine, swept, false,
                "an unhinted 2x view scale inks within 1% of the unhinted string "
                "at 2x the size");

    // Filled, not a lattice, stated exactly: this string at this size and
    // scale, hinted, drawn by app_server's own AGGTextRenderer (the text scene
    // "noto 18 view scaled 2x", tools/painter_oracle), pixel for pixel. The
    // forward scatter doubled the ink box and left the count alone. This used
    // to be a density ratio, "more than 0.8 of the identity's against a
    // lattice's ~0.25" -- a number measured on the autohinted host faces, and
    // 0.77 on the committed Noto Sans, whose v35 bytecode fills the 18 px
    // identity arm more. No ratio of two hinted renders is a property of the
    // transform; the server's own pixels are.
    check_scene_is_app_servers("noto 18 view scaled 2x",
                               "the scaled glyphs are app_server's, pixel for"
                               " pixel, not a lattice");

    // The ink box doubles in both axes. One pixel of slack per axis for the
    // floor() of a fractional edge, and one more across: the two arms are not
    // laid out alike. The identity string takes the cached-bitmap path, loaded
    // FT_LOAD_TARGET_LCD, the scaled one the outline path, loaded
    // FT_LOAD_TARGET_NORMAL (FontEngine.cpp:488-490), as on the server, and
    // the hinter -- the autohinter, on a face without bytecode -- treats the
    // two targets differently, so the hinted string differs in width by a
    // pixel or two before any transform. Measured on the committed Noto Sans,
    // as on the host's Noto Sans VF before it: 292 against 2 x 147.
    // The lattice this guards against doubles the box exactly; the comparison
    // with app_server's pixels above is what catches it.
    check(std::abs(scaled.ink.width() - 2 * plain.ink.width()) <= 2,
          "a 2x view scale doubles the ink box width (" + std::to_string(scaled.ink.width())
              + " against " + std::to_string(plain.ink.width()) + ")");
    check(std::abs(scaled.ink.height() - 2 * plain.ink.height()) <= 1,
          "a 2x view scale doubles the ink box height");

    // Hand arithmetic, not code: the transform has no translation and
    // DrawState::x_offset/y_offset are zero, so the baseline maps to 2x itself
    // and every ink offset from it doubles too.
    const int mapped_x = 2 * static_cast<int>(transform_baseline.x);
    const int mapped_y = 2 * static_cast<int>(transform_baseline.y);
    const int expected_left = mapped_x
        + 2 * (plain.ink.left - static_cast<int>(transform_baseline.x));
    const int expected_top = mapped_y
        + 2 * (plain.ink.top - static_cast<int>(transform_baseline.y));
    constexpr int slack = 2;
    check(std::abs(scaled.ink.left - expected_left) <= slack,
          "the scaled ink starts where doubling the identity offset puts it (x)");
    check(std::abs(scaled.ink.top - expected_top) <= slack,
          "the scaled ink starts where doubling the identity offset puts it (y)");
}

void test_a_scaled_view_transform_fills_monochrome_glyphs()
{
    // B_DISABLE_ANTIALIASING takes the FT_PIXEL_MODE_MONO branch, whose pixels
    // reach Surface::paint_coverage rather than paint_subpixel_coverage.
    TextEngine engine;
    const auto plain = render_with_transform(engine, transform_baseline, {},
                                             sample_size, true);
    const auto scaled = render_with_transform(engine, transform_baseline,
                                              scale_two, sample_size, true);
    // Like for like, for the reason given in the anti-aliased case above: both
    // arms load the same unhinted outline. That the *hinted* monochrome path is
    // hinted as the server hints it is witnessed glyph by glyph, against
    // FreeType itself, in
    // test_monochrome_glyphs_are_hinted_the_way_app_server_hints_them.
    // Monochrome is where one placement says least -- a pixel is in or out by
    // AGG's 0.5 threshold -- so this is the check the sweep was made for.
    check(plain.ink.count > 0, "an untransformed mono string inks the surface");
    check(scaled.ink.count > 3 * plain.ink.count,
          "a scaled mono string inks more than 3x the identity count");
    check_scene_is_app_servers("noto 18 aliased view scaled 2x",
                               "the scaled mono glyphs are app_server's, pixel"
                               " for pixel, not a lattice");
    check_swept(engine, sweep_scaled_against_doubled(engine, true), true,
                "an unhinted 2x view scale inks within 1% of the same mono "
                "string at 2x size");
}

void test_a_translation_only_transform_leaves_the_raster_alone()
{
    // app_server keeps a translation-only transform on its cached-bitmap fast
    // path and merely offsets the glyph by transform(0, 0)
    // (StringRenderer::NeedsVector, AGGTextRenderer.cpp:146, and the
    // glyph->data_type != glyph_data_outline arm at AGGTextRenderer.cpp:227-236).
    // This client does the same, and this check is also the regression guard for
    // the untransformed path: the raster has to come out byte-identical, just
    // somewhere else.
    TextEngine engine;
    constexpr int dx = 30;
    constexpr int dy = -10;
    const auto plain = render_with_transform(engine, transform_baseline, {},
                                             sample_size);
    const auto moved = render_with_transform(engine, transform_baseline,
                                             {1, 0, 0, 1, dx, dy}, sample_size);
    check(plain.ink.count > 0, "the untranslated string inks the surface");
    check(moved.ink.count == plain.ink.count,
          "a translation-only transform inks exactly as many pixels");
    check(moved.ink.left == plain.ink.left + dx
              && moved.ink.right == plain.ink.right + dx
              && moved.ink.top == plain.ink.top + dy
              && moved.ink.bottom == plain.ink.bottom + dy,
          "a translation-only transform moves the ink box by exactly the offset");

    int differing = 0;
    for (int y = 0; y < transform_surface; ++y) {
        for (int x = 0; x < transform_surface; ++x) {
            const int source_x = x - dx;
            const int source_y = y - dy;
            Color expected {255, 255, 255, 255};
            if (source_x >= 0 && source_x < transform_surface
                && source_y >= 0 && source_y < transform_surface) {
                expected = plain.surface.pixel(source_x, source_y);
            }
            if (!(moved.surface.pixel(x, y) == expected))
                ++differing;
        }
    }
    check(differing == 0,
          "a translated render is pixel-identical to the identity render, shifted");
}

void test_a_sheared_view_transform_leans_the_glyphs_the_right_way()
{
    // Painter installs the wire affine in *screen* space, y down
    // (Painter.cpp:372-383), so a positive shx adds 0.5 * y to x and the part of
    // a glyph above the baseline -- negative y relative to it -- moves left.
    // That is the same direction Haiku's font shear leans
    // (see test_neutral_shear_is_ninety_degrees), and getting the y-flip of the
    // FreeType matrix backwards would reverse it.
    TextEngine engine;
    const auto plain = render_with_transform(engine, transform_baseline, {},
                                             sample_size);
    const auto sheared = render_with_transform(engine, transform_baseline,
                                               {1, 0, 0.5, 1, 0, 0},
                                               sample_size);
    check(plain.ink.left > static_cast<int>(transform_baseline.x),
          "unsheared, the first glyph's ink starts right of the baseline");

    // shx = 0.5 maps the baseline x to x + 0.5 * y: 40 + 60 = 100.
    const int mapped_x = static_cast<int>(transform_baseline.x)
        + static_cast<int>(0.5 * transform_baseline.y);
    check(sheared.ink.left < mapped_x,
          "a positive shx leans the glyph tops left of the mapped baseline");

    // A shear preserves area, so it must neither multiply nor collapse the ink.
    check(std::abs(sheared.ink.count - plain.ink.count)
              <= plain.ink.count / 5,
          "a shear keeps the inked area within 20% of the unsheared string");
    check(!std::equal(plain.surface.pixels().begin(),
                      plain.surface.pixels().end(),
                      sheared.surface.pixels().begin()),
          "a sheared string renders differently from an unsheared one");
}

void test_a_hostile_transform_paints_nothing_and_returns()
{
    // RP_SET_TRANSFORM's six floats are unvalidated wire data. NaN and a scale
    // past FT_Fixed's range would make the conversion to a FreeType matrix
    // undefined, and a merely large scale would have FreeType allocate a glyph
    // bitmap sized by the wire rather than by the surface.
    TextEngine engine;
    const double not_a_number = std::nan("");
    const auto nan_scaled = render_with_transform(
        engine, transform_baseline,
        {not_a_number, 0, 0, not_a_number, 0, 0}, sample_size);
    check(nan_scaled.ink.count == 0, "a NaN scale inks nothing");

    const auto enormous = render_with_transform(
        engine, transform_baseline, {1e30, 0, 0, 1e30, 0, 0}, sample_size);
    check(enormous.ink.count == 0, "a scale of 1e30 inks nothing");

    // A singular affine flattens every outline to zero area, and app_server's
    // AGG rasteriser emits no coverage for one (AGGTextRenderer.cpp:384-391
    // transforms the outline unconditionally). Both raster paths are checked:
    // FreeType's bilevel renderer has dropout control of its own.
    const auto singular = render_with_transform(
        engine, transform_baseline, {2, 0, 0, 0, 0, 0}, sample_size);
    check(singular.ink.count == 0, "a singular transform inks nothing");
    const auto singular_mono = render_with_transform(
        engine, transform_baseline, {2, 0, 0, 0, 0, 0}, sample_size, true);
    check(singular_mono.ink.count == 0,
          "a singular transform inks nothing with antialiasing disabled");

    // And a scale that is large but still plausible must still *draw*,
    // filled: app_server's own pixels for it (the text scene "noto 18 view
    // scaled 10x"), where a lattice would ink one pixel in a hundred. This was
    // the same 0.8 density ratio as the 2x check, and replaced for the same
    // reason.
    const auto large = render_with_transform(engine, {1, 20},
                                             {10, 0, 0, 10, 0, 0}, sample_size);
    check(large.ink.count > 0, "a 10x view scale still draws");
    check_scene_is_app_servers("noto 18 view scaled 10x",
                               "a 10x view scale is app_server's, pixel for"
                               " pixel, filled and not a lattice");
}

void test_an_ill_conditioned_view_transform_is_still_applied()
{
    // A 1/32 horizontal squash is a legitimate BAffineTransform, and app_server
    // applies it to the glyph outline like any other (AGGTextRenderer.cpp:
    // 384-391). It is also exactly the kind of matrix FreeType 2.15's
    // FT_Set_Transform silently replaces with the identity -- it fails
    // FT_Matrix_Check, 32 * |det| = 1 <= 1 + 1/1024 -- which is how this client
    // came to draw a singular transform as an unscaled string on Windows. The
    // expectation is geometry, not a second render: x shrinks 32-fold about
    // the origin, y is untouched.
    TextEngine engine;
    const Point squash_baseline {320, 120};
    const auto plain = render_with_transform(engine, squash_baseline, {},
                                             sample_size);
    const auto squashed = render_with_transform(
        engine, squash_baseline, {1.0 / 32, 0, 0, 1, 0, 0}, sample_size);
    check(squashed.ink.count > 0, "a 1/32 horizontal squash still inks");

    // Hand arithmetic: the baseline maps to (320 / 32, 120) = (10, 120), and
    // each x offset from it shrinks 32-fold. Two pixels of slack per edge: the
    // LCD filter spreads a squashed stem by a pixel either side, and a stem
    // narrower than a pixel may fall under the ink threshold. A vetoed matrix
    // leaves the box ~150 px wide, where this asks for ~5.
    const int base_x = static_cast<int>(squash_baseline.x);
    const int mapped_x = base_x / 32;
    const int expected_left = mapped_x + (plain.ink.left - base_x) / 32;
    const int expected_right = mapped_x + (plain.ink.right - base_x) / 32;
    constexpr int slack = 2;
    check(std::abs(squashed.ink.left - expected_left) <= slack
              && std::abs(squashed.ink.right - expected_right) <= slack,
          "a 1/32 x scale shrinks the string's ink box to 1/32 of its width");

    // y is not scaled, and the outline is hinted in font space before the
    // matrix is applied, so the rows the outline touches at all are exactly the
    // unsqueezed string's rows. Measured as *any* coverage: a 1/32-wide
    // ascender carries too little to pass measure_ink's mid-grey threshold.
    check(touched_rows(squashed.surface) == touched_rows(plain.surface),
          "a 1/32 x scale touches exactly the unsqueezed string's rows");
}

void test_monochrome_glyphs_are_bilevel()
{
    // B_DISABLE_ANTIALIASING sends a font down glyph_ren_outline
    // (FontCacheEntry.cpp:429-435): FontEngine::PrepareGlyph loads it with
    // FT_LOAD_TARGET_NORMAL (FontEngine.cpp:488-490), and AGGTextRenderer
    // rasterises the outline with its own rasteriser under
    // gamma_threshold(0.5) (AGGTextRenderer.cpp:96-107) -- not FreeType's
    // monochrome renderer, which an earlier version of this test held the
    // client to. That the pixels are the server's is pinned by the text
    // goldens ("noto 12 aliased", "charter 12 aliased"); what is checked here,
    // independently of any golden, is the property the threshold gamma
    // guarantees: every cover is 0 or 255, so on white with black text in
    // B_OP_COPY each pixel is one or the other, never a shade between.
    TextEngine engine;
    Font font;
    font.size = sample_size;
    font.flags = 1;
    constexpr int size = 80;
    const Point baseline {20, 50};
    int inked = 0;
    int shaded = 0;
    for (const char code : std::string_view(sample_text)) {
        Surface drawn(size, size);
        drawn.clear({255, 255, 255, 255});
        DrawState state;
        state.high = {0, 0, 0, 255};
        state.font = font;
        engine.draw(std::string_view(&code, 1), baseline, state, drawn);
        bool any = false;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const Color c = drawn.pixel(x, y);
                if (c == Color {0, 0, 0, 255})
                    any = true;
                else if (c != Color {255, 255, 255, 255})
                    ++shaded;
            }
        }
        if (any)
            ++inked;
    }
    check(inked == static_cast<int>(std::string_view(sample_text).size()),
          "every B_DISABLE_ANTIALIASING sample glyph inks");
    check(shaded == 0, "B_DISABLE_ANTIALIASING text is bilevel ("
                           + std::to_string(shaded) + " shaded pixels)");
}

// Subpixel (LCD) text blending, against app_server's drawing_modes/*SUBPIX.h.
//
// Every expected value below was worked by hand from the server's macros, in
// the comment beside it -- not computed by the code under test. The fixture is
// deliberately asymmetric (three different covers, three different channel
// values) so that a swapped cover, a swapped channel or a dropped alpha each
// lands on a different number.
//
//     BLEND_SUBPIX  d' = ((s - d) * a + (d << 8)) >> 8        DrawingMode.h:39-46
//     BLEND16_SUBPIX d' = ((s - d) * a + (d << 16)) >> 16     DrawingMode.h:88-96
//     BLEND_COMPOSITE16_SUBPIX  a /= 255, then BLEND_SUBPIX   DrawingMode.h:182-189
//
// With gSubpixelOrderingRGB (the default, DesktopSettings.cpp:91) subpixelL is
// covers[2] and subpixelR covers[0].
constexpr Color subpix_destination {200, 100, 50, 255};
constexpr Color subpix_source {10, 220, 90, 255};
constexpr std::uint8_t cover_left = 64;
constexpr std::uint8_t cover_middle = 128;
constexpr std::uint8_t cover_right = 192;

std::optional<Color> subpix(DrawingMode mode, Color high, bool ordering_rgb = true,
                            std::uint32_t alpha_function = 0,
                            Color destination = subpix_destination)
{
    DrawState state;
    state.drawing_mode = mode;
    state.high = high;
    state.low = {30, 40, 250, 255};
    state.alpha_function = alpha_function;
    return blend_subpixel_text(destination, state, cover_left, cover_middle,
                               cover_right, ordering_rgb);
}

void test_subpixel_over_blends_each_channel_with_its_own_cover()
{
    // blend_solid_hspan_over_solid_subpix: BLEND_SUBPIX(p, c.r, c.g, c.b,
    // covers[L]=192, covers[M]=128, covers[R]=64) -- d[0] (blue) with 192,
    // d[2] (red) with 64.
    //   red   ((10 - 200) * 64 + 200 * 256) >> 8 = 39040 >> 8 = 152
    //   green ((220 - 100) * 128 + 100 * 256) >> 8 = 40960 >> 8 = 160
    //   blue  ((90 - 50) * 192 + 50 * 256) >> 8 = 20480 >> 8 = 80
    check(subpix(DrawingMode::over, subpix_source) == Color {152, 160, 80, 255},
          "B_OP_OVER subpixel text: red/green/blue take the left/middle/right "
          "covers, >> 8");
    // No colour alpha enters BLEND_OVER_SUBPIX: a half-transparent high colour
    // blends exactly like an opaque one.
    check(subpix(DrawingMode::over, {10, 220, 90, 100}) == Color {152, 160, 80, 255},
          "B_OP_OVER subpixel text ignores the high colour's alpha");
    // blend_solid_hspan_copy_solid_subpix is BLEND_OVER_SUBPIX too.
    check(subpix(DrawingMode::copy, subpix_source) == Color {152, 160, 80, 255},
          "B_OP_COPY subpixel text is the same blend as B_OP_OVER");
    // gSubpixelOrderingRGB false: subpixelL = covers[0], so blue takes 64 and
    // red 192.
    //   red   ((10 - 200) * 192 + 51200) >> 8 = 14720 >> 8 = 57
    //   blue  ((90 - 50) * 64 + 12800) >> 8 = 15360 >> 8 = 60
    check(subpix(DrawingMode::over, subpix_source, false) == Color {57, 160, 60, 255},
          "BGR ordering swaps which cover the red and blue channels take");
}

void test_subpixel_text_takes_no_average_weight()
{
    // gSubpixelAverageWeight (default 120, DesktopSettings.cpp:90) is applied
    // only by the *_avrg_filtering scanlines, compiled out with
    // AVERAGE_BASED_SUBPIXEL_FILTERING (GlobalSubpixelSettings.h:20). Had it
    // been applied, the left cover would become
    //   (64 * 135 + ((64 + 128 + 192 + 2) / 3 = 128) * 120) >> 8 = 93
    // and red ((10 - 200) * 93 + 51200) >> 8 = 130, not 152. Equal covers are
    // the one input the filter cannot move, so this is the case that tells.
    const auto filtered_red = static_cast<std::uint8_t>(
        ((10 - 200) * ((64 * 135 + 128 * 120) >> 8) + 51200) >> 8);
    check(filtered_red == 130, "the average-weight arithmetic itself (fixture)");
    const auto unfiltered = subpix(DrawingMode::over, subpix_source);
    check(unfiltered && unfiltered->r == 152 && unfiltered->r != filtered_red,
          "subpixel covers reach the blend unfiltered, as app_server builds them");
}

void test_subpixel_alpha_modes_swap_red_and_blue_covers()
{
    // blend_solid_hspan_alpha_co_solid_subpix (and _po_solid_):
    //   alphaRed = A * covers[L] = 128 * 192 = 24576
    //   alphaGreen = 128 * 128 = 16384
    //   alphaBlue = A * covers[R] = 128 * 64 = 8192
    // passed as (alphaBlue, alphaGreen, alphaRed) = a1..a3, so d[0] (blue) gets
    // 8192 and d[2] (red) gets 24576 -- the reverse of B_OP_OVER.
    //   red   ((10 - 200) * 24576 + 200 * 65536) >> 16 = 8437760 >> 16 = 128
    //   green ((220 - 100) * 16384 + 100 * 65536) >> 16 = 8519680 >> 16 = 130
    //   blue  ((90 - 50) * 8192 + 50 * 65536) >> 16 = 3604480 >> 16 = 55
    // Unswapped, red would be ((-190) * 8192 + 13107200) >> 16 = 176.
    const Color half {10, 220, 90, 128};
    check(subpix(DrawingMode::alpha, half, true, 0) == Color {128, 130, 55, 255},
          "B_OP_ALPHA overlay subpixel text: BLEND16 with the covers swapped");
    // B_ALPHA_COMPOSITE: a1 = 8192 / 255 = 32, a2 = 16384 / 255 = 64,
    // a3 = 24576 / 255 = 96, then BLEND_SUBPIX:
    //   red   ((10 - 200) * 96 + 51200) >> 8 = 32960 >> 8 = 128
    //   green ((220 - 100) * 64 + 25600) >> 8 = 33280 >> 8 = 130
    //   blue  ((90 - 50) * 32 + 12800) >> 8 = 14080 >> 8 = 55
    check(subpix(DrawingMode::alpha, half, true, 1) == Color {128, 130, 55, 255},
          "B_OP_ALPHA composite subpixel text on an opaque pixel");
}

void test_subpixel_overlay_and_composite_round_differently()
{
    // White on black at full cover and full alpha (A * cover = 65025):
    //   overlay   (255 * 65025) >> 16 = 16581375 >> 16 = 253
    //   composite 65025 / 255 = 255; (255 * 255) >> 8 = 254
    //   over      (255 * 255) >> 8 = 254
    // So the alpha function is visible on an opaque canvas after all, which is
    // why the subpixel path keeps RP_SET_BLENDING_MODE's second word.
    DrawState state;
    state.high = {255, 255, 255, 255};
    const Color black {0, 0, 0, 255};
    state.drawing_mode = DrawingMode::alpha;
    state.alpha_function = 0;
    check(blend_subpixel_text(black, state, 255, 255, 255)
              == Color {253, 253, 253, 255},
          "full-cover B_ALPHA_OVERLAY subpixel text is 253, not 255");
    state.alpha_function = 1;
    check(blend_subpixel_text(black, state, 255, 255, 255)
              == Color {254, 254, 254, 255},
          "full-cover B_ALPHA_COMPOSITE subpixel text is 254");
    state.drawing_mode = DrawingMode::over;
    check(blend_subpixel_text(black, state, 255, 255, 255)
              == Color {254, 254, 254, 255},
          "full-cover B_OP_OVER subpixel text is 254");
    // Any Porter-Duff function: AggCompOpAdapter::blend_solid_hspan_subpix
    // draws nothing (AggCompOpAdapter.h:71-79).
    state.drawing_mode = DrawingMode::alpha;
    state.alpha_function = 2; // B_ALPHA_COMPOSITE_SOURCE_IN
    check(!blend_subpixel_text(black, state, 255, 255, 255).has_value(),
          "a Porter-Duff alpha function leaves subpixel text undrawn");
}

void test_subpixel_other_modes_match_their_spans()
{
    // B_OP_ERASE: the low colour {30, 40, 250}.
    //   red   ((30 - 200) * 64 + 51200) >> 8 = 40320 >> 8 = 157
    //   green ((40 - 100) * 128 + 25600) >> 8 = 17920 >> 8 = 70
    //   blue  ((250 - 50) * 192 + 12800) >> 8 = 51200 >> 8 = 200
    check(subpix(DrawingMode::erase, subpix_source) == Color {157, 70, 200, 255},
          "B_OP_ERASE subpixel text blends the low colour");
    // B_OP_INVERT: source = 255 - d = {55, 155, 205}.
    //   red   ((55 - 200) * 64 + 51200) >> 8 = 41920 >> 8 = 163
    //   green ((155 - 100) * 128 + 25600) >> 8 = 32640 >> 8 = 127
    //   blue  ((205 - 50) * 192 + 12800) >> 8 = 42560 >> 8 = 166
    check(subpix(DrawingMode::invert, subpix_source) == Color {163, 127, 166, 255},
          "B_OP_INVERT subpixel text");
    // B_OP_ADD: min(255, d + s) = {210, 255, 140}.
    //   red   (10 * 64 + 51200) >> 8 = 51840 >> 8 = 202
    //   green (155 * 128 + 25600) >> 8 = 45440 >> 8 = 177
    //   blue  (90 * 192 + 12800) >> 8 = 30080 >> 8 = 117
    check(subpix(DrawingMode::add, subpix_source) == Color {202, 177, 117, 255},
          "B_OP_ADD subpixel text");
    // B_OP_SUBTRACT: max(0, d - s) = {190, 0, 0}.
    //   red   ((190 - 200) * 64 + 51200) >> 8 = 50560 >> 8 = 197
    //   green ((0 - 100) * 128 + 25600) >> 8 = 12800 >> 8 = 50
    //   blue  ((0 - 50) * 192 + 12800) >> 8 = 3200 >> 8 = 12
    check(subpix(DrawingMode::subtract, subpix_source) == Color {197, 50, 12, 255},
          "B_OP_SUBTRACT subpixel text");
    // B_OP_BLEND: (d + s) >> 1 = {105, 160, 70}.
    //   red   ((105 - 200) * 64 + 51200) >> 8 = 45120 >> 8 = 176
    //   green ((160 - 100) * 128 + 25600) >> 8 = 33280 >> 8 = 130
    //   blue  ((70 - 50) * 192 + 12800) >> 8 = 16640 >> 8 = 65
    check(subpix(DrawingMode::blend, subpix_source) == Color {176, 130, 65, 255},
          "B_OP_BLEND subpixel text");
    // B_OP_MIN / B_OP_MAX compare brightness_for (DrawingMode.h:192-200):
    // source (3080 + 132000 + 10440) / 1024 = 145520 / 1024 = 142,
    // destination (61600 + 60000 + 5800) / 1024 = 127400 / 1024 = 124.
    check(!subpix(DrawingMode::min, subpix_source).has_value(),
          "B_OP_MIN leaves a darker destination alone");
    check(subpix(DrawingMode::max, subpix_source) == Color {152, 160, 80, 255},
          "B_OP_MAX blends a brighter source like B_OP_OVER");
    // B_OP_SELECT: a destination equal to the high colour takes the low one,
    // equal to the low colour takes the high one, anything else is untouched.
    check(!subpix(DrawingMode::select, subpix_source).has_value(),
          "B_OP_SELECT leaves a pixel that is neither colour alone");
    //   onto {10, 220, 90} with low {30, 40, 250}:
    //   red   (20 * 64 + 2560) >> 8 = 3840 >> 8 = 15
    //   green ((-180) * 128 + 56320) >> 8 = 33280 >> 8 = 130
    //   blue  (160 * 192 + 23040) >> 8 = 53760 >> 8 = 210
    check(subpix(DrawingMode::select, subpix_source, true, 0, subpix_source)
              == Color {15, 130, 210, 255},
          "B_OP_SELECT turns the high colour into the low one");
}

void test_subpixel_coverage_reaches_the_surface_through_the_server_blend()
{
    Surface surface(4, 1);
    surface.clear(subpix_destination);
    DrawState state;
    state.drawing_mode = DrawingMode::over;
    surface.paint_subpixel_coverage(1, 0, {10, 220, 90, 100}, state, cover_left,
                                    cover_middle, cover_right);
    check(surface.pixel(1, 0) == Color {152, 160, 80, 255},
          "paint_subpixel_coverage writes app_server's B_OP_OVER subpixel blend");
    check(surface.pixel(0, 0) == subpix_destination
              && surface.pixel(2, 0) == subpix_destination,
          "paint_subpixel_coverage touches only its own pixel");
    state.clip_rects = {{2, 0, 3, 0}};
    state.clipping_set = true;
    surface.paint_subpixel_coverage(1, 0, {0, 0, 0, 255}, state, 255, 255, 255);
    check(surface.pixel(1, 0) == Color {152, 160, 80, 255},
          "paint_subpixel_coverage honours the clipping region");
    state.drawing_mode = DrawingMode::alpha;
    state.alpha_function = 3;
    state.clip_rects.clear();
    state.clipping_set = false;
    surface.paint_subpixel_coverage(1, 0, {0, 0, 0, 255}, state, 255, 255, 255);
    check(surface.pixel(1, 0) == Color {152, 160, 80, 255},
          "a Porter-Duff alpha function paints nothing through the surface");
}

// --font-report: the facts behind the two unhinted 2x checks, for a host
// nobody here can run on. First the committed face the checks draw with --
// the verdict the suite reaches, with its evidence; exit 1 if either sweep is
// beyond 1% there. Then, for comparison only, the same sweeps on the face this
// host picks by trait, which is what the checks measured before they were
// pinned: a host face far off at phase 0 but close swept is grid luck, which
// the sweep cancels; one far off swept as well is a real outline difference.
void report_sweeps(TextEngine& engine,
                   const std::shared_ptr<const FontIdentity>& identity,
                   bool& all_within)
{
    for (const bool mono : {false, true}) {
        const auto swept = sweep_scaled_against_doubled(engine, mono, sample_text,
                                                        identity);
        all_within = all_within && swept.within_one_percent();
        std::cout << "  " << (mono ? "monochrome:   " : "anti-aliased: ")
                  << describe_sweep(swept) << " -> "
                  << (swept.within_one_percent() ? "within" : "BEYOND")
                  << " the 1% bound\n";
    }
    std::cout << "  monochrome, glyph by glyph, 2x vs 2x-size:\n";
    for (const char code : std::string_view(sample_text)) {
        const auto swept = sweep_scaled_against_doubled(
            engine, true, std::string_view(&code, 1), identity);
        std::cout << "    '" << code << "' phase 0 " << swept.scaled_once << " vs "
                  << swept.doubled_once << " ("
                  << percent_apart(swept.scaled_once, swept.doubled_once)
                  << "), swept " << percent_apart(swept.scaled, swept.doubled)
                  << '\n';
    }
}

int font_report()
{
    TextEngine engine;
    std::cout << "\"" << sample_text << "\" at " << sample_size
              << " px view-scaled 2x, against " << 2 * sample_size
              << " px untransformed, both unhinted\n\n"
              << "the committed face the checks draw with:\n"
              << face_facts(engine, transform_font(sample_size, true, transform_identity()));
    bool pinned_within = true;
    report_sweeps(engine, transform_identity(), pinned_within);

    // The host's fonts, for this comparison only.
    test_fonts::HostFonts host;
    TextEngine host_engine;
    std::cout << "\nthis host's trait choice (not checked; for comparison):\n"
              << face_facts(host_engine, transform_font(sample_size, true, nullptr));
    bool host_within = true;
    report_sweeps(host_engine, nullptr, host_within);

    std::cout << "\nverdict: the checks "
              << (pinned_within ? "hold" : "FAIL") << " on the committed face; "
              << "the host's face is " << (host_within ? "within" : "BEYOND")
              << " the bound when swept\n";
    return pinned_within ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    // Every check draws with the committed faces alone (tests/test_fonts.hpp).
    test_fonts::pin();
    if (argc > 1 && std::string_view(argv[1]) == "--font-report")
        return font_report();
    {
        const std::string problem = test_fonts::pin_problem();
        check(problem.empty(),
              "text draws with the committed faces, not the host's (" + problem + ")");
    }
    test_shapes_match_app_servers_painter();
    test_text_matches_app_servers_renderer();
    test_subpixel_text_matches_app_servers_spans();
    test_stroke_defaults_are_app_servers();
    test_rotation_turns_a_horizontal_label_vertical();
    test_string_width_ignores_rotation();
    test_neutral_shear_is_ninety_degrees();
    test_false_bold_thickens_the_glyphs();
    test_tiled_bitmap_repeats_instead_of_stretching();
    test_either_tiling_bit_wraps_both_axes();
    test_bilinear_magnification_interpolates();
    test_bilinear_tiling_wraps_the_far_edge();
    test_a_scaled_view_transform_fills_the_glyphs();
    test_a_scaled_view_transform_fills_monochrome_glyphs();
    test_a_translation_only_transform_leaves_the_raster_alone();
    test_a_sheared_view_transform_leans_the_glyphs_the_right_way();
    test_a_hostile_transform_paints_nothing_and_returns();
    test_an_ill_conditioned_view_transform_is_still_applied();
    test_monochrome_glyphs_are_bilevel();
    test_subpixel_over_blends_each_channel_with_its_own_cover();
    test_subpixel_text_takes_no_average_weight();
    test_subpixel_alpha_modes_swap_red_and_blue_covers();
    test_subpixel_overlay_and_composite_round_differently();
    test_subpixel_other_modes_match_their_spans();
    test_subpixel_coverage_reaches_the_surface_through_the_server_blend();
    if (failures == 0) {
        std::cout << "PASS - " << checks << " render checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " render checks failed\n";
    return 1;
}
