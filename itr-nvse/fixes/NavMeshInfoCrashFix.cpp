//fixes crash in NavMeshInfo::GetFlag10 (0x68F320) when called with
//a stale/invalid NavMeshInfo pointer. PathingLocation::ResolveNavMeshInfo
//checks (pNavMeshInfo != null) but stale pointers like 0x4 pass that check
//and crash on [this+8]. all 8 callers treat GetFlag10()==true as "skip this
//navmesh", so returning true for invalid pointers is safe.
//seen with TLD_Travelers caravans during Stewie's UpdateLowActors_TravelOrSleep.

#include "NavMeshInfoCrashFix.h"
#include "internal/NVSEMinimal.h"

#include "internal/globals.h"

namespace NavMeshInfoCrashFix
{
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
		SafeWrite::WriteRelJump(0x68F320, (UInt32)Hook); //NavMeshInfo::GetFlag10
		Log("NavMeshInfoCrashFix: replaced GetFlag10 at 0x68F320");
	}
}

