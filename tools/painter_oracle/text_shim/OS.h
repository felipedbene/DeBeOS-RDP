// Host shim for Haiku's <OS.h>, as far as the text path reaches it:
// OpenHashTable.h includes it, FontCacheEntry.cpp stamps a use time.
#ifndef ORACLE_SHIM_OS_H
#define ORACLE_SHIM_OS_H

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <SupportDefs.h>

// Errors.h; OpenHashTable.h returns it when an allocation fails.
#ifndef B_NO_MEMORY
#	define B_NO_MEMORY (-2147483647 - 1)
#endif

// OS.h; OpenHashTable.h calls it on a duplicate insert.
inline void debugger(const char* message)
{
	fprintf(stderr, "debugger: %s\n", message);
	abort();
}

#ifndef LONGLONG_MIN
#	define LONGLONG_MIN LLONG_MIN
#endif

// Only FontCacheEntry::UpdateUsage reads the clock, to rank cache entries for
// eviction; the glyphs do not depend on it.
inline bigtime_t system_time() { return 0; }

#endif
