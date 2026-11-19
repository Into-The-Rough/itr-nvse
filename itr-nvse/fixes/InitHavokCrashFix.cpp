//guard pHkWorldObject->GethkWorld() vtable[0x94] against freed/reused vtables during cell transition

#include "InitHavokCrashFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/SafeWrite.h"

#include "internal/globals.h"

namespace InitHavokCrashFix {
	//0x576AB3: mov eax, [edx+0x94]  (6 bytes)
	//0x576AB9: call eax
	//0x576AD5: recovery path (AddObjectsAndMoveController)
	static constexpr UInt32 kPatchAddr = 0x576AB3;
	static constexpr UInt32 kReturnAddr = 0x576AB9; //call eax
	static constexpr UInt32 kSkipAddr = 0x576AD5; //null pHkWorldObject path

	__declspec(naked) void Hook()
	{
		__asm
		{
			mov eax, [edx + 0x94] //original: read GethkWorld from vtable
			test eax, eax
			jz skip
			jmp kReturnAddr //proceed to call eax
		skip:
			jmp kSkipAddr //stale object, re-init physics
		}
	}

	//mov eax,[edx+0x94]
	static constexpr UInt8 kExpected[6] = {0x8B, 0x82, 0x94, 0x00, 0x00, 0x00};

	void Init()
	{
		if (memcmp((void*)kPatchAddr, kExpected, sizeof(kExpected)) != 0) {
			Log("InitHavokCrashFix: 0x%X bytes changed, skipping", kPatchAddr);
			return;
		}

		SafeWrite::WriteRelJump(kPatchAddr, (UInt32)Hook);
		//nop the 6th byte (original instruction was 6 bytes, jmp is 5)
		SafeWrite::Write8(kPatchAddr + 5, 0x90);
	}
}
