#pragma once
#include <Windows.h>

class ScopedLock {
	CRITICAL_SECTION* cs;
public:
	explicit ScopedLock(CRITICAL_SECTION* c) : cs(c) { EnterCriticalSection(cs); }
	~ScopedLock() { LeaveCriticalSection(cs); }
	ScopedLock(const ScopedLock&) = delete;
	ScopedLock& operator=(const ScopedLock&) = delete;
};

//double-checked lock-once init for CRITICAL_SECTION
inline void InitCriticalSectionOnce(volatile LONG* initFlag, CRITICAL_SECTION* lock)
{
	if (*initFlag == 2) return;

	if (InterlockedCompareExchange(initFlag, 1, 0) == 0)
	{
		InitializeCriticalSection(lock);
		InterlockedExchange(initFlag, 2);
		return;
	}

	while (*initFlag != 2)
		Sleep(0);
}
