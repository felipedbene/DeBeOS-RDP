// Host shim for <Bitmap.h>. BitmapPainter only names BBitmap for its colour
// space conversion, which the oracle never reaches: every scene's bitmap is
// B_RGBA32, for which _ConvertColorSpace returns at once (BitmapPainter.cpp:
// 261-262). See oracle.cpp's _ConvertColorSpace.
#ifndef ORACLE_SHIM_BITMAP_H
#define ORACLE_SHIM_BITMAP_H

class BBitmap;

#endif
