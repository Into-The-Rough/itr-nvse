//fixes null dereference in BuildFollowerListRecursive (0x973640)
//the function calls Actor::GetCurrentPackage 3 times on the same actor.
//the first call is null-checked, but the third (for the ESCORT type check
//at 0x9736AA) is not. if the package becomes null between calls,
//GetPackType dereferences null and crashes at 0x41CA9A.

#include "DetectionFollowerCrashFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/SafeWrite.h"

#include "internal/globals.h"

namespace DetectionFollowerCrashFix
{
	static constexpr uint32_t kReturnAddr = 0x9736B6; //cmp eax, 7
	static constexpr uint32_t kSkipAddr = 0x9736F7;   //loop continue

	__declspec(naked) void Hook()
	{
		__asm
		{
			test eax, eax
			jz skip
			movsx eax, byte ptr [eax + 0x20] //inline GetPackType
			jmp kReturnAddr
		skip:
			jmp kSkipAddr
		}
	}

	//mov ecx,eax - call Actor::GetCurrentPackage->GetPackType (0x41CA90)
	static constexpr UInt8 kExpected[7] = { 0x8B, 0xC8, 0xE8, 0xDA, 0x93, 0xAA, 0xFF };

	void Init()
	{
		if (memcmp((void*)0x9736AF, kExpected, sizeof(kExpected)) != 0)
		{
			Log("DetectionFollowerCrashFix: 0x9736AF bytes changed, skipping");
			return;
		}

		SafeWrite::WriteRelJump(0x9736AF, (UInt32)Hook);
		SafeWrite::Write8(0x9736B4, 0x90);
		SafeWrite::Write8(0x9736B5, 0x90);
	}
}

