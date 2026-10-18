// Host shim for headers/os/support/Debug.h: GlyphLayoutEngine.h ASSERTs its
// lock discipline. Checked, as a debug app_server would.
#ifndef ORACLE_SHIM_DEBUG_H
#define ORACLE_SHIM_DEBUG_H

#include <assert.h>

#ifndef ASSERT
#	define ASSERT(x) assert(x)
#endif

#endif
