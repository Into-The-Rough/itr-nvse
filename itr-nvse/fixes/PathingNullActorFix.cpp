//fixes null pathing dereferences hit while stale saved actor paths are rebuilt
//during load.

#include "PathingNullActorFix.h"
#include "internal/NVSEPluginAPI.h"

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

	void Init()
	{
		SafeWrite::WriteRelJump(0x9E57C6, (UInt32)Hook);
		SafeWrite::WriteRelJump(0x9DE594, (UInt32)HookSetPathingFailed);
		SafeWrite::WriteNop(0x9DE599, 10);
	}
}

