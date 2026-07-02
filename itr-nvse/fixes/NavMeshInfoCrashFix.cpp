//fixes crash in NavMeshInfo::GetFlag10 (0x68F320) when called with
//a stale/invalid NavMeshInfo pointer. PathingLocation::ResolveNavMeshInfo
//checks (pNavMeshInfo != null) but stale pointers like 0x4 pass that check
//and crash on [this+8]. all 8 callers treat GetFlag10()==true as "skip this
//navmesh", so returning true for invalid pointers is safe.
//seen with TLD_Travelers caravans during Stewie's UpdateLowActors_TravelOrSleep.

#include "NavMeshInfoCrashFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"

#include "internal/globals.h"

namespace NavMeshInfoCrashFix
{
	static Detours::JumpDetour s_detour;

	//full replacement of GetFlag10, never falls through to the original
	__declspec(naked) void Hook()
	{
		__asm
		{
			cmp ecx, 0x10000
			jb invalid
			mov eax, [ecx + 8]   //this->uiFlags
			and eax, 0x10
			neg eax
			sbb eax, eax
			neg eax
			ret
		invalid:
			mov eax, 1           //flag "set" = callers skip this NavMeshInfo
			ret
		}
	}

	void Init()
	{
		//push ebp; mov ebp,esp; push ecx; mov [ebp-4],ecx = 7 bytes
		if (!s_detour.WriteRelJump(0x68F320, Hook, 7)) //NavMeshInfo::GetFlag10
			Log("NavMeshInfoCrashFix: GetFlag10 already patched, skipping");
	}
}

