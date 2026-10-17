// Host shim for Haiku's <SupportDefs.h>: only the integer types and helpers
// that the app_server sources compiled by the Painter oracle use. Definitions
// mirror headers/os/support/SupportDefs.h; nothing here is rendering logic.
#ifndef ORACLE_SHIM_SUPPORT_DEFS_H
#define ORACLE_SHIM_SUPPORT_DEFS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int64_t int64;
typedef uint64_t uint64;
typedef int32 status_t;

#define B_OK 0
#define B_ERROR (-1)
// Errors.h:45; only ever compared against B_OK here.
#define B_NO_INIT (-2147483647 - 1 + 13)

#ifndef min_c
#	define min_c(a, b) ((a) > (b) ? (b) : (a))
#endif
#ifndef max_c
#	define max_c(a, b) ((a) > (b) ? (a) : (b))
#endif

#endif
