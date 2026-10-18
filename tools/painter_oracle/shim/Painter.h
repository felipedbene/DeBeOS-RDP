// Host shim for src/servers/app/drawing/Painter/Painter.h: the class
// declaration with the text and font members removed (the oracle never draws
// text), and everything public so the oracle can set state.
// Every member *function body* is app_server's own, extracted verbatim from
// Painter.cpp and Painter.h at build time by extract.py; only this declaration
// and the constructor below are written by hand.
#ifndef ORACLE_SHIM_PAINTER_H
#define ORACLE_SHIM_PAINTER_H

#include <GraphicsDefs.h>
#include <Gradient.h>
#include <Point.h>
#include <Region.h>

#include "PainterAggInterface.h"
#include "PatternHandler.h"
#include "Transformable.h"
#include "defines.h"

// Painter.h:47-48
#define APPSERVER_SIMD_MMX	(1 << 0)
#define APPSERVER_SIMD_SSE	(1 << 1)

class BBitmap;
class ServerBitmap;

#include <agg_conv_curve.h>

#ifdef ORACLE_TEXT
// The text oracle (text_oracle.cpp) builds every translation unit with
// ORACLE_TEXT, so the class below has the text renderer in all of them.
#include "AGGTextRenderer.h"
#endif

class BAffineTransform {
public:
	double sx = 1, shy = 0, shx = 0, sy = 1, tx = 0, ty = 0;
	bool IsIdentity() const
	{
		return sx == 1.0 && shy == 0.0 && shx == 0.0 && sy == 1.0
			&& tx == 0.0 && ty == 0.0;
	}
};

class Painter {
public:
	// Painter::Painter(), Painter.cpp:205-232, minus fTextRenderer.
	Painter()
		:
		fSubpixelPrecise(false),
		fValidClipping(false),
		fAttached(false),
		fPenSize(1.0),
		fClippingRegion(NULL),
		fDrawingMode(B_OP_COPY),
		fAlphaSrcMode(B_PIXEL_ALPHA),
		fAlphaFncMode(B_ALPHA_OVERLAY),
		fLineCapMode(B_BUTT_CAP),
		fLineJoinMode(B_MITER_JOIN),
		fMiterLimit(B_DEFAULT_MITER_LIMIT),
		fPatternHandler(),
#ifdef ORACLE_TEXT
		// Painter.cpp:221-223
		fTextRenderer(fInternal.fSubpixRenderer, fInternal.fRenderer,
			fInternal.fRendererBin, fInternal.fUnpackedScanline,
			fInternal.fSubpixUnpackedScanline, fInternal.fSubpixRasterizer,
			fInternal.fMaskedUnpackedScanline, fTransform),
#endif
		fInternal(fPatternHandler)
	{
		fInternal.fPixelFormat.SetDrawingMode(fDrawingMode, fAlphaSrcMode,
			fAlphaFncMode);
	}

	BRect Bounds() const;
	void ConstrainClipping(const BRegion* region);
	const BRegion* ClippingRegion() const { return fClippingRegion; }
	void SetTransform(BAffineTransform transform, int32 xOffset,
		int32 yOffset);
	bool IsIdentityTransform() const { return fIdentityTransform; }
	const Transformable& Transform() const { return fTransform; }
	void SetHighColor(const rgb_color& color);
	rgb_color HighColor() const { return fPatternHandler.HighColor(); }
	void SetLowColor(const rgb_color& color);
	rgb_color LowColor() const { return fPatternHandler.LowColor(); }
	void SetPenSize(float size);
	float PenSize() const { return fPenSize; }
	void SetStrokeMode(cap_mode lineCap, join_mode joinMode,
		float miterLimit);
	void SetFillRule(int32 fillRule);
	void SetPattern(const pattern& p);
	pattern Pattern() const { return *fPatternHandler.GetR5Pattern(); }
	void SetDrawingMode(drawing_mode mode);
	drawing_mode DrawingMode() const { return fDrawingMode; }
	void SetBlendingMode(source_alpha srcAlpha, alpha_function alphaFunc);

