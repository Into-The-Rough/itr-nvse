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

class TESObjectREFR;

namespace EventDispatch {
	void InitEventManager(void* nvseInterface);
	void RegisterEvents();

	typedef void (*ProbeHandlerFn)(TESObjectREFR*, void*);

	//sentinel handler at extreme priority - while it is still first in line for the
	//event, no external handler is registered and hooks can skip their bookkeeping
	struct ListenerProbe {
		const char* eventName;
		const char* handlerName;
		ProbeHandlerFn handler;
		volatile bool hasListeners = true;
		bool installed = false;
		UInt32 refreshCounter = 0;

		bool Install();
		bool Refresh(bool force);
	};
}
