// Host shim for Haiku's <GraphicsDefs.h> and the parts of <InterfaceDefs.h>
// the Painter sources need. Enum values are copied from
// headers/os/interface/GraphicsDefs.h:13-50, :309-346 and
// headers/os/interface/InterfaceDefs.h:279-302.
#ifndef ORACLE_SHIM_GRAPHICS_DEFS_H
#define ORACLE_SHIM_GRAPHICS_DEFS_H

#include <SupportDefs.h>

typedef struct pattern {
	uint8 data[8];
} pattern;

inline bool
operator==(const pattern& a, const pattern& b)
{
	return memcmp(a.data, b.data, 8) == 0;
}

inline bool
operator!=(const pattern& a, const pattern& b)
{
	return !(a == b);
}

extern const pattern B_SOLID_HIGH;
extern const pattern B_MIXED_COLORS;
extern const pattern B_SOLID_LOW;

typedef struct rgb_color {
	uint8 red;
	uint8 green;
	uint8 blue;
	uint8 alpha;

	bool operator==(const rgb_color& other) const
	{
		return red == other.red && green == other.green
			&& blue == other.blue && alpha == other.alpha;
	}
	bool operator!=(const rgb_color& other) const
	{
		return !(*this == other);
	}
} rgb_color;

enum drawing_mode {
	B_OP_COPY,
	B_OP_OVER,
	B_OP_ERASE,
	B_OP_INVERT,
	B_OP_ADD,
	B_OP_SUBTRACT,
	B_OP_BLEND,
	B_OP_MIN,
	B_OP_MAX,
	B_OP_SELECT,
	B_OP_ALPHA
};

enum source_alpha {
	B_PIXEL_ALPHA = 0,
	B_CONSTANT_ALPHA
};

enum alpha_function {
	B_ALPHA_OVERLAY = 0,
	B_ALPHA_COMPOSITE,
	B_ALPHA_COMPOSITE_SOURCE_OVER = B_ALPHA_COMPOSITE,
	B_ALPHA_COMPOSITE_SOURCE_IN,
	B_ALPHA_COMPOSITE_SOURCE_OUT,
	B_ALPHA_COMPOSITE_SOURCE_ATOP,
	B_ALPHA_COMPOSITE_DESTINATION_OVER,
	B_ALPHA_COMPOSITE_DESTINATION_IN,
	B_ALPHA_COMPOSITE_DESTINATION_OUT,
	B_ALPHA_COMPOSITE_DESTINATION_ATOP,
	B_ALPHA_COMPOSITE_XOR,
	B_ALPHA_COMPOSITE_CLEAR,
	B_ALPHA_COMPOSITE_DIFFERENCE,
	B_ALPHA_COMPOSITE_LIGHTEN,
	B_ALPHA_COMPOSITE_DARKEN
};

enum join_mode {
	B_ROUND_JOIN = 0,
	B_MITER_JOIN,
	B_BEVEL_JOIN,
	B_BUTT_JOIN,
	B_SQUARE_JOIN
};

enum cap_mode {
	B_ROUND_CAP = B_ROUND_JOIN,
	B_BUTT_CAP = B_BUTT_JOIN,
	B_SQUARE_CAP = B_SQUARE_JOIN
};

const float B_DEFAULT_MITER_LIMIT = 10.0F;

enum {
	B_EVEN_ODD = 0,
	B_NONZERO
};

#endif
