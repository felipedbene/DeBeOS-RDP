// Host shim for src/servers/app/font/GlobalFontManager.h. The oracle has no
// installed fonts to fall back to: Lock() fails, so
// GlyphLayoutEngine::PopulateFallbacks returns before asking for any style
// (GlyphLayoutEngine.h:426-427) and a code point the face lacks takes the
// server's no-fallback path -- the face's own .notdef, or a space for
// whitespace (FontCacheEntry::CreateGlyph). The text scenes only draw code
// points their face has.
#ifndef ORACLE_SHIM_GLOBAL_FONT_MANAGER_H
#define ORACLE_SHIM_GLOBAL_FONT_MANAGER_H

#include <SupportDefs.h>

class FontStyle;

class GlobalFontManager {
public:
	bool Lock() { return false; }
	void Unlock() {}
	FontStyle* GetStyle(const char* family, const char* style) const
		{ return 0; }
	FontStyle* GetStyleByIndex(const char* family, int32 index) const
		{ return 0; }
};

extern GlobalFontManager* gFontManager;

#endif
