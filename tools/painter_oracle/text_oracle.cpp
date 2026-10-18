// The text half of the Painter oracle: every scene in tests/text_scenes.hpp
// through app_server's own string drawing, printed as tests/text_goldens.inc.
//
// What runs is the server's code: Painter::DrawString and Painter::SetFont
// (extracted verbatim by extract.py --text), AGGTextRenderer.cpp,
// FontCacheEntry.cpp, FontEngine.cpp, the GlyphLayoutEngine.h template, the
// Painter-private scanlines and rasterisers, PixelFormat and its drawing modes,
// and AGG -- with the glyphs rasterised by the vendored FreeType 2.6.3,
// configured as the image's freetype_bootstrap package
// (third_party/freetype/README.md). The hand-written parts are the shims in
// text_shim/ (a ServerFont that names its file directly, a FontCache keyed on
// FontCacheEntry::GenerateSignature, no-op locks, no fallback fonts) and this
// driver, which sets the state through Painter's setters in SetDrawState()'s
// order (Painter.cpp:294-349) and makes one DrawString call.
//
// gDefaultHintingMode and gSubpixelAntialiasing are set per scene;
// gSubpixelAverageWeight and gSubpixelOrderingRGB stay at DesktopSettings.cpp's
// 120 and RGB (oracle.cpp).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "oracle_prelude.h"
#include "FontCache.h"
#include "GlobalFontManager.h"

#include "text_scenes.hpp"

#ifndef ORACLE_FONT_DIR
#	error "build.sh defines ORACLE_FONT_DIR, the repository's tests/fonts"
#endif

extern const pattern B_SOLID_HIGH;

GlobalFontManager* gFontManager = NULL;

// Point.cpp
const BPoint B_ORIGIN(0, 0);

// oracle.cpp
std::string encode_rgb(const std::vector<std::uint8_t>& rgb);
std::uint64_t fnv1a_rgb(const std::vector<std::uint8_t>& rgb);

namespace {

using namespace text_scenes;

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

// What FontStyle would say about the file: checked against the scene list, so
// a renamed face fails here instead of in the client's face lookup.
void
check_face(const Face& face, const std::string& path)
{
	FT_Library library;
	FT_Face ftFace;
	if (FT_Init_FreeType(&library) != 0
		|| FT_New_Face(library, path.c_str(), 0, &ftFace) != 0) {
		fprintf(stderr, "text oracle: cannot open %s\n", path.c_str());
		exit(2);
	}
	if (std::string(ftFace->family_name) != face.family
		|| std::string(ftFace->style_name) != face.style
		|| (FT_IS_FIXED_WIDTH(ftFace) != 0) != face.fixed) {
		fprintf(stderr, "text oracle: %s is '%s' '%s'%s, not as text_scenes.hpp"
			" says\n", path.c_str(), ftFace->family_name, ftFace->style_name,
			FT_IS_FIXED_WIDTH(ftFace) ? " (fixed)" : "");
		exit(2);
	}
	FT_Done_Face(ftFace);
	FT_Done_FreeType(library);
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
			background(x, y, rgb);
			std::uint8_t* p = &bgra[(static_cast<size_t>(y) * w + x) * 4];
			p[0] = rgb[2];
			p[1] = rgb[1];
			p[2] = rgb[0];
			p[3] = 255;
		}
	}

	gDefaultHintingMode = scene.hinting;
	gSubpixelAntialiasing = scene.subpixel;
	// Each scene starts from a cold cache, as after a reboot.
	FontCache::Default()->Clear();

	const Face& face = faces()[scene.face];
	const std::string path = std::string(ORACLE_FONT_DIR) + "/" + face.file;
	check_face(face, path);
	ServerFont font;
	font.SetFile(path, 0, face.family, face.style, face.fixed);
	font.SetSize(scene.size);
	font.SetFlags(scene.flags);
	font.SetSpacing(scene.spacing);
	font.SetFace(scene.face_bits);
	font.SetRotation(scene.rotation);
	font.SetShear(scene.shear);
	font.SetFalseBoldWidth(scene.false_bold);

	Painter painter;
	// AttachToBuffer() without the RenderingBuffer wrapper (Painter.cpp:246-268).
	painter.fInternal.fBuffer.attach(bgra.data(), w, h, w * 4);
	painter.fAttached = true;
	painter._SetRendererColor(painter.fPatternHandler.HighColor());
	BRegion clip;
	clip.Include(clipping_rect{0, 0, w - 1, h - 1});
	painter.ConstrainClipping(&clip);

	// SetDrawState() order, Painter.cpp:305-348.
	BAffineTransform transform;
	transform.sx = scene.transform[0];
	transform.shy = scene.transform[1];
	transform.shx = scene.transform[2];
	transform.sy = scene.transform[3];
	transform.tx = scene.transform[4];
	transform.ty = scene.transform[5];
	painter.SetTransform(transform, 0, 0);
	painter.SetPenSize(1);
	painter.fSubpixelPrecise = false;
	painter.SetDrawingMode(static_cast<drawing_mode>(scene.mode));
	painter.SetBlendingMode(static_cast<source_alpha>(scene.source_alpha),
		static_cast<alpha_function>(scene.alpha_function));
	painter.SetPattern(B_SOLID_HIGH);
	painter.SetHighColor(to_color(scene.high));
	painter.SetLowColor(to_color(scene.low));
	painter.SetFont(font);

	const std::string text = scene.text;
	painter.DrawString(text.c_str(), (uint32)text.size(),
		BPoint(scene.x, scene.y), NULL);

	std::vector<std::uint8_t> rgb;
	rgb.reserve(static_cast<size_t>(w) * h * 3);
	for (int i = 0; i < w * h; i++) {
		rgb.push_back(bgra[i * 4 + 2]);
		rgb.push_back(bgra[i * 4 + 1]);
		rgb.push_back(bgra[i * 4 + 0]);
	}
	return rgb;
}

} // namespace

void
print_text_goldens(bool full)
{
	printf("// Generated by tools/painter_oracle (make painter-goldens) from\n"
		"// app_server's own AGGTextRenderer, FontCacheEntry, FontEngine,\n"
		"// Painter and AGG, with glyphs from the vendored FreeType 2.6.3. Do not\n"
		"// edit: regenerate. One entry per scene in tests/text_scenes.hpp, in\n"
		"// order, in the layout of painter_goldens.inc.\n");
	for (const Scene& scene : scenes()) {
		const std::vector<std::uint8_t> rgb = render(scene);
		std::vector<int> changed;
		for (int y = 0; y < scene.height; y++) {
			for (int x = 0; x < scene.width; x++) {
				std::uint8_t bg[3];
				background(x, y, bg);
				const size_t at = (static_cast<size_t>(y) * scene.width + x) * 3;
				if (memcmp(bg, &rgb[at], 3) != 0)
					changed.push_back(y * scene.width + x);
			}
		}
		if (changed.empty()) {
			fprintf(stderr, "text oracle: %s draws nothing\n", scene.name);
			exit(2);
		}
		printf("{\"%s\", %d, %d, 0x%016llxull, %zu, {", scene.name,
			scene.width, scene.height, (unsigned long long)fnv1a_rgb(rgb),
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
			const std::string encoded = encode_rgb(rgb);
			for (size_t i = 0; i < encoded.size(); i += 96)
				printf(" \"%s\"\n", encoded.substr(i, 96).c_str());
		} else {
			printf(" \"\"\n");
		}
		printf("},\n");
	}
	gDefaultHintingMode = HINTING_MODE_ON;
	gSubpixelAntialiasing = true;
}
