// The Painter oracle: renders every scene in tests/painter_scenes.hpp through
// app_server's own DrawingEngine, Painter, PixelFormat, drawing modes and AGG,
// compiled on the host, and prints the result as tests/painter_goldens.inc.
//
// What runs here is the server's code, not a model of it: the AGG headers and
// sources, the Painter-private rasterisers and scanlines, PixelFormat.cpp and
// every drawing_modes/*.h, the bitmap_painter/DrawBitmap*.h blitters,
// PatternHandler.cpp, and the bodies of the Painter, BitmapPainter,
// Transformable and DrawingEngine methods, and ColorConversion.cpp's per-pixel
// ConvertBits -- the latter cut out of the Haiku tree verbatim by extract.py.
// The only hand-written parts are the shim declarations in shim/ (types,
// enums, the BRegion container, ServerBitmap, BBitmap and the ImportBits call
// chain down to ConvertBits for B_RGB32 -> B_RGBA32), and this driver,
// which sets the drawing state through Painter's own setters in the order
// SetDrawState() uses (Painter.cpp:294-349) and makes one DrawingEngine call.
//
// Server settings that are not on the wire are fixed at app_server's defaults,
// DesktopSettings.cpp:88-91: subpixel antialiasing on, RGB subpixel order,
// average weight 120. The view's B_SUBPIXEL_PRECISE flag is set per scene:
// it reaches a client that negotiates RP_CAP_SUBPIXEL_PRECISE.
//
// Build and regenerate with `make painter-goldens`; `make painter-oracle-check`
// regenerates into a temporary file and fails if it differs from the committed
// goldens.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "oracle_prelude.h"
#include "BitmapPainter.h"
#include "ServerBitmap.h"
#include "SystemPalette.h"

#include "painter_scenes.hpp"

