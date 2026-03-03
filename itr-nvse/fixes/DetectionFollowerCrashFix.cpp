//fixes null dereference in BuildFollowerListRecursive (0x973640)
//the function calls Actor::GetCurrentPackage 3 times on the same actor.
//the first call is null-checked, but the third (for the ESCORT type check
//at 0x9736AA) is not. if the package becomes null between calls,
//GetPackType dereferences null and crashes at 0x41CA9A.

#include "DetectionFollowerCrashFix.h"
#include "internal/NVSEMinimal.h"

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

	void Init()
	{
		SafeWrite::WriteRelJump(0x9736AF, (UInt32)Hook);
		SafeWrite::Write8(0x9736B4, 0x90);
		SafeWrite::Write8(0x9736B5, 0x90);
		Log("DetectionFollowerCrashFix installed at 0x9736AF");
	}
}

void DetectionFollowerCrashFix_Init()
{
	DetectionFollowerCrashFix::Init();
}
