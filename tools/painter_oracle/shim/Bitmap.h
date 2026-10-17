// Host shim for <Bitmap.h>: the BBitmap that BitmapPainter::_ConvertColorSpace
// (BitmapPainter.cpp:258-329) builds to hold a converted bitmap. Only what it
// calls is here -- the constructor, Bits(), BytesPerRow() and the five
// argument ImportBits -- and only the conversion a scene reaches, B_RGB32 into
// B_RGBA32. ImportBits is defined in BitmapImport.h, after extract.py has
// placed ColorConversion.cpp's ConvertBits in the same translation unit.
#ifndef ORACLE_SHIM_BITMAP_H
#define ORACLE_SHIM_BITMAP_H

#include <vector>

#include <GraphicsDefs.h>
#include <Rect.h>

// Bitmap.h:37
#define B_BITMAP_NO_SERVER_LINK 0x00000100

class BBitmap {
public:
	// Bitmap.cpp: a B_BITMAP_NO_SERVER_LINK bitmap is plain heap memory of
	// get_bytes_per_row(colorSpace, width) * height bytes; for the one colour
	// space created here, B_RGBA32, a row is width * 4 bytes.
	BBitmap(BRect bounds, uint32 flags, color_space colorSpace)
		:
		fBounds(bounds),
		fColorSpace(colorSpace),
		fBytesPerRow((bounds.IntegerWidth() + 1) * 4),
		fBits((size_t)fBytesPerRow * (bounds.IntegerHeight() + 1))
	{
	}

	void* Bits() const { return (void*)fBits.data(); }
	int32 BytesPerRow() const { return fBytesPerRow; }
	int32 BitsLength() const { return (int32)fBits.size(); }

	status_t ImportBits(const void* data, int32 length, int32 bpr,
		int32 offset, color_space colorSpace);

private:
	BRect fBounds;
	color_space fColorSpace;
	int32 fBytesPerRow;
	std::vector<uint8> fBits;
};

#endif