const pattern B_SOLID_HIGH = {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
const pattern B_MIXED_COLORS = {{0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55}};
const pattern B_SOLID_LOW = {{0, 0, 0, 0, 0, 0, 0, 0}};

// GraphicsDefs.cpp:30-33
const uint16 B_TRANSPARENT_MAGIC_RGBA15 = 0x39ce;
const uint32 B_TRANSPARENT_MAGIC_RGBA32 = 0x00777477;

// Painter.cpp:111 on anything but x86 (detect_simd, :117-174): no SIMD, so
// the bilinear copy takes BilinearDefault or BilinearLowFilterRatio.
//
// ORACLE_SIMD claims MMX and SSE instead, so a B_RGB32/B_OP_COPY bilinear draw
// takes BilinearSimd, whose x loop is the i386 assembly below, modelled in C.
// BilinearSimd is only compiled under __i386__ (DrawBitmapBilinear.h, the
// kUseSIMDVersion case), so this also needs a tree whose header has that
// #ifdef removed; it is for comparing the two paths, not for goldens.
#ifdef ORACLE_SIMD
uint32 gSIMDFlags = APPSERVER_SIMD_MMX | APPSERVER_SIMD_SSE;

// painter_bilinear_scale.nasm, bilinear_scale_xloop_mmxsse: every byte is
// held as value << 8 and every multiply is PMULHUW, (a * b) >> 16 on 16 bits.
// Like the assembly it reads the pixel to the right and the row below
// unconditionally, so AddressSanitizer sees what the x86 server would read.
extern "C" void
bilinear_scale_xloop_mmxsse(const uint8* src, void* dst, void* xWeights,
	uint32 xmin, uint32 xmax, uint32 wTop, uint32 srcBPR)
{
	const auto mulhi = [](uint32 a, uint32 b) -> uint32 {
		return ((a & 0xffff) * (b & 0xffff)) >> 16;
	};
	const uint16* weights = static_cast<const uint16*>(xWeights);
	uint8* d = static_cast<uint8*>(dst);
	const uint32 top = (wTop << 8) & 0xffff;
	const uint32 bottom = ((255 << 8) - top) & 0xffff;
	if (xmin > xmax)
		return;
	for (uint32 x = xmin; x <= xmax; x++) {
		const uint32 index = weights[x * 2];
		const uint32 left = (uint32)weights[x * 2 + 1] << 8 & 0xffff;
		const uint32 right = ((255 << 8) - left) & 0xffff;
		const uint8* lt = src + index;
		const uint8* lb = src + index + srcBPR;
		uint8 out[4];
		for (int c = 0; c < 4; c++) {
			const uint32 t = (mulhi(lt[c] << 8, left)
				+ mulhi(lt[c + 4] << 8, right)) & 0xffff;
			const uint32 b = (mulhi(lb[c] << 8, left)
				+ mulhi(lb[c + 4] << 8, right)) & 0xffff;
			const uint32 sum = (mulhi(t, top) + mulhi(b, bottom)) & 0xffff;
			const uint32 v = mulhi(sum, 129 << 8) >> 7;
			out[c] = v > 255 ? 255 : v;	// PACKUSWB
		}
		out[3] = 0xff;	// POR 0xff000000
		memcpy(d, out, 4);
		d += 4;
	}
}
#else
uint32 gSIMDFlags = 0;
#endif

// DrawBitmapNoScale::Draw fetches the palette whatever the colour space
// (DrawBitmapNoScale.h:56), but only the B_CMAP8 blenders read it, and no
// scene draws a B_CMAP8 bitmap.
const rgb_color*
SystemPalette()
{
	static rgb_color palette[256];
	return palette;
}

bool gSubpixelAntialiasing = true;
uint8 gDefaultHintingMode = 0;
uint8 gSubpixelAverageWeight = 120;
bool gSubpixelOrderingRGB = true;

namespace {

using namespace painter_scenes;

rgb_color
to_color(const std::uint8_t c[4])
{
	rgb_color color;
	color.red = c[0];
	color.green = c[1];
	color.blue = c[2];
	color.alpha = c[3];
	return color;
}

BRect
bounds_of(const std::vector<BPoint>& points)
{
	BRect bounds(points[0], points[0]);
	for (const BPoint& p : points) {
		bounds.left = min_c(bounds.left, p.x);
		bounds.top = min_c(bounds.top, p.y);
		bounds.right = max_c(bounds.right, p.x);
		bounds.bottom = max_c(bounds.bottom, p.y);
	}
	return bounds;
}

// The scene's BGradient: the subclass for its type, with the stops in the
// order given, as RemoteMessage::ReadGradient leaves one.
std::unique_ptr<BGradient>
make_gradient(const Scene& scene)
{
	std::unique_ptr<BGradient> gradient;
	const BPoint center(scene.gradient_center.x, scene.gradient_center.y);
	switch (scene.gradient_kind) {
		case gradient_linear:
			gradient.reset(new BGradientLinear(
				BPoint(scene.gradient_start.x, scene.gradient_start.y),
				BPoint(scene.gradient_end.x, scene.gradient_end.y)));
			break;
		case gradient_radial:
			gradient.reset(new BGradientRadial(center, scene.gradient_radius));
			break;
		case gradient_radial_focus:
			gradient.reset(new BGradientRadialFocus(center,
				scene.gradient_radius,
				BPoint(scene.gradient_focal.x, scene.gradient_focal.y)));
			break;
		case gradient_diamond:
			gradient.reset(new BGradientDiamond(center));
			break;
		case gradient_conic:
			gradient.reset(new BGradientConic(center, scene.gradient_angle));
			break;
		default:
			fprintf(stderr, "painter oracle: %s: unknown gradient kind\n",
				scene.name);
			exit(2);
	}
	if (scene.gradient_stops.empty()) {
		gradient->AddColor(to_color(scene.gradient_color), 0);
		gradient->AddColor(to_color(scene.gradient_color), 255);
	} else {
		for (const GradientStop& stop : scene.gradient_stops)
			gradient->AddColor(to_color(stop.color), stop.offset);
	}
	return gradient;
}

std::vector<std::uint8_t>
render(const Scene& scene)
{
	const int w = scene.width;
	const int h = scene.height;
	std::vector<std::uint8_t> bgra(static_cast<size_t>(w) * h * 4);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			std::uint8_t rgb[3];
			background(scene, x, y, rgb);
			std::uint8_t* p = &bgra[(static_cast<size_t>(y) * w + x) * 4];
			p[0] = rgb[2];
			p[1] = rgb[1];
			p[2] = rgb[0];
			p[3] = 255;
		}
	}

	Painter painter;
	// AttachToBuffer() without the RenderingBuffer wrapper (Painter.cpp:246-268).
	painter.fInternal.fBuffer.attach(bgra.data(), w, h, w * 4);
	painter.fAttached = true;
	painter._SetRendererColor(painter.fPatternHandler.HighColor());

	BRegion clip;
	if (scene.clip.empty()) {
		clip.Include(clipping_rect{0, 0, w - 1, h - 1});
	} else {
		for (const IRect& r : scene.clip)
			clip.Include(clipping_rect{r.left, r.top, r.right, r.bottom});
	}
	painter.ConstrainClipping(&clip);

	// SetDrawState() order, Painter.cpp:305-348.
	BAffineTransform transform;
	transform.sx = scene.transform[0];
	transform.shy = scene.transform[1];
	transform.shx = scene.transform[2];
	transform.sy = scene.transform[3];
	transform.tx = scene.transform[4];
	transform.ty = scene.transform[5];
	painter.SetTransform(transform, scene.x_offset, scene.y_offset);
	painter.SetPenSize(scene.pen);
	// SetDrawState reads it straight off the DrawState (Painter.cpp:311).
	painter.fSubpixelPrecise = scene.subpixel_precise;
	painter.SetDrawingMode(static_cast<drawing_mode>(scene.mode));
	painter.SetBlendingMode(static_cast<source_alpha>(scene.source_alpha),
		static_cast<alpha_function>(scene.alpha_function));
	pattern p;
	memcpy(p.data, scene.pattern, 8);
	painter.SetPattern(p);
	painter.fPatternHandler.SetOffsets(scene.x_offset, scene.y_offset);
	painter.SetStrokeMode(static_cast<cap_mode>(scene.cap),
		static_cast<join_mode>(scene.join), scene.miter);
	painter.SetFillRule(B_NONZERO);
	painter.SetHighColor(to_color(scene.high));
	painter.SetLowColor(to_color(scene.low));

	DrawingEngine engine(&painter);

	const std::unique_ptr<BGradient> gradientObject = make_gradient(scene);
	const BGradient& gradient = *gradientObject;

	BRect rect(scene.rect[0], scene.rect[1], scene.rect[2], scene.rect[3]);
	std::vector<BPoint> points;
	for (const P& point : scene.points)
		points.push_back(BPoint(point.x, point.y));
	const rgb_color color = to_color(scene.color);
	const bool g = scene.gradient;

	switch (scene.op) {
		case Op::fill_rect:
			if (g) engine.FillRect(rect, gradient); else engine.FillRect(rect);
			break;
		case Op::fill_rect_color:
			engine.FillRect(rect, color);
			break;
		case Op::stroke_rect:
			if (g) engine.StrokeRect(rect, gradient); else engine.StrokeRect(rect);
			break;
		case Op::stroke_rect_color:
			engine.StrokeRect(rect, color);
			break;
		case Op::stroke_line:
			if (g)
				engine.StrokeLine(points[0], points[1], gradient);
			else
				engine.StrokeLine(points[0], points[1]);
			break;
		case Op::stroke_line_color:
			engine.StrokeLine(points[0], points[1], color);
			break;
		case Op::stroke_point_color:
			engine.StrokePoint(points[0], color);
			break;
		case Op::stroke_line_array: {
			std::vector<ViewLineArrayInfo> lines;
			for (const ArrayLine& line : scene.lines) {
				ViewLineArrayInfo info;
				info.startPoint = BPoint(line.from.x, line.from.y);
				info.endPoint = BPoint(line.to.x, line.to.y);
				info.color = to_color(line.color);
				lines.push_back(info);
			}
			engine.StrokeLineArray((int32)lines.size(), lines.data());
			break;
		}
		case Op::fill_ellipse:
		case Op::stroke_ellipse: {
			const bool filled = scene.op == Op::fill_ellipse;
			if (g) engine.DrawEllipse(rect, filled, gradient);
			else engine.DrawEllipse(rect, filled);
			break;
		}
		case Op::fill_arc:
		case Op::stroke_arc: {
			const bool filled = scene.op == Op::fill_arc;
			if (g) engine.DrawArc(rect, scene.a, scene.b, filled, gradient);
			else engine.DrawArc(rect, scene.a, scene.b, filled);
			break;
		}
		case Op::fill_round_rect:
		case Op::stroke_round_rect: {
			const bool filled = scene.op == Op::fill_round_rect;
			if (g) engine.DrawRoundRect(rect, scene.a, scene.b, filled, gradient);
			else engine.DrawRoundRect(rect, scene.a, scene.b, filled);
			break;
		}
		case Op::fill_polygon:
		case Op::stroke_polygon: {
			const bool filled = scene.op == Op::fill_polygon;
			const BRect bounds = bounds_of(points);
			if (g) {
				engine.DrawPolygon(points.data(), (int32)points.size(), bounds,
					filled, scene.closed, gradient);
			} else {
				engine.DrawPolygon(points.data(), (int32)points.size(), bounds,
					filled, scene.closed);
			}
			break;
		}
		case Op::fill_bezier:
		case Op::stroke_bezier: {
			const bool filled = scene.op == Op::fill_bezier;
			if (g) engine.DrawBezier(points.data(), filled, gradient);
			else engine.DrawBezier(points.data(), filled);
			break;
		}
		case Op::fill_triangle:
		case Op::stroke_triangle: {
			const bool filled = scene.op == Op::fill_triangle;
			if (g) engine.DrawTriangle(points.data(), rect, filled, gradient);
			else engine.DrawTriangle(points.data(), rect, filled);
			break;
		}
		case Op::fill_shape:
		case Op::stroke_shape: {
			const bool filled = scene.op == Op::fill_shape;
			const BRect bounds = bounds_of(points);
			const BPoint offset(scene.shape_offset.x, scene.shape_offset.y);
			if (g) {
				engine.DrawShape(bounds, (int32)scene.shape_ops.size(),
					scene.shape_ops.data(), (int32)points.size(), points.data(),
					filled, gradient, offset, scene.shape_scale);
			} else {
				engine.DrawShape(bounds, (int32)scene.shape_ops.size(),
					scene.shape_ops.data(), (int32)points.size(), points.data(),
					filled, offset, scene.shape_scale);
			}
			break;
		}
		case Op::fill_region: {
			BRegion region;
			for (const IRect& r : scene.region)
				region.Include(clipping_rect{r.left, r.top, r.right, r.bottom});
			if (g) engine.FillRegion(region, gradient); else engine.FillRegion(region);
			break;
		}
		case Op::draw_bitmap: {
			// B_RGBA32 and B_RGB32 are BGRA in memory, rows packed (for
			// B_RGB32 the fourth byte is the "unused" one, which
			// bitmap_pixel() sets like any other). The buffer is exactly the
			// bitmap, with nothing after it: since Haiku-Graviton #812 no
			// blitter reads past the source, so a build under
			// AddressSanitizer (ORACLE_SANITIZE=address, build.sh) fails on
			// any read that does. ORACLE_BITMAP_PAD_ROWS appends zeroed rows,
			// only for rendering a tree from before that fix, whose bilinear
			// loops read past the end (DrawBitmapBilinear.h, #812).
#ifndef ORACLE_BITMAP_PAD_ROWS
#define ORACLE_BITMAP_PAD_ROWS 0
#endif
			const size_t rowBytes = static_cast<size_t>(scene.bitmap_width) * 4;
			std::vector<uint8> bits(rowBytes
				* (scene.bitmap_height + ORACLE_BITMAP_PAD_ROWS));
			for (int y = 0; y < scene.bitmap_height; y++) {
				for (int x = 0; x < scene.bitmap_width; x++) {
					std::uint8_t rgba[4];
					bitmap_pixel(x, y, rgba, scene.bitmap_color_space);
					uint8* p = &bits[(static_cast<size_t>(y)
						* scene.bitmap_width + x) * 4];
					p[0] = rgba[2];
					p[1] = rgba[1];
					p[2] = rgba[0];
					p[3] = rgba[3];
				}
			}
			ServerBitmap bitmap(bits.data(), scene.bitmap_width,
				scene.bitmap_height, scene.bitmap_width * 4,
				static_cast<color_space>(scene.bitmap_color_space));
			engine.DrawBitmap(&bitmap, BRect(scene.source[0], scene.source[1],
					scene.source[2], scene.source[3]),
				rect, scene.bitmap_options);
			break;
		}
	}

	std::vector<std::uint8_t> rgb;
	rgb.reserve(static_cast<size_t>(w) * h * 3);
	for (int i = 0; i < w * h; i++) {
		rgb.push_back(bgra[i * 4 + 2]);
		rgb.push_back(bgra[i * 4 + 1]);
		rgb.push_back(bgra[i * 4 + 0]);
	}
	return rgb;
}

