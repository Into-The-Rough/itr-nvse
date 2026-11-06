//crash at 0xCF2565 when a ragdoll ray is cast from a phantom that has no havok world

#include "RagdollPhantomWorldFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/HavokLayout.h"

#include "internal/globals.h"

namespace RagdollPhantomWorldFix {
	static Detours::CallDetour s_cast[2];

	template<UInt32 Site>
	static void __fastcall CastRay(HkpWorldObjectView* phantom, void* edx, const void* input, void* collector)
	{
		//both callers empty the collector before this thiscall with two stack arguments, ret 8
		if (!phantom || !phantom->world) return;
		typedef void(__thiscall * CastFn)(HkpWorldObjectView*, const void*, void*);
		((CastFn)s_cast[Site].GetOverwrittenAddr())(phantom, input, collector);
	}

	void Init()
	{
		if (s_cast[0].IsInstalled() || s_cast[1].IsInstalled()) return;
		//0xCA1D1D forward ray, 0xCA1E19 reverse ray
		if (!s_cast[0].WriteRelCall(0xCA1D1D, CastRay<0>)) {
			Log("RagdollPhantomWorldFix: 0xCA1D1D is not a call, skipping");
			return;
		}
		if (!s_cast[1].WriteRelCall(0xCA1E19, CastRay<1>)) {
			s_cast[0].Remove();
			Log("RagdollPhantomWorldFix: 0xCA1E19 is not a call, skipping");
		}
	}
}
