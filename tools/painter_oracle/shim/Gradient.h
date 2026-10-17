// Host shim for the BGradient family: a data container with the accessors the
// Painter gradient code reads (headers/os/interface/Gradient.h:27-60 and the
// GradientLinear/Radial/... headers). Only linear gradients are built by the
// oracle; the other subclasses exist so Painter.cpp's casts compile.
#ifndef ORACLE_SHIM_GRADIENT_H
#define ORACLE_SHIM_GRADIENT_H

#include <vector>

#include <GraphicsDefs.h>
#include <Point.h>

class BGradient {
public:
	enum Type {
		TYPE_LINEAR = 0,
		TYPE_RADIAL,
		TYPE_RADIAL_FOCUS,
		TYPE_DIAMOND,
		TYPE_CONIC,
		TYPE_NONE
	};

	struct ColorStop {
		ColorStop() : offset(0) { color.red = color.green = color.blue = 0; color.alpha = 255; }
		ColorStop(const rgb_color c, float o) : color(c), offset(o) {}
		rgb_color color;
		float offset;
	};

	virtual ~BGradient() {}

	Type GetType() const { return fType; }
	int32 CountColorStops() const { return (int32)fStops.size(); }
	ColorStop* ColorStopAt(int32 index) const
	{
		if (index < 0 || index >= CountColorStops())
			return NULL;
		return const_cast<ColorStop*>(&fStops[index]);
	}
	ColorStop* ColorStopAtFast(int32 index) const
	{
		return const_cast<ColorStop*>(&fStops[index]);
	}
	void AddColor(const rgb_color& color, float offset)
	{
		fStops.push_back(ColorStop(color, offset));
	}

protected:
	Type fType = TYPE_NONE;
	std::vector<ColorStop> fStops;
};

class BGradientLinear : public BGradient {
public:
	BGradientLinear(BPoint start, BPoint end) : fStart(start), fEnd(end)
	{
		fType = TYPE_LINEAR;
	}
	BPoint Start() const { return fStart; }
	BPoint End() const { return fEnd; }

private:
	BPoint fStart;
	BPoint fEnd;
};

class BGradientRadial : public BGradient {
public:
	BPoint Center() const { return BPoint(); }
	float Radius() const { return 0; }
};

class BGradientRadialFocus : public BGradient {
public:
	BPoint Center() const { return BPoint(); }
	BPoint Focal() const { return BPoint(); }
	float Radius() const { return 0; }
};

class BGradientDiamond : public BGradient {
public:
	BPoint Center() const { return BPoint(); }
};

class BGradientConic : public BGradient {
public:
	BPoint Center() const { return BPoint(); }
	float Angle() const { return 0; }
};

#endif
