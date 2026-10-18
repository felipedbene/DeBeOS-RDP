/*
 * Force-included into src/gzip/ftgzip.c on Apple targets only (Makefile,
 * CMakeLists.txt). FreeType 2.6.3 bundles zlib 1.1.4, whose zconf.h skips
 * `typedef unsigned char Byte;` whenever TARGET_OS_MAC is defined, expecting
 * the classic Mac OS MacTypes.h to have supplied it; every current macOS SDK
 * defines TARGET_OS_MAC and nothing supplies Byte, so the file does not
 * compile. This is the typedef zconf.h would have made. It changes no code
 * path: the gzip stream only ever decompresses .gz font files.
 */
typedef unsigned char Byte;
