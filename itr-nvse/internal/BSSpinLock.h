#pragma once
#include <cstdint>
#include <cstring>

class BSSpinLock {
public:
	BSSpinLock() { memset(this, 0, sizeof(BSSpinLock)); }

	uint32_t uiOwningThread;
	uint32_t uiLockCount;
	uint32_t unk08[6];

	//0x40FBF0
	void Lock(const char* apName = nullptr) {
		((void(__thiscall*)(BSSpinLock*, const char*))0x40FBF0)(this, apName);
	}

	//0x40FBA0
	void Unlock() {
		((void(__thiscall*)(BSSpinLock*))0x40FBA0)(this);
	}
};
static_assert(sizeof(BSSpinLock) == 0x20);

class BSSpinLockScope {
public:
	BSSpinLockScope(BSSpinLock& lock) : pLock(&lock) { pLock->Lock(); }
	BSSpinLockScope(BSSpinLock* lock) : pLock(lock) { pLock->Lock(); }
	~BSSpinLockScope() { pLock->Unlock(); }
	BSSpinLockScope(const BSSpinLockScope&) = delete;
	BSSpinLockScope& operator=(const BSSpinLockScope&) = delete;
protected:
	BSSpinLock* pLock;
};
