//allows sleeping in owned beds with consequences

#include "OwnedBeds.h"
#include <Windows.h>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace OwnedBeds
{
	static bool g_enabled = false;

	constexpr UInt32 kAddr_IsAnOwner = 0x5785E0;
	constexpr UInt32 kAddr_IsAnOwnerCall = 0x509679;
	constexpr UInt32 kAddr_ResolveOwnership = 0x567790;
	constexpr UInt32 kAddr_PlayerSingleton = 0x011DEA3C;
	constexpr UInt32 kAddr_ProcessListsSingleton = 0x11E0E80;
	constexpr UInt32 kAddr_AttackAlarm = 0x8C0460;
	constexpr UInt32 kAddr_GetActorRefInHigh = 0x970B30;
	constexpr UInt32 kAddr_GetActorRefInHigh_0 = 0x970A20;
	constexpr UInt32 kAddr_GetDetectionLevelAgainstActor = 0x8A0D10;
	constexpr UInt32 kAddr_GetCurrentProcess = 0x8D8520;
	constexpr UInt32 kAddr_GetTopic = 0x61A2D0;
	constexpr UInt32 kAddr_ProcessGreet = 0x8DBE30;
	constexpr UInt8 kFormType_TESFaction = 8;
	constexpr UInt32 DT_COMBAT = 4;

	static bool g_playerWarnedAboutBed = false;

	template <typename T_Ret = void, typename... Args>
	__forceinline T_Ret OBThisCall(UInt32 addr, void* thisObj, Args... args) {
		return reinterpret_cast<T_Ret(__thiscall*)(void*, Args...)>(addr)(thisObj, args...);
	}

	inline UInt8 GetFormType(void* form) {
		return *((UInt8*)form + 4);
	}

	typedef bool (__thiscall *_IsAnOwner)(void* thisObj, void* actor, bool checkFaction);
	static _IsAnOwner IsAnOwner = (_IsAnOwner)kAddr_IsAnOwner;

	typedef void* (__cdecl *_GetTopic)(UInt32 type, int index);
	static _GetTopic GetTopic = (_GetTopic)kAddr_GetTopic;

	typedef void* (__thiscall *_MobileGetProcess)(void* actor);
	static _MobileGetProcess MobileGetProcess = (_MobileGetProcess)kAddr_GetCurrentProcess;

	typedef void (__thiscall *_ProcessGreet)(void* process, void* actor, void* topic, bool forceSub, bool stop, bool queue, bool sayCallback);
	static _ProcessGreet ProcessGreet = (_ProcessGreet)kAddr_ProcessGreet;

	static void SendAssaultAlarmToBedOwner(void* bedRef, void* owner) {
		void* player = *(void**)kAddr_PlayerSingleton;
		void* processList = (void*)kAddr_ProcessListsSingleton;
		void* nearbyActor = nullptr;

		UInt8 formType = GetFormType(owner);

		if (formType == kFormType_TESFaction) {
			nearbyActor = OBThisCall<void*>(kAddr_GetActorRefInHigh, processList, owner, true, true);
		} else {
			nearbyActor = OBThisCall<void*>(kAddr_GetActorRefInHigh_0, processList, owner, 0);
		}

		if (!nearbyActor || nearbyActor == player)
			return;

		bool hasLOS = false;
		bool a8 = false;
		SInt32 detectionLevel = OBThisCall<SInt32>(kAddr_GetDetectionLevelAgainstActor,
			nearbyActor, true, player, &hasLOS, false, false, 0, &a8);

		if (detectionLevel <= 0)
			return;

		if (!g_playerWarnedAboutBed) {
			void* topic = GetTopic(DT_COMBAT, 9);
			if (topic) {
				void* process = MobileGetProcess(nearbyActor);
				if (process) {
					ProcessGreet(process, nearbyActor, topic, false, false, true, false);
				}
			}
			g_playerWarnedAboutBed = true;
		} else {
			OBThisCall(kAddr_AttackAlarm, nearbyActor, player, false, 1);
		}
	}

	bool __fastcall IsAnOwnerHook(void* bedRef, void* edx, void* actor, bool checkFaction) {
		//if disabled, just call original
		if (!g_enabled)
			return IsAnOwner(bedRef, actor, checkFaction);

		bool isOwner = IsAnOwner(bedRef, actor, checkFaction);

		if (!isOwner) {
			void* owner = OBThisCall<void*>(kAddr_ResolveOwnership, bedRef);
			if (owner) {
				SendAssaultAlarmToBedOwner(bedRef, owner);
			}
			return true;
		}
		return true;
	}

	void PatchWrite8(uint32_t addr, uint8_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)addr = data;
		VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
	}

	void PatchWrite32(uint32_t addr, uint32_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	void WriteRelCall(UInt32 addr, UInt32 target) {
		PatchWrite8(addr, 0xE8);
		PatchWrite32(addr + 1, target - addr - 5);
	}

	void SetEnabled(bool enabled) {
		g_enabled = enabled;
		Log("OwnedBeds %s", enabled ? "enabled" : "disabled");
	}

	void Init(bool enabled)
	{
		WriteRelCall(kAddr_IsAnOwnerCall, (UInt32)IsAnOwnerHook);
		g_enabled = enabled;
		Log("OwnedBeds initialized (enabled=%d)", enabled);
	}
}

void OwnedBeds_Init(bool enabled)
{
	OwnedBeds::Init(enabled);
}

void OwnedBeds_SetEnabled(bool enabled)
{
	OwnedBeds::SetEnabled(enabled);
}
