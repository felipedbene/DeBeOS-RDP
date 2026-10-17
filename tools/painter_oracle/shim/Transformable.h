// Host shim for src/servers/app/drawing/Painter/Transformable.h without the
// BArchivable half. The member function bodies come verbatim from
// Transformable.cpp, extracted at build time (see extract.py).
#ifndef ORACLE_SHIM_TRANSFORMABLE_H
#define ORACLE_SHIM_TRANSFORMABLE_H

#include <agg_trans_affine.h>

#include <Point.h>

class Transformable : public agg::trans_affine {
public:
	Transformable() : agg::trans_affine() {}
	Transformable(const Transformable& other) : agg::trans_affine(other) {}

	Transformable& operator=(const agg::trans_affine& other);
	void Reset();
	bool IsIdentity() const;
	void Transform(double* x, double* y) const;
	void Transform(BPoint* point) const;
	BRect TransformBounds(const BRect& bounds) const;
	void TransformationChanged() {}
};

#endif
