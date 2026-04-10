//xNVSE event registration for ITR events
#pragma once

struct NVSEEventManagerInterface;

extern NVSEEventManagerInterface* g_eventManagerInterface;

inline void* PackEventFloatArg(float value)
{
	union
	{
		float f;
		UInt32 u;
	} bits;
	bits.f = value;
	return reinterpret_cast<void*>(bits.u);
}

namespace EventDispatch {
	void InitEventManager(void* nvseInterface);
	void RegisterEvents();
}
