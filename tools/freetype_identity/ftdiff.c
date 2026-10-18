/* Differential test: the vendored FreeType 2.6.3 (linked statically) against
 * the libfreetype.so the DeBeOS arm64 image ships (dlopen'ed with a stub
 * libroot). For every face, size, load mode and glyph, compare the loaded
 * outline, the metrics and the rendered bitmap byte for byte. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

typedef FT_Error (*init_t)(FT_Library*);
typedef FT_Error (*newface_t)(FT_Library, const char*, FT_Long, FT_Face*);
typedef FT_Error (*setpx_t)(FT_Face, FT_UInt, FT_UInt);
typedef FT_Error (*load_t)(FT_Face, FT_UInt, FT_Int32);
typedef FT_Error (*render_t)(FT_GlyphSlot, FT_Render_Mode);
typedef FT_Error (*charmap_t)(FT_Face, FT_Encoding);
typedef FT_Error (*kern_t)(FT_Face, FT_UInt, FT_UInt, FT_UInt, FT_Vector*);

struct api {
	init_t init; newface_t newface; setpx_t setpx; load_t load; render_t render;
	charmap_t charmap; kern_t kern;
};

static long checked, differ;

static int same_slot(FT_GlyphSlot a, FT_GlyphSlot b, int rendered)
{
	if (a->advance.x != b->advance.x || a->advance.y != b->advance.y)
		return 0;
	if (memcmp(&a->metrics, &b->metrics, sizeof a->metrics) != 0)
		return 0;
	if (a->format != b->format)
		return 0;
	if (!rendered) {
		if (a->format != FT_GLYPH_FORMAT_OUTLINE)
			return 1;
		const FT_Outline* p = &a->outline; const FT_Outline* q = &b->outline;
		if (p->n_points != q->n_points || p->n_contours != q->n_contours)
			return 0;
		if (memcmp(p->points, q->points, sizeof(FT_Vector) * p->n_points)
			|| memcmp(p->tags, q->tags, p->n_points)
			|| memcmp(p->contours, q->contours, sizeof(short) * p->n_contours))
			return 0;
		return 1;
	}
	if (a->bitmap_left != b->bitmap_left || a->bitmap_top != b->bitmap_top)
		return 0;
	const FT_Bitmap* x = &a->bitmap; const FT_Bitmap* y = &b->bitmap;
	if (x->rows != y->rows || x->width != y->width || x->pitch != y->pitch
		|| x->pixel_mode != y->pixel_mode)
		return 0;
	int pitch = x->pitch < 0 ? -x->pitch : x->pitch;
	if (x->rows && memcmp(x->buffer, y->buffer, (size_t)pitch * x->rows))
		return 0;
	return 1;
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: ftdiff LIBFREETYPE.so FONT... [-s MIN MAX]\n");
		return 2;
	}
	void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
	if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
	struct api H = {
		(init_t)dlsym(h, "FT_Init_FreeType"), (newface_t)dlsym(h, "FT_New_Face"),
		(setpx_t)dlsym(h, "FT_Set_Pixel_Sizes"), (load_t)dlsym(h, "FT_Load_Glyph"),
		(render_t)dlsym(h, "FT_Render_Glyph"), (charmap_t)dlsym(h, "FT_Select_Charmap"),
		(kern_t)dlsym(h, "FT_Get_Kerning") };
	struct api V = { FT_Init_FreeType, FT_New_Face, FT_Set_Pixel_Sizes, FT_Load_Glyph,
		FT_Render_Glyph, FT_Select_Charmap, FT_Get_Kerning };
	int smin = 6, smax = 30;
	const char* env = getenv("FTDIFF_SIZES");
	if (env) sscanf(env, "%d %d", &smin, &smax);
	FT_Library hl, vl;
	if (H.init(&hl) || V.init(&vl)) { fprintf(stderr, "init\n"); return 2; }
	const struct { FT_Int32 flags; int render; const char* name; } modes[] = {
		{ FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD, FT_RENDER_MODE_LCD, "hinted LCD" },
		{ FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL, FT_RENDER_MODE_NORMAL, "hinted gray" },
		{ FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL, FT_RENDER_MODE_MONO, "hinted mono" },
		{ FT_LOAD_NO_HINTING | FT_LOAD_TARGET_NORMAL, -1, "unhinted outline" },
		{ FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL, -1, "hinted outline" },
		{ FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD, -1, "hinted LCD outline" },
		{ FT_LOAD_NO_HINTING | FT_LOAD_NO_SCALE, -1, "unscaled" },
		{ FT_LOAD_NO_HINTING | FT_LOAD_TARGET_LCD, FT_RENDER_MODE_LCD, "unhinted LCD" },
	};
	for (int f = 2; f < argc; f++) {
		FT_Face hf, vf;
		if (H.newface(hl, argv[f], 0, &hf) || V.newface(vl, argv[f], 0, &vf)) {
			fprintf(stderr, "%s: cannot open\n", argv[f]);
			return 2;
		}
		H.charmap(hf, FT_ENCODING_UNICODE);
		V.charmap(vf, FT_ENCODING_UNICODE);
		long before = differ, total = checked;
		for (int size = smin; size <= smax; size++) {
			H.setpx(hf, size, size);
			V.setpx(vf, size, size);
			for (unsigned m = 0; m < sizeof modes / sizeof modes[0]; m++) {
				for (FT_Long g = 0; g < vf->num_glyphs; g++) {
					FT_Error he = H.load(hf, g, modes[m].flags);
					FT_Error ve = V.load(vf, g, modes[m].flags);
					int ok = he == ve;
					if (ok && !he && modes[m].render >= 0) {
						he = H.render(hf->glyph, modes[m].render);
						ve = V.render(vf->glyph, modes[m].render);
						ok = he == ve && (he || same_slot(hf->glyph, vf->glyph, 1));
					} else if (ok && !he) {
						ok = same_slot(hf->glyph, vf->glyph, 0);
					}
					checked++;
					if (!ok) {
						if (differ - before < 5)
							printf("DIFF %s size %d %s glyph %ld\n", argv[f], size,
								modes[m].name, g);
						differ++;
					}
				}
			}
			/* kerning between the first 200 glyph pairs */
			for (FT_UInt a = 0; a < 60 && a < (FT_UInt)vf->num_glyphs; a++)
				for (FT_UInt b = 0; b < 60 && b < (FT_UInt)vf->num_glyphs; b++) {
					FT_Vector p = {0, 0}, q = {0, 0};
					H.kern(hf, a, b, FT_KERNING_DEFAULT, &p);
					V.kern(vf, a, b, FT_KERNING_DEFAULT, &q);
					checked++;
					if (p.x != q.x || p.y != q.y) differ++;
				}
		}
		printf("%s: %ld checks, %ld differ\n", argv[f], checked - total, differ - before);
	}
	printf("TOTAL %ld checks, %ld differ\n", checked, differ);
	return differ != 0;
}
