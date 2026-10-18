// Host shim for src/servers/app/ServerFont.h, for the text half of the Painter
// oracle (text_oracle.cpp). The real ServerFont reaches its face through a
// FontStyle owned by the FontManager; the oracle has no font manager, so this
// one carries the file, the face index and the family/style names itself.
// Every accessor the text path calls (AGGTextRenderer.cpp, FontCacheEntry.cpp,
// GlyphLayoutEngine.h) returns the field it does on the server, and Hinting()
// is ServerFont.h's own inline body. Nothing here is rendering logic.
#ifndef ORACLE_SHIM_SERVER_FONT_H
#define ORACLE_SHIM_SERVER_FONT_H

#include <stdio.h>
#include <stdlib.h>
#include <string>

#include <SupportDefs.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "GlobalSubpixelSettings.h"
#include "Transformable.h"

// headers/os/interface/Font.h:23-28, 38-41, 79-91, 138-149
enum {
	B_CHAR_SPACING			= 0,
	B_STRING_SPACING		= 1,
	B_BITMAP_SPACING		= 2,
	B_FIXED_SPACING			= 3
};

enum {
	B_DISABLE_ANTIALIASING	= 0x00000001,
	B_FORCE_ANTIALIASING	= 0x00000002
};

enum {
	B_ITALIC_FACE			= 0x0001,
	B_UNDERSCORE_FACE		= 0x0002,
	B_NEGATIVE_FACE			= 0x0004,
	B_OUTLINED_FACE			= 0x0008,
	B_STRIKEOUT_FACE		= 0x0010,
	B_BOLD_FACE				= 0x0020,
	B_REGULAR_FACE			= 0x0040,
	B_CONDENSED_FACE		= 0x0080,
	B_LIGHT_FACE			= 0x0100,
	B_HEAVY_FACE			= 0x0200
};

struct font_height {
	float ascent;
	float descent;
	float leading;
};

struct escapement_delta {
	float nonspace;
	float space;
};

class FontManager;
class FontStyle;

class ServerFont {
public:
	ServerFont()
		: fFaceIndex(0), fFixedWidth(false), fSize(12.0), fRotation(0.0),
		fShear(90.0), fFalseBoldWidth(0.0), fFlags(0),
		fSpacing(B_BITMAP_SPACING), fFace(B_REGULAR_FACE)
	{
	}

	// GlyphLayoutEngine::PopulateFallbacks builds one of these per fallback
	// style; the oracle's font manager offers none (GlobalFontManager.h).
	ServerFont(FontStyle& style, float size)
	{
		fprintf(stderr, "text oracle: no fallback fonts\n");
		abort();
	}

	// The oracle's own setter: the file and face the FontStyle would carry.
	void SetFile(const std::string& path, long faceIndex,
		const std::string& family, const std::string& style, bool fixedWidth)
	{
		fPath = path;
		fFaceIndex = faceIndex;
		fFamily = family;
		fStyle = style;
		fFixedWidth = fixedWidth;
	}

	uint32 Flags() const { return fFlags; }
	uint32 Spacing() const { return fSpacing; }
	float Shear() const { return fShear; }
	float Rotation() const { return fRotation; }
	float FalseBoldWidth() const { return fFalseBoldWidth; }
	float Size() const { return fSize; }
	uint16 Face() const { return fFace; }
	const char* Style() const { return fStyle.c_str(); }
	const char* Family() const { return fFamily.c_str(); }
	const char* Path() const { return fPath.c_str(); }
	long FaceIndex() const { return fFaceIndex; }
	bool IsFixedWidth() const { return fFixedWidth; }

	// FontCacheEntry::GenerateSignature's first field; the oracle keys its
	// cache on the file instead (FontCache.h), so this only has to be stable.
	uint32 GetFamilyAndStyle() const { return 0; }
	FontManager* Manager() const { return NULL; }

	void SetFlags(uint32 value) { fFlags = value; }
	void SetSpacing(uint32 value) { fSpacing = value; }
	void SetShear(float value) { fShear = value; }
	void SetSize(float value) { fSize = value; }
	void SetRotation(float value) { fRotation = value; }
	void SetFalseBoldWidth(float value) { fFalseBoldWidth = value; }
	void SetFace(uint16 face) { fFace = face; }

	inline bool Hinting() const;

	// Only B_STRIKEOUT_FACE asks for the height (AGGTextRenderer.cpp:178-182),
	// and no oracle scene strikes out.
	void GetHeight(font_height& height) const
	{
		fprintf(stderr, "text oracle: ServerFont::GetHeight is not shimmed\n");
		abort();
	}

	uint32 FontDataSize() const { return 0; }
	FT_Byte* FontData() const { return NULL; }

private:
	std::string fPath;
	long fFaceIndex;
	std::string fFamily;
	std::string fStyle;
	bool fFixedWidth;
	float fSize;
	float fRotation;
	float fShear;
	float fFalseBoldWidth;
	uint32 fFlags;
	uint32 fSpacing;
	uint16 fFace;
};

// ServerFont.h:206-217, verbatim.
inline bool ServerFont::Hinting() const
{
	switch (gDefaultHintingMode) {
		case HINTING_MODE_OFF:
			return false;
		default:
		case HINTING_MODE_ON:
			return true;
		case HINTING_MODE_MONOSPACED_ONLY:
			return IsFixedWidth();
	}
}

#endif
