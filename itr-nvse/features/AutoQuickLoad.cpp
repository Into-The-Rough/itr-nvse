//auto-loads quicksave when reaching start menu
//hooks PollControls to inject F9 keypress after a configurable delay

#include "AutoQuickLoad.h"
#include "internal/Detours.h"
#include "internal/settings.h"
#include "internal/GameGlobals.h"
#include "internal/globals.h"
#include <Windows.h>

namespace AutoQuickLoad
{
	static bool g_done = false;
	static DWORD g_startTime = 0;

	#ifndef kMenuType_Start
	#define kMenuType_Start 0x3F5
	#endif

	typedef void (__thiscall *_PollControls)(void*);
	static Detours::CallDetour s_pollControlsCall;

	static bool IsStartMenuVisible()
	{
		return IsMenuVisible(kMenuType_Start);
	}

	//hooked at 0x86E88C - injects F9 keypress AFTER PollControls reads hardware
	//so the game sees it as a real keypress when it checks GetUserAction(QuickLoad)
	void __fastcall PollControlsHook(void* tesMain, void* edx)
	{
		auto original = reinterpret_cast<_PollControls>(s_pollControlsCall.GetOverwrittenAddr());
		if (original)
			original(tesMain);

		if (g_done)
			return;

		if (!Settings::bAutoQuickLoad)
		{
			g_startTime = 0;
			return;
		}

		if (!g_startTime)
			return;

		if (!IsStartMenuVisible())
		{
			g_startTime = 0;
			return;
		}

		if ((GetTickCount() - g_startTime) < (DWORD)Settings::iAutoQuickLoadDelayMs)
			return;

		OSInputGlobalsSetKeyState(*g_inputGlobalsPtr, 0x43, 0x80); //DIK_F9
		g_done = true;
	}

	void InstallHook()
	{
		s_pollControlsCall.WriteRelCall(0x86E88C, PollControlsHook);
	}

	void Update()
	{
		if (g_done)
			return;

		if (!Settings::bAutoQuickLoad)
		{
			g_startTime = 0;
			return;
		}

		if (IsStartMenuVisible())
		{
			if (!g_startTime)
				g_startTime = GetTickCount();
		}
		else
		{
			//cancel the pending quickload if the player leaves the start menu
			//before the delay expires, then re-arm when it opens again
			g_startTime = 0;
		}
	}
}
