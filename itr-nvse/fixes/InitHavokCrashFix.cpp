//fixes crash in TESObjectREFR::InitHavok when a physics object's memory has
//been freed and reused during cell transition. the code calls
//pHkWorldObject->GethkWorld() via vtable[0x94], but the object's vtable
//points to reused memory (e.g. animation strings). [vtable+0x94] reads 0,
//call 0 crashes. the null case naturally falls through to AddObjects which
//re-initializes the physics correctly.

#include "InitHavokCrashFix.h"
#include "internal/NVSEMinimal.h"

#include "internal/globals.h"

namespace InitHavokCrashFix
{
	//0x576AB3: mov eax, [edx+0x94]  (6 bytes)
	//0x576AB9: call eax
	//0x576AD5: recovery path (AddObjectsAndMoveController)
	static constexpr uint32_t kPatchAddr = 0x576AB3;
	static constexpr uint32_t kReturnAddr = 0x576AB9;  //call eax
	static constexpr uint32_t kSkipAddr = 0x576AD5;    //null pHkWorldObject path

	__declspec(naked) void Hook()
	{
		__asm
		{
			mov eax, [edx + 0x94]   //original: read GethkWorld from vtable
			test eax, eax
			jz skip
			jmp kReturnAddr         //proceed to call eax
		skip:
			jmp kSkipAddr           //stale object, re-init physics
		}
	}

	void Init()
	{
		SafeWrite::WriteRelJump(kPatchAddr, (UInt32)Hook);
		//nop the 6th byte (original instruction was 6 bytes, jmp is 5)
		SafeWrite::Write8(kPatchAddr + 5, 0x90);
		Log("InitHavokCrashFix: patched GethkWorld call at 0x576AB3");
	}
}

void InitHavokCrashFix_Init()
{
	InitHavokCrashFix::Init();
}
