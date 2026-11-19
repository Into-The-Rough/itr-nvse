#include "RagdollPhantomWorldFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/HavokLayout.h"

#include "internal/globals.h"

namespace RagdollPhantomWorldFix {
	static Detours::CallDetour s_cast[2];

	template<UInt32 Site>
	static void __fastcall Hook_CastRay(HkpWorldObjectView* phantom, void* edx, const void* input, void* collector)
	{
		//both callers empty the collector before the call
		if (!phantom || !phantom->world) return;
		using Cast_t = void(__thiscall*)(HkpWorldObjectView*, const void*, void*);
		((Cast_t)s_cast[Site].GetOverwrittenAddr())(phantom, input, collector);
	}

	void Init()
	{
		s_cast[0].WriteRelCall(0xCA1D1D, Hook_CastRay<0>);
		s_cast[1].WriteRelCall(0xCA1E19, Hook_CastRay<1>);
	}
}
