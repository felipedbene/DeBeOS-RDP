// Host shim for Haiku's BPoint and BRect, with exactly the semantics of
// src/kits/interface/Point.cpp and Rect.cpp for the members used (Width() is
// right - left, OffsetTo keeps the width, InsetBy moves both edges).
#ifndef ORACLE_SHIM_POINT_H
#define ORACLE_SHIM_POINT_H

#include <math.h>

#include <SupportDefs.h>

class BPoint {
public:
	float x;
	float y;

	BPoint() : x(0), y(0) {}
	BPoint(float x_, float y_) : x(x_), y(y_) {}

	bool operator==(const BPoint& o) const { return x == o.x && y == o.y; }
	bool operator!=(const BPoint& o) const { return !(*this == o); }
};

struct clipping_rect {
	int32 left;
	int32 top;
	int32 right;
	int32 bottom;
};

class BRect {
public:
	float left;
	float top;
	float right;
	float bottom;

	BRect() : left(0), top(0), right(-1), bottom(-1) {}
	BRect(float l, float t, float r, float b)
		: left(l), top(t), right(r), bottom(b) {}
	BRect(BPoint lt, BPoint rb)
		: left(lt.x), top(lt.y), right(rb.x), bottom(rb.y) {}

	bool IsValid() const { return left <= right && top <= bottom; }
	float Width() const { return right - left; }
	float Height() const { return bottom - top; }
	int32 IntegerWidth() const { return (int32)ceil(right - left); }
	int32 IntegerHeight() const { return (int32)ceil(bottom - top); }

	void InsetBy(float dx, float dy)
	{
		left += dx;
		right -= dx;
		top += dy;
		bottom -= dy;
	}

	void OffsetTo(float x, float y)
	{
		right = (right - left) + x;
		left = x;
		bottom = (bottom - top) + y;
		top = y;
	}

	void OffsetTo(BPoint p) { OffsetTo(p.x, p.y); }

	bool Contains(BRect o) const
	{
		return o.IsValid() && left <= o.left && right >= o.right
			&& top <= o.top && bottom >= o.bottom;
	}

	BRect operator&(BRect o) const
	{
		return BRect(max_c(left, o.left), max_c(top, o.top),
			min_c(right, o.right), min_c(bottom, o.bottom));
	}

	BRect operator|(BRect o) const
	{
		return BRect(min_c(left, o.left), min_c(top, o.top),
			max_c(right, o.right), max_c(bottom, o.bottom));
	}

	bool operator==(BRect o) const
	{
		return left == o.left && top == o.top && right == o.right
			&& bottom == o.bottom;
	}
	bool operator!=(BRect o) const { return !(*this == o); }
};

#endif
