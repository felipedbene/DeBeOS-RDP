// Host shim for headers/private/shared/AutoDeleter.h: the one member of
// ObjectDeleter BitmapPainter uses.
#ifndef ORACLE_SHIM_AUTO_DELETER_H
#define ORACLE_SHIM_AUTO_DELETER_H

template<typename C>
class ObjectDeleter {
public:
	ObjectDeleter() : fObject(0) {}
	~ObjectDeleter() { delete fObject; }
	void SetTo(C* object) { delete fObject; fObject = object; }

private:
	C* fObject;
};

#endif