// Run-length text: "N*rrggbb" per run (the "N*" omitted for a run of one),
// comma separated. Decoded by tests/render_tests.cpp.
std::string
encode(const std::vector<std::uint8_t>& rgb)
{
	std::string out;
	const size_t pixels = rgb.size() / 3;
	size_t i = 0;
	char buffer[32];
	while (i < pixels) {
		size_t run = 1;
		while (i + run < pixels
			&& memcmp(&rgb[i * 3], &rgb[(i + run) * 3], 3) == 0) {
			run++;
		}
		if (!out.empty())
			out += ',';
		if (run > 1) {
			snprintf(buffer, sizeof(buffer), "%zu*", run);
			out += buffer;
		}
		snprintf(buffer, sizeof(buffer), "%02x%02x%02x", rgb[i * 3],
			rgb[i * 3 + 1], rgb[i * 3 + 2]);
		out += buffer;
		i += run;
	}
	return out;
}

// FNV-1a, 64 bit, over the RGB bytes in row order. Duplicated in
// tests/render_tests.cpp; the constants are the published FNV ones.
std::uint64_t
fnv1a(const std::vector<std::uint8_t>& rgb)
{
	std::uint64_t hash = 14695981039346656037ull;
	for (std::uint8_t byte : rgb) {
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	return hash;
}

} // namespace

