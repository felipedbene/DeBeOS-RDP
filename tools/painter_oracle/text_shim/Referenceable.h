// Host shim for headers/os/support/Referenceable.h: FontCacheEntry derives
// from BReferenceable; the oracle's FontCache (FontCache.h) owns its entries
// outright, so the count is kept but never frees anything.
#ifndef ORACLE_SHIM_REFERENCEABLE_H
#define ORACLE_SHIM_REFERENCEABLE_H

#include <SupportDefs.h>

class BReferenceable {
public:
	BReferenceable() : fReferenceCount(1) {}
	virtual ~BReferenceable() {}
	int32 AcquireReference() { return fReferenceCount++; }
	int32 ReleaseReference() { return fReferenceCount--; }
	int32 CountReferences() const { return fReferenceCount; }

private:
	int32 fReferenceCount;
};

#endif
