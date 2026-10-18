// Host shim for headers/os/support/ObjectList.h: the owning list
// GlyphLayoutEngine::LayoutGlyphs keeps its fallback references in. Only the
// members it calls.
#ifndef ORACLE_SHIM_OBJECT_LIST_H
#define ORACLE_SHIM_OBJECT_LIST_H

#include <vector>

#include <SupportDefs.h>

template<class T, bool Owning = false>
class BObjectList {
public:
	BObjectList(int32 itemsPerBlock = 20) {}
	~BObjectList()
	{
		if (Owning) {
			for (T* item : fItems)
				delete item;
		}
	}
	bool IsEmpty() const { return fItems.empty(); }
	int32 CountItems() const { return (int32)fItems.size(); }
	T* ItemAt(int32 index) const { return fItems[index]; }
	bool AddItem(T* item) { fItems.push_back(item); return true; }

private:
	std::vector<T*> fItems;
};

#endif
