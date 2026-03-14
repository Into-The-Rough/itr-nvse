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

	//hooked at 0x86E88C - injects F9 keypress AFTER PollControls reads hardware
	//so the game sees it as a real keypress when it checks GetUserAction(QuickLoad)
	void __fastcall PollControlsHook(void* tesMain, void* edx)
	{
		PollControls(tesMain);
		if (g_done == false && g_startTime)
		{
			if ((GetTickCount() - g_startTime) >= (DWORD)Settings::iAutoQuickLoadDelayMs)
			{
				//DIK_F9=0x43, currKeyStates at +0x18F8
				auto input = *(UInt8**)0x11F35CC;
				if (input) input[0x18F8 + 0x43] = 0x80;
				g_done = true;
				Log("AutoQuickLoad: injected F9 (after %dms)", GetTickCount() - g_startTime);
			}
		}
	}

	void InstallHook()
	{
		SafeWrite::WriteRelCall(0x86E88C, (UInt32)PollControlsHook);
		Log("AutoQuickLoad: hooked PollControls, delay=%dms", Settings::iAutoQuickLoadDelayMs);
	}

	void Update()
	{
		if (!Settings::bAutoQuickLoad || g_done || g_startTime)
			return;
		if (g_MenuVisibilityArray[kMenuType_Start])
		{
			g_startTime = GetTickCount();
			Log("AutoQuickLoad: start menu detected, loading in %dms", Settings::iAutoQuickLoadDelayMs);
		}
	}
}

void AutoQuickLoad_InstallHook()
{
	AutoQuickLoad::InstallHook();
}

void AutoQuickLoad_Update()
{
	AutoQuickLoad::Update();
}
