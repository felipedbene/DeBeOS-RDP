// Host shim for src/servers/app/font/FontCache.h. FontCache::FontCacheEntryFor
// (FontCache.cpp:48-97) keys its entries on FontCacheEntry::GenerateSignature
// -- run here as it is on the server -- and creates and Init()s an entry the
// first time a signature is asked for, so a later font that differs only in
// what the signature leaves out (a size within the same "%.1f", say) shares the
// first one's FontEngine, as it does on the server. The signature's first two
// fields name the family and style through the font manager, which the oracle
// has none of; the file and face index stand in for them. Entries live for the
// whole run (the server evicts beyond 30, FontCache.cpp:113-155, which no scene
// set comes near).
#ifndef ORACLE_SHIM_FONT_CACHE_H
#define ORACLE_SHIM_FONT_CACHE_H

#include <map>
#include <string>

#include "FontCacheEntry.h"
#include "ServerFont.h"

class FontCache {
public:
	static FontCache* Default()
	{
		static FontCache sDefaultInstance;
		return &sDefaultInstance;
	}

	FontCacheEntry* FontCacheEntryFor(const ServerFont& font, bool forceVector)
	{
		char signature[512];
		FontCacheEntry::GenerateSignature(signature, sizeof(signature), font,
			forceVector);
		std::string key = std::string(font.Path()) + "|"
			+ std::to_string(font.FaceIndex()) + "|" + signature;
		std::map<std::string, FontCacheEntry*>::iterator found
			= fEntries.find(key);
		if (found != fEntries.end())
			return found->second;
		FontCacheEntry* entry = new FontCacheEntry();
		if (!entry->Init(font, forceVector)) {
			fprintf(stderr, "FontCache::FontCacheEntryFor() - "
				"out of memory or no font file\n");
			delete entry;
			return NULL;
		}
		fEntries[key] = entry;
		return entry;
	}

	void Recycle(FontCacheEntry* entry)
	{
		if (entry != NULL)
			entry->UpdateUsage();
	}

	// Drop every entry: between scenes, so each starts from the server's
	// state after a fresh boot rather than inheriting another scene's cache.
	void Clear()
	{
		for (std::map<std::string, FontCacheEntry*>::iterator it
				= fEntries.begin(); it != fEntries.end(); ++it) {
			delete it->second;
		}
		fEntries.clear();
	}

private:
	std::map<std::string, FontCacheEntry*> fEntries;
};

#endif
