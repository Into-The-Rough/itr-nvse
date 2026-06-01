//fixes CTD when GetLineOfSight is called on a disabled actor.
//GetLineOfSight at 0x59C990 calls Actor::LineOfSight which dereferences
//pCurrentProcess and havok body data without null-checking. those are
//torn down when an actor is disabled, so the call faults inside the
//engine. the wiki note for GetLOS warns: "When called on a disabled
//actor, it crashes the game".
//
//we hook the very first comparison in the function (the existing
//apCaster null check) and extend it to also bail out when either
//apCaster or apTarget has the disabled flag (TESForm flags & 0x800)
//set. on bail we jump to the function's existing early-exit label,
//which returns 1 with *adResult left at 0.0 (already zeroed by the
//prologue at 0x59C9D5).

#include "GetLineOfSightCrashFix.h"
#include "internal/NVSEMinimal.h"

#include "internal/globals.h"

namespace GetLineOfSightCrashFix
{
	//0x59C9D7: cmp dword ptr [ebx+8], 0   (4 bytes)
	//0x59C9DB: jz   0x59CE53              (6 bytes) - early-exit on null caster
	//0x59C9E1: mov  dword ptr [ebp-18h], 0 (continuation)
	//0x59CE53: mov  al, 1; epilogue       (early-exit label)
	static constexpr uint32_t kPatchAddr  = 0x59C9D7;
	static constexpr uint32_t kReturnAddr = 0x59C9E1; //continue with original code
	static constexpr uint32_t kBailAddr   = 0x59CE53; //return 1 with *adResult = 0.0

	__declspec(naked) void Hook()
	{
		__asm
		{
			mov edx, [ebx + 8]              //apCaster
			test edx, edx
			jz bail                          //null caster -> original behaviour
			test dword ptr [edx + 8], 0x800  //TESForm flags: disabled bit
			jnz bail
			mov edx, [ebx + 0x0C]           //apTarget
			test edx, edx
			jz cont                          //null target handled later by original
			test dword ptr [edx + 8], 0x800
			jnz bail
		cont:
			jmp kReturnAddr
		bail:
			jmp kBailAddr
		}
	}

	void Init()
	{
		//replace 4-byte cmp + 6-byte jz (10 bytes) with 5-byte jmp + 5 NOPs
		SafeWrite::WriteRelJump(kPatchAddr, (UInt32)Hook);
		SafeWrite::WriteNop(kPatchAddr + 5, 5);
	}
}
