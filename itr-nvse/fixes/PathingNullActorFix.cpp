//fixes null actor dereference in DetailedActorPathHandler avoidance loop
//the ActorArray can contain null entries from unloaded actors, but the
//loop only checks (actor != self) and never checks for null before
//dereferencing the vtable. 13+ crashes all at 0x9E57C9.

#include "PathingNullActorFix.h"
#include "internal/NVSEMinimal.h"

#include "internal/globals.h"

namespace PathingNullActorFix
{
	static constexpr uint32_t kReturnAddr = 0x9E57CB;
	static constexpr uint32_t kSkipAddr = 0x9E5A49;   //loop continue

	__declspec(naked) void Hook()
	{
		__asm
		{
			mov edx, [ebp - 0x78]
			test edx, edx
			jz skip
			mov eax, [edx]
			jmp kReturnAddr
		skip:
			jmp kSkipAddr
		}
	}

	void Init()
	{
		SafeWrite::WriteRelJump(0x9E57C6, (UInt32)Hook);
	}
}

