// What Painter.cpp, Transformable.cpp and DrawingEngine.cpp include, reduced to
// the headers their extracted functions need. AGG and the Painter-private
// headers are the real ones from the Haiku tree.
#ifndef ORACLE_PRELUDE_H
#define ORACLE_PRELUDE_H

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <agg_bezier_arc.h>
#include <agg_bounding_rect.h>
#include <agg_conv_curve.h>
#include <agg_conv_stroke.h>
#include <agg_conv_transform.h>
#include <agg_ellipse.h>
#include <agg_path_storage.h>
#include <agg_pixfmt_rgba.h>
#include <agg_rounded_rect.h>
#include <agg_span_allocator.h>
#include <agg_span_gradient.h>
#include <agg_span_interpolator_linear.h>

#include "drawing_support.h"
#include "GlobalSubpixelSettings.h"

#include "DrawingEngine.h"

// Painter.cpp:69-83 compiles its tracing out the same way.
#define TRACE(x...)
#define GTRACE(x...)

// headers/private/interface/ShapePrivate.h:21-28
#define OP_LINETO			0x10000000
#define OP_BEZIERTO			0x20000000
#define OP_CLOSE			0x40000000
#define OP_MOVETO			0x80000000
#define OP_LARGE_ARC_TO_CW	0x01000000
#define OP_LARGE_ARC_TO_CCW	0x02000000
#define OP_SMALL_ARC_TO_CW	0x04000000
#define OP_SMALL_ARC_TO_CCW	0x08000000

#endif
