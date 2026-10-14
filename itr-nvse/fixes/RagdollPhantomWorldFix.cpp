#include "RagdollPhantomWorldFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/layout/Havok.h"
#include "internal/globals.h"

namespace RagdollPhantomWorldFix
{
	static Detours::CallDetour s_cast[2];
	static volatile LONG s_skipped[2] = {};
	static unsigned s_reports = 0;
	constexpr unsigned kReportLimit = 8;

	template<unsigned Site>
	static void __fastcall CastRay(HkpWorldObjectView* phantom, void*, const void* input, void* collector)
	{
		//both callers empty the collector before this thiscall with two stack arguments, ret 8
		if (!phantom || !phantom->world) {
			InterlockedIncrement(&s_skipped[Site]);
			return;
		}
		using Cast = void(__thiscall*)(HkpWorldObjectView*, const void*, void*);
		((Cast)s_cast[Site].GetOverwrittenAddr())(phantom, input, collector);
	}

	void Init()
	{
		if (s_cast[0].IsInstalled() || s_cast[1].IsInstalled()) return;
		if (*(UInt8*)0xCA1D1D != 0xE8 || *(UInt8*)0xCA1E19 != 0xE8) {
			Log("RagdollPhantomWorldFix: ray call sites changed, skipping");
			return;
		}
		if (!s_cast[0].WriteRelCall(0xCA1D1D, CastRay<0>)) {
			Log("RagdollPhantomWorldFix: forward hook failed");
			return;
		}
		if (!s_cast[1].WriteRelCall(0xCA1E19, CastRay<1>)) {
			bool removed = s_cast[0].Remove();
			Log("RagdollPhantomWorldFix: reverse hook failed, forward rollback=%u", removed);
			return;
		}
		Log("RagdollPhantomWorldFix: installed forward=0xCA1D1D reverse=0xCA1E19 originals=%08X/%08X",
			s_cast[0].GetOverwrittenAddr(), s_cast[1].GetOverwrittenAddr());
	}

	void Update()
	{
		if (!s_cast[0].IsInstalled() && !s_cast[1].IsInstalled()) return;
		LONG forward = InterlockedExchange(&s_skipped[0], 0);
		LONG reverse = InterlockedExchange(&s_skipped[1], 0);
		if ((!forward && !reverse) || s_reports >= kReportLimit) return;
		Log("RagdollPhantomWorldFix: skipped detached phantom queries forward=%ld reverse=%ld report=%u/%u",
			forward, reverse, ++s_reports, kReportLimit);
	}
}
