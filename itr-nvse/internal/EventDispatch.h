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
class TESForm;

namespace EventDispatch {
	void InitEventManager(void* nvseInterface);
	void RegisterEvents();

	//typed dispatch wrappers for translation units stuck on the old SDK headers,
	//which cannot include internal/NVSEPluginAPI.h without struct redefinitions
	bool DispatchConsoleCommand(const char* commandName, const char* fullCommand, TESObjectREFR* calleeRef);
	//ShowOff pre-activate gate for inventory refs - true means activation may proceed
	bool DispatchShowOffPreActivate(TESObjectREFR* player, TESForm* baseForm, TESObjectREFR* invRef);

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
