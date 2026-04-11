//auto-loads quicksave when reaching start menu
//hooks PollControls to inject F9 keypress after a configurable delay

#include "AutoQuickLoad.h"
#include "internal/SafeWrite.h"
#include "internal/settings.h"
#include "internal/globals.h"
#include <Windows.h>

namespace AutoQuickLoad
{
	static bool g_done = false;
	static DWORD g_startTime = 0;
	static UInt8* g_MenuVisibilityArray = (UInt8*)0x011F308F;

	#ifndef kMenuType_Start
	#define kMenuType_Start 0x3F5
	#endif

	typedef void (__thiscall *_PollControls)(void*);
	static const _PollControls PollControls = (_PollControls)0x86F390;

	static bool IsStartMenuVisible()
	{
		return g_MenuVisibilityArray && g_MenuVisibilityArray[kMenuType_Start] != 0;
	}

	//hooked at 0x86E88C - injects F9 keypress AFTER PollControls reads hardware
	//so the game sees it as a real keypress when it checks GetUserAction(QuickLoad)
	void __fastcall PollControlsHook(void* tesMain, void* edx)
	{
		PollControls(tesMain);
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

		//DIK_F9=0x43, currKeyStates at +0x18F8
		auto input = *(UInt8**)0x11F35CC;
		if (input) input[0x18F8 + 0x43] = 0x80;
		g_done = true;
	}

	void InstallHook()
	{
		SafeWrite::WriteRelCall(0x86E88C, (UInt32)PollControlsHook);
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