// For text_oracle.cpp, which prints its goldens in the same layout.
std::string
encode_rgb(const std::vector<std::uint8_t>& rgb)
{
	return encode(rgb);
}

std::uint64_t
fnv1a_rgb(const std::vector<std::uint8_t>& rgb)
{
	return fnv1a(rgb);
}

// text_blend.cpp
void print_text_blend_goldens();
#ifdef ORACLE_TEXT
// text_oracle.cpp
void print_text_goldens(bool full);
#endif

int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--text-blend") == 0) {
		print_text_blend_goldens();
		return 0;
	}
#ifdef ORACLE_TEXT
	if (argc > 1 && (strcmp(argv[1], "--text") == 0
			|| strcmp(argv[1], "--text-full") == 0)) {
		print_text_goldens(strcmp(argv[1], "--text-full") == 0);
		return 0;
	}
#endif
	const bool full = argc > 1 && strcmp(argv[1], "--full") == 0;
	printf("// Generated by tools/painter_oracle (make painter-goldens) from\n"
		"// app_server's own Painter, DrawingEngine and AGG sources. Do not edit:\n"
		"// regenerate. One entry per scene in tests/painter_scenes.hpp, in order:\n"
		"// name, width, height, FNV-1a-64 of the RGB bytes, the number of pixels\n"
		"// that differ from the background, twelve of those pixels as\n"
		"// {x, y, 0xRRGGBB} spread evenly through them, and -- only when built\n"
		"// with --full, for diagnosing a mismatch -- every pixel, run-length\n"
		"// encoded.\n");
	// ORACLE_TRACE names each scene on stderr before it renders, so a
	// sanitizer report can be tied to the scene that triggered it.
	const bool trace = getenv("ORACLE_TRACE") != NULL;
	for (const Scene& scene : scenes()) {
		if (trace)
			fprintf(stderr, "scene %s\n", scene.name);
		const std::vector<std::uint8_t> rgb = render(scene);
		std::vector<int> changed;
		for (int y = 0; y < scene.height; y++) {
			for (int x = 0; x < scene.width; x++) {
				std::uint8_t bg[3];
				background(scene, x, y, bg);
				const size_t at = (static_cast<size_t>(y) * scene.width + x) * 3;
				if (memcmp(bg, &rgb[at], 3) != 0)
					changed.push_back(y * scene.width + x);
			}
		}
		printf("{\"%s\", %d, %d, 0x%016llxull, %zu, {", scene.name,
			scene.width, scene.height, (unsigned long long)fnv1a(rgb),
			changed.size());
		const size_t samples = changed.size() < 12 ? changed.size() : 12;
		for (size_t i = 0; i < samples; i++) {
			const int index = changed[i * changed.size() / samples];
			const size_t at = static_cast<size_t>(index) * 3;
			printf("%s{%d, %d, 0x%02x%02x%02x}", i ? ", " : "",
				index % scene.width, index / scene.width, rgb[at], rgb[at + 1],
				rgb[at + 2]);
		}
		printf("},\n");
		if (full) {
			const std::string text = encode(rgb);
			for (size_t i = 0; i < text.size(); i += 96)
				printf(" \"%s\"\n", text.substr(i, 96).c_str());
		} else {
			printf(" \"\"\n");
		}
		printf("},\n");
	}
	return 0;
}
