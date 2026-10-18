// Host shim for headers/private/shared/AutoDeleter.h, for the text oracle:
// shim/AutoDeleter.h's ObjectDeleter plus the IsSet() and operator-> that
// FontCacheEntry uses on its glyph cache pool. Found ahead of shim/ only when
// the oracle is built with the text path.
#ifndef ORACLE_SHIM_AUTO_DELETER_H
#define ORACLE_SHIM_AUTO_DELETER_H

template<typename C>
class ObjectDeleter {
public:
	ObjectDeleter() : fObject(0) {}
	ObjectDeleter(C* object) : fObject(object) {}
	~ObjectDeleter() { delete fObject; }
	void SetTo(C* object) { delete fObject; fObject = object; }
	bool IsSet() const { return fObject != 0; }
	C* Get() const { return fObject; }
	C* operator->() const { return fObject; }

private:
	ObjectDeleter(const ObjectDeleter&);
	ObjectDeleter& operator=(const ObjectDeleter&);

	C* fObject;
};

#endif
