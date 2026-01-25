//shows popup when revisiting discovered locations with cooldown

#include "LocationVisitPopup.h"
#include <Windows.h>

extern void Log(const char* fmt, ...);

namespace LocationVisitPopup
{
	static DWORD g_cooldownMs = 5 * 60 * 1000;
	static bool g_disableSound = false;
	static int g_muxDetected = -1;

	static const UInt32 kMaxMarkers = 256;
	static UInt32 s_markers[kMaxMarkers];
	static DWORD s_popupTime[kMaxMarkers];
	static bool s_hasLeft[kMaxMarkers];
	static DWORD s_lastSeen[kMaxMarkers];
	static UInt32 s_count = 0;

	template <typename T_Ret = void, typename ...Args>
	__forceinline T_Ret LVPThisCall(UInt32 _addr, void* _this, Args ...args) {
		return ((T_Ret(__thiscall*)(void*, Args...))_addr)(_this, std::forward<Args>(args)...);
	}

	static void PatchWrite8(UInt32 addr, UInt8 data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)addr = data;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	static void PatchWrite32(UInt32 addr, UInt32 data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt32*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	static void WriteRelJump(UInt32 jumpSrc, UInt32 jumpTgt) {
		PatchWrite8(jumpSrc, 0xE9);
		PatchWrite32(jumpSrc + 1, jumpTgt - jumpSrc - 5);
	}

	static void UpdateCooldowns() {
		DWORD now = GetTickCount();
		for (UInt32 i = 0; i < s_count; i++)
			if (!s_hasLeft[i] && now - s_lastSeen[i] > 3000)
				s_hasLeft[i] = true;
	}

	static int CheckMarker(UInt32 refID) {
		DWORD now = GetTickCount();
		for (UInt32 i = 0; i < s_count; i++) {
			if (s_markers[i] == refID) {
				s_lastSeen[i] = now;
				if (!s_hasLeft[i])
					return 1;
				if (s_popupTime[i] && now - s_popupTime[i] < g_cooldownMs)
					return 1;
				return 0;
			}
		}
		if (s_count < kMaxMarkers) {
			s_markers[s_count] = refID;
			s_popupTime[s_count] = 0;
			s_hasLeft[s_count] = false;
			s_lastSeen[s_count] = now;
			s_count++;
		}
		return 1;
	}

	static void MarkShown(UInt32 refID) {
		DWORD now = GetTickCount();
		for (UInt32 i = 0; i < s_count; i++) {
			if (s_markers[i] == refID) {
				s_popupTime[i] = now;
				return;
			}
		}
	}

	typedef void(__cdecl* SetCustomQuestText_t)(const char*, const char*, int, int, int, int, const char*);
	static SetCustomQuestText_t SetCustomQuestText = (SetCustomQuestText_t)0x76B960;

	static void* GetHUDMainMenuTile() {
		void** g_interfaceManager = (void**)0x11D8A80;
		if (!*g_interfaceManager) return nullptr;
		void* hudMenu = *(void**)((UInt8*)*g_interfaceManager + 0x64);
		if (!hudMenu) return nullptr;
		return *(void**)((UInt8*)hudMenu + 0x30);
	}

	static bool CheckMUXInstalled() {
		void* tile = GetHUDMainMenuTile();
		if (!tile) return false;
		int traitIndex = LVPThisCall<int>(0xA0B110, tile, "_UXQV+Location");
		return traitIndex != -1;
	}

	static void ShowPopupMUX(const char* name) {
		void* tile = GetHUDMainMenuTile();
		if (!tile) return;
		int locTrait = LVPThisCall<int>(0xA0B110, tile, "_UXQV+Location");
		int alphaTrait = LVPThisCall<int>(0xA0B110, tile, "_UXQV+Alpha");
		if (locTrait != -1)
			LVPThisCall<void>(0xA0B270, tile, locTrait, name, true);
		if (alphaTrait != -1)
			LVPThisCall<void>(0xA0A280, tile, alphaTrait, 255.0f, true);
	}

	static void ShowPopupVanilla(const char* name) {
		SetCustomQuestText("", name, 1, 0, -1, -1, g_disableSound ? "" : "UIPopUpQuestNew");
	}

	void __cdecl OnInDiscoveredMarkerRadius(UInt32 markerRefID, void* markerDataPtr) {
		if (!markerDataPtr)
			return;
		UpdateCooldowns();
		if (CheckMarker(markerRefID) != 0)
			return;
		const char* name = *(const char**)((UInt8*)markerDataPtr + 4);
		if (name && name[0]) {
			MarkShown(markerRefID);
			if (g_muxDetected == -1)
				g_muxDetected = CheckMUXInstalled() ? 1 : 0;
			if (g_muxDetected)
				ShowPopupMUX(name);
			else
				ShowPopupVanilla(name);
		}
	}

	__declspec(naked) void CheckDiscoveredMarkerHook() {
		static const UInt32 kRetnAddr = 0x7795E4;
		__asm {
			movzx edx, byte ptr[ebp - 0x90]
			test edx, edx
			jz skipCheck
			mov eax, [ebp - 0x8C]
			test eax, eax
			jz skipCheck
			mov ecx, [eax]
			test ecx, ecx
			jz skipCheck
			mov eax, [eax + 4]
			test eax, eax
			jz skipCheck
			pushad
			push ecx
			push dword ptr[eax + 0x0C]
			call OnInDiscoveredMarkerRadius
			add esp, 8
			popad
			movzx edx, byte ptr[ebp - 0x90]
		skipCheck:
			jmp kRetnAddr
		}
	}

	static int s_cooldownSeconds = 300;

	void Init(int cooldownSeconds, bool disableSound) {
		s_cooldownSeconds = cooldownSeconds;
		g_cooldownMs = cooldownSeconds * 1000;
		g_disableSound = disableSound;
		WriteRelJump(0x7795DD, (UInt32)CheckDiscoveredMarkerHook);
		PatchWrite8(0x7795E2, 0x90);
		PatchWrite8(0x7795E3, 0x90);
		Log("LocationVisitPopup installed (cooldown=%ds, sound=%s)",
			cooldownSeconds, disableSound ? "off" : "on");
	}
}

void LocationVisitPopup_Init(int cooldownSeconds, bool disableSound)
{
	LocationVisitPopup::Init(cooldownSeconds, disableSound);
}
