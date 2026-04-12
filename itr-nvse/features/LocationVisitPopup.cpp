//shows popup when revisiting discovered locations with cooldown

#include "LocationVisitPopup.h"
#include "internal/CooldownTracker.h"
#include "internal/SafeWrite.h"
#include "internal/EngineFunctions.h"
#include <Windows.h>
#include <vector>
#include <cstring>

#include "internal/globals.h"
#include "internal/ScopedLock.h"

namespace LocationVisitPopup
{
	static DWORD g_cooldownMs = 5 * 60 * 1000;
	static DWORD g_leaveThresholdMs = 3000;
	static bool g_disableSound = false;
	static int g_muxDetected = -1;
	static DWORD g_mainThreadId = 0;

	struct PendingPopup
	{
		char name[260];
	};

	static CooldownTracker<256> s_tracker;
	static CRITICAL_SECTION s_stateLock;
	static bool s_lockInitialized = false;
	static std::vector<PendingPopup> s_pendingPopups;

	static void UpdateCooldowns() {
		s_tracker.UpdateCooldowns(GetTickCount(), g_leaveThresholdMs);
	}

	static int CheckMarker(UInt32 refID) {
		return s_tracker.Check(refID, GetTickCount(), g_cooldownMs);
	}

	static void MarkShown(UInt32 refID) {
		s_tracker.MarkShown(refID, GetTickCount());
	}

	typedef void(__cdecl* SetCustomQuestText_t)(const char*, const char*, int, int, int, int, const char*);
	static SetCustomQuestText_t SetCustomQuestText = (SetCustomQuestText_t)0x76B960;

	static void* GetHUDMainMenuTile() {
		void* hud = *(void**)0x11D96C0; //HUDMainMenu
		if (!hud) return nullptr;
		return *(void**)((UInt8*)hud + 0x04); //Menu::tile
	}

	static bool CheckMUXInstalled() {
		void* tile = GetHUDMainMenuTile();
		if (!tile) return false;
		int traitID = Engine::Tile_TextToTrait("_UXQV+Location");
		return Engine::Tile_GetValue(tile, traitID) != nullptr;
	}

	static void ShowPopupMUX(const char* name) {
		void* tile = GetHUDMainMenuTile();
		if (!tile) return;
		int locTrait = Engine::Tile_TextToTrait("_UXQV+Location");
		int alphaTrait = Engine::Tile_TextToTrait("_UXQV+Alpha");
		if (Engine::Tile_GetValue(tile, locTrait))
			Engine::Tile_SetString(tile, locTrait, name, true);
		if (Engine::Tile_GetValue(tile, alphaTrait))
			Engine::Tile_SetFloat(tile, alphaTrait, 255.0f, true);
	}

	static void ShowPopupVanilla(const char* name) {
		SetCustomQuestText("", name, 1, 0, -1, -1, g_disableSound ? "" : "UIPopUpQuestNew");
	}

	static void ShowPopup(const char* name)
	{
		if (!name || !name[0])
			return;

		if (g_muxDetected == -1)
			g_muxDetected = CheckMUXInstalled() ? 1 : 0;

		if (g_muxDetected)
			ShowPopupMUX(name);
		else
			ShowPopupVanilla(name);
	}

	static void QueuePopup(const char* name)
	{
		if (!name || !name[0] || !s_lockInitialized)
			return;

		PendingPopup popup{};
		strncpy_s(popup.name, sizeof(popup.name), name, _TRUNCATE);

		ScopedLock lock(&s_stateLock);
		constexpr size_t kMaxQueuedPopups = 32;
		if (s_pendingPopups.size() >= kMaxQueuedPopups)
			s_pendingPopups.erase(s_pendingPopups.begin());
		s_pendingPopups.push_back(popup);
	}

	void __cdecl OnInDiscoveredMarkerRadius(UInt32 markerRefID, void* markerDataPtr) {
		if (!markerDataPtr)
			return;

		const char* name = *(const char**)((UInt8*)markerDataPtr + 4);
		if (!name || !name[0])
			return;

		char nameCopy[260];
		strncpy_s(nameCopy, sizeof(nameCopy), name, _TRUNCATE);
		if (!nameCopy[0])
			return;

		{
			ScopedLock lock(&s_stateLock);
			UpdateCooldowns();
			if (CheckMarker(markerRefID) != 0)
				return;
			MarkShown(markerRefID);
		}

		//can run on AI worker threads, defer UI to main thread
		if (GetCurrentThreadId() == g_mainThreadId)
			ShowPopup(nameCopy);
		else
			QueuePopup(nameCopy);
	}

	__declspec(naked) void CheckDiscoveredMarkerHook() {
		static const UInt32 kRetnAddr = 0x7795E4;
		__asm {
			movzx edx, byte ptr[ebp - 0x90]      //replay stolen byte read (flag) so the return path is consistent
			test edx, edx
			jz skipCheck
			mov eax, [ebp - 0x8C]                //caller's marker entry pair
			test eax, eax
			jz skipCheck
			mov ecx, [eax]                       //ecx = markerDataPtr at +0x00
			test ecx, ecx
			jz skipCheck
			mov eax, [eax + 4]                   //eax = marker ref pointer at +0x04
			test eax, eax
			jz skipCheck
			pushad
			pushfd
			push ecx                             //cdecl arg2: markerDataPtr (name ptr lives at +4)
			push dword ptr[eax + 0x0C]           //cdecl arg1: markerRefID from TESObjectREFR::refID
			call OnInDiscoveredMarkerRadius
			add esp, 8                           //cdecl caller cleans 2 dwords
			popfd
			popad
			movzx edx, byte ptr[ebp - 0x90]      //re-load edx for the fallthrough; pop clobbered it
		skipCheck:
			jmp kRetnAddr
		}
	}

	static int s_cooldownSeconds = 300;

	void Init(int cooldownSeconds, bool disableSound) {
		g_mainThreadId = GetCurrentThreadId();
		if (!s_lockInitialized) {
			InitializeCriticalSection(&s_stateLock);
			s_lockInitialized = true;
		}
		s_cooldownSeconds = cooldownSeconds;
		g_cooldownMs = cooldownSeconds * 1000;
		g_disableSound = disableSound;
		SafeWrite::WriteRelJump(0x7795DD, (UInt32)CheckDiscoveredMarkerHook);
		SafeWrite::Write8(0x7795E2, 0x90);
		SafeWrite::Write8(0x7795E3, 0x90);
	}

	void UpdateSettings(int cooldownSeconds, bool disableSound)
	{
		g_cooldownMs = cooldownSeconds * 1000;
		g_disableSound = disableSound;
	}

	void Update()
	{
		if (!s_lockInitialized || GetCurrentThreadId() != g_mainThreadId)
			return;

		std::vector<PendingPopup> pending;
		{
			ScopedLock lock(&s_stateLock);
			pending.swap(s_pendingPopups);
		}

		for (const auto& popup : pending)
			ShowPopup(popup.name);
	}
}
