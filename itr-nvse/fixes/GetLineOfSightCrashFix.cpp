//GetLineOfSight 0x59C990 faults on a disabled actor - Actor::LineOfSight derefs
//pCurrentProcess and havok body data, both torn down on disable. extend the existing
//apCaster null check to also bail on the disabled flag

#include "GetLineOfSightCrashFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/SafeWrite.h"

#include "internal/globals.h"

namespace GetLineOfSightCrashFix
{
	//0x59C9D7: cmp dword ptr [ebx+8], 0   (4 bytes)
	//0x59C9DB: jz   0x59CE53              (6 bytes) - early-exit on null caster
	//0x59C9E1: mov  dword ptr [ebp-18h], 0 (continuation)
	//0x59CE53: mov  al, 1 then epilogue   (early-exit label)
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

	//cmp [ebx+8],0 - jz 0x59CE53
	static constexpr UInt8 kExpected[10] = { 0x83, 0x7B, 0x08, 0x00, 0x0F, 0x84, 0x72, 0x04, 0x00, 0x00 };

	void Init()
	{
		if (memcmp((void*)kPatchAddr, kExpected, sizeof(kExpected)) != 0)
		{
			Log("GetLineOfSightCrashFix: 0x%X bytes changed, skipping", kPatchAddr);
			return;
		}

		//replace 4-byte cmp + 6-byte jz (10 bytes) with 5-byte jmp + 5 NOPs
		SafeWrite::WriteRelJump(kPatchAddr, (UInt32)Hook);
		SafeWrite::WriteNop(kPatchAddr + 5, 5);
	}
}
