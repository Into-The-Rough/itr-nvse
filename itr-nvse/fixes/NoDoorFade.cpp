//disables actor fade-out when entering doors
//hooks HighProcess::FadeOut - still sets up teleport state but zeros alpha immediately
//so the fade completes instantly without visual effect

#include "NoDoorFade.h"
#include <cstdint>

#include "internal/Detours.h"
#include "internal/globals.h"

namespace NoDoorFade
{
	static bool g_enabled = false;

	typedef void (__thiscall* FadeOut_t)(void* process, void* actor, void* doorRef, bool teleport);
	static Detours::CallDetour s_fadeOutCall;

	void __fastcall Hook_FadeOut(void* process, void* edx, void* actor, void* doorRef, bool teleport)
	{
		auto original = reinterpret_cast<FadeOut_t>(s_fadeOutCall.GetOverwrittenAddr());
		if (original)
			original(process, actor, doorRef, teleport);

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
		s_fadeOutCall.WriteRelCall(0x51895B, Hook_FadeOut);
		g_enabled = enabled;
	}
}

