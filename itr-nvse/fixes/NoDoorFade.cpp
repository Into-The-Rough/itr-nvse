//disables actor fade-out when entering doors
//hooks HighProcess::FadeOut - still sets up teleport state but zeros alpha immediately
//so the fade completes instantly without visual effect

#include "NoDoorFade.h"
#include <Windows.h>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace NoDoorFade
{
	constexpr uint32_t kAddr_FadeOutCall = 0x51895B;
	constexpr uint32_t kAddr_FadeOut = 0x8FE960;

	//HighProcess offsets
	constexpr uint32_t kOffset_eFadeState = 0x3E8;
	constexpr uint32_t kOffset_fFadeAlpha = 0x3EC;

	typedef void (__thiscall* FadeOut_t)(void* process, void* actor, void* doorRef, bool teleport);
	FadeOut_t Original_FadeOut = (FadeOut_t)kAddr_FadeOut;

	void __fastcall Hook_FadeOut(void* process, void* edx, void* actor, void* doorRef, bool teleport)
	{
		//call original to set up fade state and teleport ref
		Original_FadeOut(process, actor, doorRef, teleport);

		//immediately zero alpha so fade completes next frame
		if (teleport)
		{
			float* fadeAlpha = (float*)((uint8_t*)process + kOffset_fFadeAlpha);
			*fadeAlpha = 0.0f;
		}
	}

	void PatchCall(uint32_t src, uint32_t dst)
	{
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
	}

	void Init()
	{
		PatchCall(kAddr_FadeOutCall, (uint32_t)Hook_FadeOut);
		Log("NoDoorFade: Hooked FadeOut for instant door transitions");
	}
}

void NoDoorFade_Init()
{
	NoDoorFade::Init();
}
