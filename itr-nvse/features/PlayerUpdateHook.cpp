//QuickDrop & Quick180 (single combined hook)

#include "PlayerUpdateHook.h"
#include "internal/SafeWrite.h"
#include "internal/EngineFunctions.h"

#include "internal/globals.h"

namespace PlayerUpdateHook
{
	constexpr float PI = 3.14159265358979323846f;
	constexpr uint32_t kAddr_PlayerUpdateCall = 0x940C78;

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

	bool GetControlState(void* input, uint32_t controlCode, KeyState state) {
		return Engine::OSInputGlobals_GetControlState(input, controlCode, (UInt8)state);
	}

	void* GetEquippedWeapon(void* actor) {
		return Engine::Actor_GetEquippedWeapon(actor);
	}

	void TryDropWeapon(void* actor) {
		Engine::Actor_TryDropWeapon(actor);
	}

	void RotatePlayer180(void* player) {
		float* rotZ = (float*)((uint8_t*)player + 0x2C); //rotZ
		*rotZ += PI;
		while (*rotZ > PI) *rotZ -= 2.0f * PI;
		while (*rotZ < -PI) *rotZ += 2.0f * PI;
	}

	void __fastcall PlayerUpdate_Hook(void* player, void* edx, float timeDelta) {
		((void(__thiscall*)(void*, float))g_originalCallTarget)(player, timeDelta);

		void* osGlobals = *(void**)0x11DEA0C;
		void* inputGlobals = *(void**)0x11F35CC;

		if (!osGlobals) {
			g_quickDropLastPressed = false;
			g_quick180LastPressed = false;
			return;
		}

		HWND gameWindow = *(HWND*)((uint8_t*)osGlobals + 0x08); //hWnd
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
		g_originalCallTarget = SafeWrite::GetRelJumpTarget(kAddr_PlayerUpdateCall);
		SafeWrite::Write32(kAddr_PlayerUpdateCall + 1, (UInt32)PlayerUpdate_Hook - kAddr_PlayerUpdateCall - 5);
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

void PlayerUpdateHook_UpdateSettings(int quickDropModKey, int quickDropControlID,
                                     int quick180ModKey, int quick180ControlID)
{
	PlayerUpdateHook::g_quickDropModifierKey = quickDropModKey;
	PlayerUpdateHook::g_quickDropControlID = quickDropControlID;
	PlayerUpdateHook::g_quick180ModifierKey = quick180ModKey;
	PlayerUpdateHook::g_quick180ControlID = quick180ControlID;
}
