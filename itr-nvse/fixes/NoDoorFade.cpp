//disables actor fade-out when entering doors
//hooks HighProcess::FadeOut - still sets up teleport state but zeros alpha immediately
//so the fade completes instantly without visual effect

#include "NoDoorFade.h"
#include <cstdint>

#include "internal/SafeWrite.h"
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

	void SetEnabled(bool enabled)
	{
		g_enabled = enabled;
	}

	void Init(bool enabled)
	{
		SafeWrite::WriteRelCall(0x51895B, (UInt32)Hook_FadeOut);
		g_enabled = enabled;
	}
}

