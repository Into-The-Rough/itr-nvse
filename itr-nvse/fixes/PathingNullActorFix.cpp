//fixes null pathing dereferences hit while stale saved actor paths are rebuilt
//during load.

#include "PathingNullActorFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/SafeWrite.h"

#include "internal/globals.h"

namespace PathingNullActorFix
{
	static constexpr uint32_t kReturnAddr = 0x9E57CB;
	static constexpr uint32_t kSkipAddr = 0x9E5A49;   //loop continue
	static constexpr uint32_t kNiPointerDeref = 0x559450;
	static constexpr uint32_t kSetPathingFailedUseRequest = 0x9DE5A5;
	static constexpr uint32_t kSetPathingFailedSkipWarp = 0x9DE5FB;

	__declspec(naked) void Hook()
	{
		__asm
		{
			mov edx, [ebp - 0x78]    //ActorArray[i] local
			test edx, edx
			jz skip                  //null entry -> skip this iteration instead of deref
			mov eax, [edx]           //replay stolen vtable load
			jmp kReturnAddr
		skip:
			jmp kSkipAddr
		}
	}

	__declspec(naked) void HookSetPathingFailed()
	{
		__asm
		{
			push 1                    //abMoveFollowersEvenIfVisible
			push 0                    //abUseFollowerSpacing
			mov ecx, [ebp - 0x60]     //ActorMover*
			add ecx, 0x1C             //spPathingRequest
			call kNiPointerDeref
			test eax, eax
			jz skipWarp
			mov ecx, eax
			jmp kSetPathingFailedUseRequest
		skipWarp:
			add esp, 8                //discard Actor::WarpTo args pushed above
			jmp kSetPathingFailedSkipWarp
		}
	}

	//mov edx,[ebp-0x78] - mov eax,[edx]
	static constexpr UInt8 kExpectedHook[5] = { 0x8B, 0x55, 0x88, 0x8B, 0x02 };
	//push1 - push0 - mov ecx,[ebp-0x60] - add ecx,0x1C - call 0x559450
	static constexpr UInt8 kExpectedSetPathingFailed[15] = {
		0x6A, 0x01, 0x6A, 0x00, 0x8B, 0x4D, 0xA0, 0x83, 0xC1, 0x1C, 0xE8, 0xAD, 0xAE, 0xB7, 0xFF
	};

	void Init()
	{
		if (memcmp((void*)0x9E57C6, kExpectedHook, sizeof(kExpectedHook)) == 0)
			SafeWrite::WriteRelJump(0x9E57C6, (UInt32)Hook);
		else
			Log("PathingNullActorFix: 0x9E57C6 bytes changed, skipping");

		if (memcmp((void*)0x9DE594, kExpectedSetPathingFailed, sizeof(kExpectedSetPathingFailed)) == 0)
		{
			SafeWrite::WriteRelJump(0x9DE594, (UInt32)HookSetPathingFailed);
			SafeWrite::WriteNop(0x9DE599, 10);
		}
		else
			Log("PathingNullActorFix: 0x9DE594 bytes changed, skipping");
	}
}

