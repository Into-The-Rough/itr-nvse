//disables actor fade-out when entering doors
//hooks HighProcess::FadeOut - still sets up teleport state but zeros alpha immediately
//so the fade completes instantly without visual effect

#include "NoDoorFade.h"
#include <Windows.h>
#include <cstdint>

#include "internal/globals.h"

namespace NoDoorFade
{
	static bool g_enabled = false;

	typedef void (__thiscall* FadeOut_t)(void* process, void* actor, void* doorRef, bool teleport);
	FadeOut_t Original_FadeOut = (FadeOut_t)0x8FE960; //HighProcess::FadeOut

	void __fastcall Hook_FadeOut(void* process, void* edx, void* actor, void* doorRef, bool teleport)
	{
		//call original to set up fade state and teleport ref
		Original_FadeOut(process, actor, doorRef, teleport);

		//if enabled, immediately zero alpha so fade completes next frame
		if (g_enabled && teleport)
		{
			float* fadeAlpha = (float*)((uint8_t*)process + 0x3EC); //fFadeAlpha
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

	void SetEnabled(bool enabled)
	{
		g_enabled = enabled;
		Log("NoDoorFade %s", enabled ? "enabled" : "disabled");
	}

	void Init(bool enabled)
	{
		PatchCall(0x51895B, (uint32_t)Hook_FadeOut);
		g_enabled = enabled;
		Log("NoDoorFade initialized (enabled=%d)", enabled);
	}
}

void NoDoorFade_Init(bool enabled)
{
	NoDoorFade::Init(enabled);
}

void NoDoorFade_SetEnabled(bool enabled)
{
	NoDoorFade::SetEnabled(enabled);
}
