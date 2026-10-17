// Host shim for the slice of src/servers/app/drawing/DrawingEngine.h that the
// oracle drives. The DrawTransaction class and every member function body are
// extracted verbatim from DrawingEngine.cpp by extract.py; the graphics card is
// a stand-in whose overlay calls do nothing, which is all a software framebuffer
// sees of them.
#ifndef ORACLE_SHIM_DRAWING_ENGINE_H
#define ORACLE_SHIM_DRAWING_ENGINE_H

#include "Painter.h"

#define ASSERT_PARALLEL_LOCKED()

struct ViewLineArrayInfo {
	BPoint startPoint;
	BPoint endPoint;
	rgb_color color;
};

struct OracleGraphicsCard {
	bool HideFloatingOverlays(const BRect&) { return false; }
	bool HideFloatingOverlays() { return false; }
	void ShowFloatingOverlays() {}
	void InvalidateRegion(const BRegion&) {}
};

class DrawingEngine {
public:
	DrawingEngine(Painter* painter) : fPainter(painter) {}

	void DrawArc(BRect r, const float& angle, const float& span, bool filled);
	void DrawArc(BRect r, const float& angle, const float& span, bool filled,
		const BGradient& gradient);
	void DrawBezier(BPoint* pts, bool filled);
	void DrawBezier(BPoint* pts, bool filled, const BGradient& gradient);
	void DrawEllipse(BRect r, bool filled);
	void DrawEllipse(BRect r, bool filled, const BGradient& gradient);
	void DrawPolygon(BPoint* ptlist, int32 numpts, BRect bounds, bool filled,
		bool closed);
	void DrawPolygon(BPoint* ptlist, int32 numpts, BRect bounds, bool filled,
		bool closed, const BGradient& gradient);
	void StrokePoint(const BPoint& pt, const rgb_color& color);
	void StrokeLine(const BPoint& start, const BPoint& end,
		const rgb_color& color);
	void StrokeRect(BRect r, const rgb_color& color);
	void FillRect(BRect r, const rgb_color& color);
	void FillRegion(BRegion& r, const rgb_color& color);
	void StrokeRect(BRect r);
	void FillRect(BRect r);
	void StrokeRect(BRect r, const BGradient& gradient);
	void FillRect(BRect r, const BGradient& gradient);
	void FillRegion(BRegion& r);
	void FillRegion(BRegion& r, const BGradient& gradient);
	void DrawRoundRect(BRect r, float xrad, float yrad, bool filled);
	void DrawRoundRect(BRect r, float xrad, float yrad, bool filled,
		const BGradient& gradient);
	void DrawShape(const BRect& bounds, int32 opCount, const uint32* opList,
		int32 ptCount, const BPoint* ptList, bool filled,
		const BPoint& viewToScreenOffset, float viewScale);
	void DrawShape(const BRect& bounds, int32 opCount, const uint32* opList,
		int32 ptCount, const BPoint* ptList, bool filled,
		const BGradient& gradient, const BPoint& viewToScreenOffset,
		float viewScale);
	void DrawTriangle(BPoint* pts, const BRect& bounds, bool filled);
	void DrawTriangle(BPoint* pts, const BRect& bounds, bool filled,
		const BGradient& gradient);
	void StrokeLine(const BPoint& start, const BPoint& end);
	void StrokeLine(const BPoint& start, const BPoint& end,
		const BGradient& gradient);
	void StrokeLineArray(int32 numlines, const ViewLineArrayInfo* data);

	Painter* fPainter;
	OracleGraphicsCard fGraphicsCardStorage;
	OracleGraphicsCard* fGraphicsCard = &fGraphicsCardStorage;
	bool fCopyToFront = false;
};

#endif
