//blinds modded keybinds while the console is open
//vanilla controls already stop (InterfaceManager::Idle gates control translation on the
//console-active check 0x4A4040) but the raw key state arrays and NVSE's DIHookControl
//keep updating every poll, so mods reading them keep firing their binds. after each
//device poll we zero what mods read. console typing and scrolling ride the buffered
//pipeline (GetDeviceData) and are unaffected, mouse is left alone for ref-picking

#include "ConsoleInputSuppression.h"
#include "internal/Detours.h"
#include "internal/DIHookView.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/globals.h"
#include "internal/settings.h"

#include <cstring>

namespace ConsoleInputSuppression
{
	using PollInputDevices_t = void(__thiscall*)(void* inputGlobals);

	//per-frame device poll 0xA23010 called from the main input update 0x86F390
	constexpr UInt32 kCall_PollInputDevices = 0x86F39E;

	//current XINPUT_STATE.Gamepad stored by the poll, previous copy right after
	static UInt8* const kStoredGamepadCurr = (UInt8*)0x11F35AC;
	static UInt8* const kStoredGamepadPrev = (UInt8*)0x11F35BC;
	constexpr UInt32 kGamepadSize = 12; //wButtons, triggers, thumb axes

	static Detours::CallDetour s_pollCall;
	static PollInputDevices_t s_pollOriginal = nullptr;
	static DIHook::ControlView* s_diHookControl = nullptr;
	static bool s_suppressing = false;

	//GetControlState 0xA24660 hardcodes these scancodes while the console is open:
	//esc opens the pause menu, grave closes the console, sysrq screenshots
	static bool IsPreservedKey(UInt32 key)
	{
		return key == 0x01 || key == 0x29 || key == 0xB7;
	}

	static void SuppressStates(void* inputGlobals)
	{
		auto* view = reinterpret_cast<OSInputGlobalsView*>(inputGlobals);
		for (UInt32 i = 0; i < 256; ++i)
		{
			if (IsPreservedKey(i))
				continue;
			view->currKeyStates[i] = 0;
			view->lastKeyStates[i] = 0;
			if (s_diHookControl)
				s_diHookControl->ClearKeyState(i);
		}

		memset(kStoredGamepadCurr, 0, kGamepadSize);
		memset(kStoredGamepadPrev, 0, kGamepadSize);
	}

	static void __fastcall Hook_PollInputDevices(void* inputGlobals, void*)
	{
		if (s_pollOriginal)
			s_pollOriginal(inputGlobals);

		const bool active = Settings::bSuppressInputInConsole && inputGlobals && Engine::IsConsoleVisible();
		if (active != s_suppressing)
		{
			s_suppressing = active;
			Log("ConsoleInputSuppression: %s", active ? "suppressing" : "restored");
		}
		if (active)
			SuppressStates(inputGlobals);
	}

	void InstallHook()
	{
		if (s_pollOriginal)
			return;
		if (s_pollCall.WriteRelCall(kCall_PollInputDevices, Hook_PollInputDevices))
			s_pollOriginal = reinterpret_cast<PollInputDevices_t>(s_pollCall.GetOverwrittenAddr());
		else
			Log("ConsoleInputSuppression: failed to hook input device poll");
	}

	bool Init(void* nvseInterface)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
		if (nvse->isEditor) return false;

		auto* dataInterface = reinterpret_cast<NVSEDataInterface*>(nvse->QueryInterface(kInterface_Data));
		if (dataInterface)
			s_diHookControl = reinterpret_cast<DIHook::ControlView*>(dataInterface->GetSingleton(DIHook::kNVSEData_DIHookControl));
		if (!s_diHookControl)
			Log("ConsoleInputSuppression: DIHookControl unavailable, NVSE key states will not be suppressed");
		return true;
	}
}
