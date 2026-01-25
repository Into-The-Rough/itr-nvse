//QuickDrop & Quick180 (single combined hook)

#include "PlayerUpdateHook.h"
#include <Windows.h>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace PlayerUpdateHook
{
	constexpr float PI = 3.14159265358979323846f;
	constexpr uint32_t kAddr_OSGlobals = 0x11DEA0C;
	constexpr uint32_t kAddr_OSInputGlobals = 0x11F35CC;
	constexpr uint32_t kAddr_GetControlState = 0xA24660;
	constexpr uint32_t kAddr_PlayerUpdateCall = 0x940C78;
	constexpr uint32_t kAddr_TryDropWeapon = 0x89F580;
	constexpr uint32_t kAddr_GetEquippedWeapon = 0x8A1710;
	constexpr uint32_t kOffset_OSGlobals_Window = 0x08;
	constexpr uint32_t kOffset_Actor_RotZ = 0x2C;

	enum KeyState { isHeld, isPressed, isDepressed, isChanged };

	static bool g_quickDropEnabled = false;
	static int g_quickDropModifierKey = 0;
	static int g_quickDropControlID = 0;
	static bool g_quick180Enabled = false;
	static int g_quick180ModifierKey = 0;
	static int g_quick180ControlID = 0;

	bool g_quickDropLastPressed = false;
	bool g_quick180LastPressed = false;
	uint32_t g_originalCallTarget = 0;

	void PatchWrite32(uint32_t addr, uint32_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	void PatchCall(uint32_t jumpSrc, uint32_t jumpTgt) {
		PatchWrite32(jumpSrc + 1, jumpTgt - jumpSrc - 5);
	}

	uint32_t ReadCallTarget(uint32_t jumpSrc) {
		return *(uint32_t*)(jumpSrc + 1) + jumpSrc + 5;
	}

	bool GetControlState(void* input, uint32_t controlCode, KeyState state) {
		return ((bool(__thiscall*)(void*, uint32_t, KeyState))kAddr_GetControlState)(input, controlCode, state);
	}

	void* GetEquippedWeapon(void* actor) {
		return ((void*(__thiscall*)(void*))kAddr_GetEquippedWeapon)(actor);
	}

	void TryDropWeapon(void* actor) {
		((void(__thiscall*)(void*))kAddr_TryDropWeapon)(actor);
	}

	void RotatePlayer180(void* player) {
		float* rotZ = (float*)((uint8_t*)player + kOffset_Actor_RotZ);
		*rotZ += PI;
		while (*rotZ > PI) *rotZ -= 2.0f * PI;
		while (*rotZ < -PI) *rotZ += 2.0f * PI;
	}

	void __fastcall PlayerUpdate_Hook(void* player, void* edx, float timeDelta) {
		((void(__thiscall*)(void*, float))g_originalCallTarget)(player, timeDelta);

		void* osGlobals = *(void**)kAddr_OSGlobals;
		void* inputGlobals = *(void**)kAddr_OSInputGlobals;

		if (!osGlobals) {
			g_quickDropLastPressed = false;
			g_quick180LastPressed = false;
			return;
		}

		HWND gameWindow = *(HWND*)((uint8_t*)osGlobals + kOffset_OSGlobals_Window);
		if (GetForegroundWindow() != gameWindow) {
			g_quickDropLastPressed = false;
			g_quick180LastPressed = false;
			return;
		}

		//QuickDrop
		if (g_quickDropEnabled) {
			bool modifierHeld = (g_quickDropModifierKey == 0) || ((GetAsyncKeyState(g_quickDropModifierKey) & 0x8000) != 0);
			bool controlPressed = GetControlState(inputGlobals, g_quickDropControlID, isPressed);
			if (controlPressed && !g_quickDropLastPressed && modifierHeld) {
				if (GetEquippedWeapon(player)) {
					TryDropWeapon(player);
				}
			}
			g_quickDropLastPressed = controlPressed;
		}

		//Quick180
		if (g_quick180Enabled) {
			bool modifierHeld = (g_quick180ModifierKey == 0) || ((GetAsyncKeyState(g_quick180ModifierKey) & 0x8000) != 0);
			bool controlPressed = GetControlState(inputGlobals, g_quick180ControlID, isPressed);
			if (controlPressed && !g_quick180LastPressed && modifierHeld) {
				RotatePlayer180(player);
			}
			g_quick180LastPressed = controlPressed;
		}
	}

	void Init() {
		g_originalCallTarget = ReadCallTarget(kAddr_PlayerUpdateCall);
		PatchCall(kAddr_PlayerUpdateCall, (uint32_t)PlayerUpdate_Hook);
		Log("PlayerUpdateHook installed (chaining to 0x%08X)", g_originalCallTarget);
	}
}

void PlayerUpdateHook_Init(bool quickDrop, int quickDropModKey, int quickDropControlID,
                           bool quick180, int quick180ModKey, int quick180ControlID)
{
	PlayerUpdateHook::g_quickDropEnabled = quickDrop;
	PlayerUpdateHook::g_quickDropModifierKey = quickDropModKey;
	PlayerUpdateHook::g_quickDropControlID = quickDropControlID;
	PlayerUpdateHook::g_quick180Enabled = quick180;
	PlayerUpdateHook::g_quick180ModifierKey = quick180ModKey;
	PlayerUpdateHook::g_quick180ControlID = quick180ControlID;
	PlayerUpdateHook::Init();
}