	void StrokeLine(BPoint a, BPoint b);
	void StrokeLine(BPoint a, BPoint b, const BGradient& gradient);
	bool StraightLine(BPoint a, BPoint b, const rgb_color& c) const;
	BRect StrokeTriangle(BPoint pt1, BPoint pt2, BPoint pt3) const;
	BRect StrokeTriangle(BPoint pt1, BPoint pt2, BPoint pt3,
		const BGradient& gradient);
	BRect FillTriangle(BPoint pt1, BPoint pt2, BPoint pt3) const;
	BRect FillTriangle(BPoint pt1, BPoint pt2, BPoint pt3,
		const BGradient& gradient);
	BRect DrawPolygon(BPoint* ptArray, int32 numPts, bool filled,
		bool closed) const;
	BRect DrawPolygon(BPoint* ptArray, int32 numPts, bool filled,
		bool closed, const BGradient& gradient);
	BRect DrawBezier(BPoint* controlPoints, bool filled) const;
	BRect DrawBezier(BPoint* controlPoints, bool filled,
		const BGradient& gradient);
	BRect DrawShape(const int32& opCount, const uint32* opList,
		const int32& ptCount, const BPoint* ptList, bool filled,
		const BPoint& viewToScreenOffset, float viewScale) const;
	BRect DrawShape(const int32& opCount, const uint32* opList,
		const int32& ptCount, const BPoint* ptList, bool filled,
		const BGradient& gradient, const BPoint& viewToScreenOffset,
		float viewScale);
	BRect StrokeRect(const BRect& r) const;
	BRect StrokeRect(const BRect& r, const BGradient& gradient);
	void StrokeRect(const BRect& r, const rgb_color& c) const;
	BRect FillRect(const BRect& r) const;
	BRect FillRect(const BRect& r, const BGradient& gradient);
	void FillRect(const BRect& r, const rgb_color& c) const;
	void FillRectVerticalGradient(BRect r,
		const BGradientLinear& gradient) const;
	void FillRectNoClipping(const clipping_rect& r,
		const rgb_color& c) const;
	BRect StrokeRoundRect(const BRect& r, float xRadius,
		float yRadius) const;
	BRect StrokeRoundRect(const BRect& r, float xRadius, float yRadius,
		const BGradient& gradient);
	BRect FillRoundRect(const BRect& r, float xRadius,
		float yRadius) const;
	BRect FillRoundRect(const BRect& r, float xRadius, float yRadius,
		const BGradient& gradient);
	void AlignEllipseRect(BRect* rect, bool filled) const;
	BRect DrawEllipse(BRect r, bool filled) const;
	BRect DrawEllipse(BRect r, bool filled, const BGradient& gradient);
	BRect StrokeArc(BPoint center, float xRadius, float yRadius,
		float angle, float span) const;
	BRect StrokeArc(BPoint center, float xRadius, float yRadius,
		float angle, float span, const BGradient& gradient);
	BRect FillArc(BPoint center, float xRadius, float yRadius,
		float angle, float span) const;
	BRect FillArc(BPoint center, float xRadius, float yRadius,
		float angle, float span, const BGradient& gradient);
	BRect FillRegion(const BRegion* region) const;
#ifdef ORACLE_TEXT
	void SetFont(const ServerFont& font);
	BRect DrawString(const char* utf8String, uint32 length, BPoint baseLine,
		const escapement_delta* delta,
		FontCacheReference* cacheReference = NULL);
	BRect DrawString(const char* utf8String, uint32 length,
		const BPoint* offsets, FontCacheReference* cacheReference = NULL);
	const ServerFont& Font() const { return fTextRenderer.Font(); }
#endif
	BRect DrawBitmap(const ServerBitmap* bitmap, BRect bitmapRect,
		BRect viewRect, uint32 options) const;
	BRect FillRegion(const BRegion* region, const BGradient& gradient);
	inline BRect TransformAndClipRect(BRect rect) const;
	inline BRect ClipRect(BRect rect) const;
	inline BRect TransformAlignAndClipRect(BRect rect) const;
	inline BRect AlignAndClipRect(BRect rect) const;
	inline BRect AlignRect(BRect rect) const;

