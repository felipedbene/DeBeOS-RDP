// Host shim for the BGradient family: a data container with the accessors the
// Painter gradient code reads (headers/os/interface/Gradient.h:27-60 and the
// GradientLinear/Radial/RadialFocus/Diamond/Conic headers). Each subclass
// carries exactly the geometry its public accessors return; Painter.cpp's
// casts and accessors are the server's own.
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
	BGradientRadial(BPoint center, float radius)
		: fCenter(center), fRadius(radius)
	{
		fType = TYPE_RADIAL;
	}
	BPoint Center() const { return fCenter; }
	float Radius() const { return fRadius; }

private:
	BPoint fCenter;
	float fRadius;
};

class BGradientRadialFocus : public BGradient {
public:
	BGradientRadialFocus(BPoint center, float radius, BPoint focal)
		: fCenter(center), fFocal(focal), fRadius(radius)
	{
		fType = TYPE_RADIAL_FOCUS;
	}
	BPoint Center() const { return fCenter; }
	BPoint Focal() const { return fFocal; }
	float Radius() const { return fRadius; }

private:
	BPoint fCenter;
	BPoint fFocal;
	float fRadius;
};

class BGradientDiamond : public BGradient {
public:
	explicit BGradientDiamond(BPoint center) : fCenter(center)
	{
		fType = TYPE_DIAMOND;
	}
	BPoint Center() const { return fCenter; }

private:
	BPoint fCenter;
};

class BGradientConic : public BGradient {
public:
	BGradientConic(BPoint center, float angle) : fCenter(center), fAngle(angle)
	{
		fType = TYPE_CONIC;
	}
	BPoint Center() const { return fCenter; }
	float Angle() const { return fAngle; }

private:
	BPoint fCenter;
	float fAngle;
};

#endif
