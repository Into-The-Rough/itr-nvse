//shows popup when revisiting discovered locations with cooldown

#include "LocationVisitPopup.h"
#include "internal/CooldownTracker.h"
#include "internal/SafeWrite.h"
#include <Windows.h>
#include <vector>
#include <cstring>

extern void Log(const char* fmt, ...);

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

	class ScopedLock
	{
		CRITICAL_SECTION* cs;
	public:
		explicit ScopedLock(CRITICAL_SECTION* c) : cs(c) { EnterCriticalSection(cs); }
		~ScopedLock() { LeaveCriticalSection(cs); }
		ScopedLock(const ScopedLock&) = delete;
		ScopedLock& operator=(const ScopedLock&) = delete;
	};

	static CooldownTracker<256> s_tracker;
	static CRITICAL_SECTION s_stateLock;
	static bool s_lockInitialized = false;
	static std::vector<PendingPopup> s_pendingPopups;

	template <typename T_Ret = void, typename ...Args>
	__forceinline T_Ret LVPThisCall(UInt32 _addr, void* _this, Args ...args) {
		return ((T_Ret(__thiscall*)(void*, Args...))_addr)(_this, std::forward<Args>(args)...);
	}

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
		__try
		{
			void** g_interfaceManager = (void**)0x11D8A80;
			if (!*g_interfaceManager) return nullptr;
			void* hudMenu = *(void**)((UInt8*)*g_interfaceManager + 0x64);
			if (!hudMenu) return nullptr;
			return *(void**)((UInt8*)hudMenu + 0x30);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
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

		{
			ScopedLock lock(&s_stateLock);
			UpdateCooldowns();
			if (CheckMarker(markerRefID) != 0)
				return;
			MarkShown(markerRefID);
		}

		// This hook can run on AI worker threads; defer UI access to main thread.
		if (GetCurrentThreadId() == g_mainThreadId)
			ShowPopup(name);
		else
			QueuePopup(name);
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
			pushfd
			push ecx
			push dword ptr[eax + 0x0C]
			call OnInDiscoveredMarkerRadius
			add esp, 8
			popfd
			popad
			movzx edx, byte ptr[ebp - 0x90]
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
		Log("LocationVisitPopup installed (cooldown=%ds, sound=%s)",
			cooldownSeconds, disableSound ? "off" : "on");
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

void LocationVisitPopup_Init(int cooldownSeconds, bool disableSound)
{
	LocationVisitPopup::Init(cooldownSeconds, disableSound);
}

void LocationVisitPopup_UpdateSettings(int cooldownSeconds, bool disableSound)
{
	LocationVisitPopup::g_cooldownMs = cooldownSeconds * 1000;
	LocationVisitPopup::g_disableSound = disableSound;
}

void LocationVisitPopup_Update()
{
	LocationVisitPopup::Update();
}