	float _Align(float coord, bool round, bool centerOffset) const;
	void _Align(BPoint* point, bool round, bool centerOffset) const;
	void _Align(BPoint* point, bool centerOffset = true) const;
	BPoint _Align(const BPoint& point, bool centerOffset = true) const;
	BRect _Clipped(const BRect& rect) const;
	void _UpdateDrawingMode();
	void _SetRendererColor(const rgb_color& color) const;
	BRect _DrawTriangle(BPoint pt1, BPoint pt2, BPoint pt3,
		bool fill) const;
	BRect _DrawTriangle(BPoint pt1, BPoint pt2, BPoint pt3, bool fill,
		const BGradient& gradient);
	void _IterateShapeData(const int32& opCount, const uint32* opList,
		const int32& ptCount, const BPoint* points,
		const BPoint& viewToScreenOffset, float viewScale) const;
	void _InvertRect32(BRect r) const;
	void _BlendRect32(const BRect& r, const rgb_color& c) const;
	template<class VertexSource>
	BRect _BoundingBox(VertexSource& path) const;
	template<class VertexSource>
	BRect _StrokePath(VertexSource& path) const;
	template<class VertexSource>
	BRect _StrokePath(VertexSource& path, cap_mode capMode) const;
	template<class VertexSource>
	BRect _StrokePath(VertexSource& path, const BGradient& gradient);
	template<class VertexSource>
	BRect _StrokePath(VertexSource& path, cap_mode capMode,
		const BGradient& gradient);
	template<class VertexSource>
	BRect _FillPath(VertexSource& path) const;
	template<class VertexSource>
	BRect _RasterizePath(VertexSource& path) const;
	template<class VertexSource>
	BRect _FillPath(VertexSource& path, const BGradient& gradient);
	template<class VertexSource>
	BRect _RasterizePath(VertexSource& path, const BGradient& gradient);
	void _CalcLinearGradientTransform(BPoint startPoint, BPoint endPoint,
		agg::trans_affine& mtx, float gradient_d2 = 100.0f) const;
	void _CalcRadialGradientTransform(BPoint center,
		agg::trans_affine& mtx, float gradient_d2 = 100.0f) const;
	void _MakeGradient(const BGradient& gradient, int32 colorCount,
		uint32* colors, int32 arrayOffset, int32 arraySize) const;
	template<class Array>
	void _MakeGradient(Array& array, const BGradient& gradient) const;
	template<class VertexSource, typename GradientFunction>
	void _RasterizePath(VertexSource& path, const BGradient& gradient,
		GradientFunction function, agg::trans_affine& gradientTransform,
		int gradientStop = 100);

	// Painter.h:346-348
	class BitmapPainter;
	friend class BitmapPainter;

	bool fSubpixelPrecise : 1;
	bool fValidClipping : 1;
	bool fAttached : 1;
	bool fIdentityTransform : 1;
	Transformable fTransform;
	float fPenSize;
	const BRegion* fClippingRegion;
	drawing_mode fDrawingMode;
	source_alpha fAlphaSrcMode;
	alpha_function fAlphaFncMode;
	cap_mode fLineCapMode;
	join_mode fLineJoinMode;
	float fMiterLimit;
	PatternHandler fPatternHandler;
#ifdef ORACLE_TEXT
	// Painter.h:369-372
	mutable AGGTextRenderer fTextRenderer;
#endif
	mutable PainterAggInterface fInternal;
};

#endif
