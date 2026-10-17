// Host shim for Haiku's BRegion, restricted to what the Painter and
// DrawingEngine code paths the oracle compiles call. Rects are held inclusive
// and must be disjoint, which every region the oracle builds is. Conversions
// follow src/kits/interface/Region.cpp: a BRect becomes the integer rect
// {floorf(left), floorf(top), ceilf(right), ceilf(bottom)} (_Convert, :537),
// Frame() of an empty region is the invalid (0, 0, -1, -1) (:180-185), and
// Contains(BPoint) truncates the point to int (:268-271).
#ifndef ORACLE_SHIM_REGION_H
#define ORACLE_SHIM_REGION_H

#include <math.h>

#include <vector>

#include <Point.h>

class BRegion {
public:
	BRegion() {}
	BRegion(const BRect& rect) { Set(rect); }

	void MakeEmpty() { fRects.clear(); }

	void Set(const BRect& rect)
	{
		fRects.clear();
		clipping_rect r = { (int32)floorf(rect.left), (int32)floorf(rect.top),
			(int32)ceilf(rect.right), (int32)ceilf(rect.bottom) };
		Include(r);
	}

	// Adds a rect that must not overlap any rect already present.
	void Include(clipping_rect r)
	{
		if (r.left <= r.right && r.top <= r.bottom)
			fRects.push_back(r);
	}

	void IntersectWith(const BRegion* other)
	{
		std::vector<clipping_rect> result;
		for (const clipping_rect& a : fRects) {
			for (const clipping_rect& b : other->fRects) {
				clipping_rect r = { max_c(a.left, b.left), max_c(a.top, b.top),
					min_c(a.right, b.right), min_c(a.bottom, b.bottom) };
				if (r.left <= r.right && r.top <= r.bottom)
					result.push_back(r);
			}
		}
		fRects = result;
	}

	int32 CountRects() const { return (int32)fRects.size(); }

	clipping_rect RectAtInt(int32 index) const
	{
		if (index >= 0 && index < CountRects())
			return fRects[index];
		clipping_rect invalid = { 1, 1, 0, 0 };
		return invalid;
	}

	BRect RectAt(int32 index) const
	{
		if (index >= 0 && index < CountRects()) {
			const clipping_rect& r = fRects[index];
			return BRect(r.left, r.top, r.right, r.bottom);
		}
		return BRect();
	}

	clipping_rect FrameInt() const
	{
		if (fRects.empty()) {
			clipping_rect empty = { 0, 0, -1, -1 };
			return empty;
		}
		clipping_rect f = fRects[0];
		for (const clipping_rect& r : fRects) {
			f.left = min_c(f.left, r.left);
			f.top = min_c(f.top, r.top);
			f.right = max_c(f.right, r.right);
			f.bottom = max_c(f.bottom, r.bottom);
		}
		return f;
	}

	BRect Frame() const
	{
		clipping_rect f = FrameInt();
		return BRect(f.left, f.top, f.right, f.bottom);
	}

	bool Contains(BPoint point) const
	{
		int x = (int)point.x;
		int y = (int)point.y;
		for (const clipping_rect& r : fRects) {
			if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom)
				return true;
		}
		return false;
	}

private:
	std::vector<clipping_rect> fRects;
};

#endif
