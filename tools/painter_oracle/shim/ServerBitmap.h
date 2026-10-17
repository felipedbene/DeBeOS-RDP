// Host shim for src/servers/app/ServerBitmap.h: the accessors BitmapPainter
// reads (ServerBitmap.h:40-70), over a bitmap the oracle owns. Bounds() is
// (0, 0, width - 1, height - 1) as ServerBitmap::Bounds() returns it.
#ifndef ORACLE_SHIM_SERVER_BITMAP_H
#define ORACLE_SHIM_SERVER_BITMAP_H

#include <GraphicsDefs.h>
#include <Point.h>

class ServerBitmap {
public:
	ServerBitmap(uint8* bits, int32 width, int32 height, int32 bytesPerRow,
		color_space space)
		: fBits(bits), fWidth(width), fHeight(height),
		  fBytesPerRow(bytesPerRow), fSpace(space) {}

	bool IsValid() const { return fBits != NULL; }
	uint8* Bits() const { return fBits; }
	int32 Width() const { return fWidth; }
	int32 Height() const { return fHeight; }
	int32 BytesPerRow() const { return fBytesPerRow; }
	color_space ColorSpace() const { return fSpace; }
	BRect Bounds() const { return BRect(0, 0, fWidth - 1, fHeight - 1); }

private:
	uint8* fBits;
	int32 fWidth;
	int32 fHeight;
	int32 fBytesPerRow;
	color_space fSpace;
};

#endif
