//allows sleeping in owned beds with consequences

#include "OwnedBeds.h"
#include "internal/NVSEMinimal.h"
#include "internal/EngineFunctions.h"

#include "internal/globals.h"

namespace OwnedBeds
{
	static bool g_enabled = false;

	static bool g_playerWarnedAboutBed = false;

	template <typename T_Ret = void, typename... Args>
	__forceinline T_Ret OBThisCall(UInt32 addr, void* thisObj, Args... args) {
		return reinterpret_cast<T_Ret(__thiscall*)(void*, Args...)>(addr)(thisObj, args...);
	}

	inline UInt8 GetFormType(void* form) {
		return *((UInt8*)form + 4);
	}

	typedef void* (__cdecl *_GetTopic)(UInt32 type, int index);
	static _GetTopic GetTopic = (_GetTopic)0x61A2D0;

	typedef void (__thiscall *_ProcessGreet)(void* process, void* actor, void* topic, bool forceSub, bool stop, bool queue, bool sayCallback);
	static _ProcessGreet ProcessGreet = (_ProcessGreet)0x8DBE30;

	static void SendAssaultAlarmToBedOwner(void* bedRef, void* owner) {
		void* player = *(void**)0x11DEA3C;
		void* processList = (void*)0x11E0E80;
		void* nearbyActor = nullptr;

		UInt8 formType = GetFormType(owner);

		if (formType == 8) { //kFormType_TESFaction
			nearbyActor = OBThisCall<void*>(0x970B30, processList, owner, true, true);
		} else {
			nearbyActor = OBThisCall<void*>(0x970A20, processList, owner, 0);
		}

		if (!nearbyActor || nearbyActor == player)
			return;

		bool hasLOS = false;
		bool a8 = false;
		SInt32 detectionLevel = OBThisCall<SInt32>(0x8A0D10,
			nearbyActor, true, player, &hasLOS, false, false, 0, &a8);

		if (detectionLevel <= 0)
			return;

		if (!g_playerWarnedAboutBed) {
			void* topic = GetTopic(4, 9); //DT_COMBAT
			if (topic) {
				void* process = Engine::Actor_GetProcess(nearbyActor);
				if (process) {
					ProcessGreet(process, nearbyActor, topic, false, false, true, false);
				}
			}
			g_playerWarnedAboutBed = true;
		} else {
			OBThisCall(0x8C0460, nearbyActor, player, false, 1); //AttackAlarm
		}
	}

	bool __fastcall IsAnOwnerHook(void* bedRef, void* edx, void* actor, bool checkFaction) {
		if (!g_enabled)
			return Engine::TESObjectREFR_IsAnOwner(bedRef, actor, checkFaction);

		bool isOwner = Engine::TESObjectREFR_IsAnOwner(bedRef, actor, checkFaction);

		if (!isOwner) {
			void* owner = Engine::TESObjectREFR_GetOwnerRawForm(bedRef);
			if (owner) {
				SendAssaultAlarmToBedOwner(bedRef, owner);
			}
			return true;
		}
		return true;
	}

	void SetEnabled(bool enabled) {
		g_enabled = enabled;
		Log("OwnedBeds %s", enabled ? "enabled" : "disabled");
	}

	void Init(bool enabled)
	{
		SafeWrite::WriteRelCall(0x509679, (UInt32)IsAnOwnerHook);
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
