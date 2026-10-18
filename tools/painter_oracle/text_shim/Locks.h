// Host shims for the locks the font cache takes: BLocker (Locker.h), BAutolock
// (Autolock.h) and MultiLocker (MultiLocker.h). The oracle is one thread, so
// every lock succeeds and guards nothing; included by those three names.
#ifndef ORACLE_SHIM_LOCKS_H
#define ORACLE_SHIM_LOCKS_H

class BLocker {
public:
	BLocker(const char* name = 0) {}
	bool Lock() { return true; }
	void Unlock() {}
};

class BAutolock {
public:
	BAutolock(BLocker& locker) {}
	BAutolock(BLocker* locker) {}
	bool IsLocked() const { return true; }
};

class MultiLocker {
public:
	MultiLocker(const char* name) {}
	virtual ~MultiLocker() {}
	bool ReadLock() { return true; }
	bool WriteLock() { return true; }
	bool ReadUnlock() { return true; }
	bool WriteUnlock() { return true; }
	bool IsWriteLocked() const { return true; }
	bool IsReadLocked() const { return true; }
};

#endif
