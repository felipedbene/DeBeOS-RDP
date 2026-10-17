// BBitmap::ImportBits for the oracle (see Bitmap.h), included by the extracted
// translation unit right after ColorConversion.cpp's per-pixel ConvertBits
// template, which does the converting. What is hand-written here is only the
// plumbing that leads to it, for the one conversion a scene reaches:
//
//   BBitmap::ImportBits                         Bitmap.cpp:710-732
//   -> BPrivate::ConvertBits(..., width, height)  ColorConversion.cpp:1099-1107
//   -> ConvertBits(..., BPoint(0, 0), BPoint(0, 0), ...)          :1128-1135
//      case B_RGB32: shifts 24, 16, 8, alpha shift 0, alphaBits 0  :1166-1170
//   -> template ConvertBits<srcByte>, case B_RGBA32: shifts - 24, - 16, - 8,
//      alpha shift - 32, masks 00ff0000 0000ff00 000000ff ff000000 :959-966
//   -> template ConvertBits<srcByte, dstByte>, the extracted one    :726-888
//
// Anything else stops the oracle, so a scene cannot silently reach a
// conversion that is not the server's.
#ifndef ORACLE_SHIM_BITMAP_IMPORT_H
#define ORACLE_SHIM_BITMAP_IMPORT_H

#include <stdio.h>
#include <stdlib.h>

status_t
BBitmap::ImportBits(const void* data, int32 length, int32 bpr, int32 offset,
	color_space colorSpace)
{
	if (colorSpace != B_RGB32 || fColorSpace != B_RGBA32 || bpr <= 0) {
		fprintf(stderr, "painter oracle: ImportBits 0x%x -> 0x%x, bpr %d is "
			"not modelled\n", (unsigned)colorSpace, (unsigned)fColorSpace,
			(int)bpr);
		exit(2);
	}

	// Bitmap.cpp:718-719
	if (!data || offset > BitsLength() || length < 0)
		return -1;

	const int32 width = fBounds.IntegerWidth() + 1;
	const int32 height = fBounds.IntegerHeight() + 1;
	const int32 dstBitsLength = BitsLength() - offset;
	// ColorConversion.cpp:1133-1135
	if (width < 0 || height < 0 || length < 0 || dstBitsLength < 0)
		return -1;

	const int32 redShift = 24, greenShift = 16, blueShift = 8;
	const int32 alphaShift = 0, alphaBits = 0;
	return ConvertBits((const uint32*)data,
		(uint32*)((uint8*)Bits() + offset), length, dstBitsLength,
		redShift - 24, greenShift - 16, blueShift - 8, alphaShift - 32,
		alphaBits, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000, bpr,
		fBytesPerRow, 32, 32, colorSpace, fColorSpace, BPoint(0, 0),
		BPoint(0, 0), width, height, false, false, (readFunc*)NULL,
		(writeFunc*)NULL);
}

#endif
