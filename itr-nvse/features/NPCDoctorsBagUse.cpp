//allows NPCs to use doctor's bags when they have crippled limbs during combat

#include "NPCDoctorsBagUse.h"
#include <Windows.h>
#include <cstdint>

namespace NPCDoctorsBagUse
{
	//config
	float g_fUseTimer = 15.0f;
	bool g_enabled = false;

	//NVRestoreLimbs "Restore Limb Condition" [MGEF:000FFCA0]
	constexpr uint32_t kFormID_RestoreLimbsEffect = 0xFFCA0;

	//limb condition AVs
	constexpr uint32_t kAVCode_PerceptionCondition = 0x19;
	constexpr uint32_t kAVCode_RightMobilityCondition = 0x1E;
	constexpr uint32_t kAVCode_IgnoreCrippledLimbs = 0x48;

	//addresses
	constexpr uint32_t kAddr_DrinkPotion = 0x8C1F80;
	constexpr uint32_t kAddr_GetPackageOwner = 0x97AE90;
	constexpr uint32_t kAddr_GetExtraData = 0x410220;

	//timer using real-time milliseconds
	constexpr int MAX_TIMERS = 64;
	uint32_t g_timerRefIDs[MAX_TIMERS] = {0};
	DWORD g_timerValues[MAX_TIMERS] = {0};
	int g_timerCount = 0;

	bool IsTimerReady(uint32_t refID)
	{
		DWORD now = GetTickCount();
		DWORD cooldownMs = (DWORD)(g_fUseTimer * 1000.0f);
		for (int i = 0; i < g_timerCount; i++)
		{
			if (g_timerRefIDs[i] == refID)
				return (now - g_timerValues[i]) >= cooldownMs;
		}
		return true;
	}

	void ResetTimer(uint32_t refID)
	{
		DWORD now = GetTickCount();
		for (int i = 0; i < g_timerCount; i++)
		{
			if (g_timerRefIDs[i] == refID)
			{
				g_timerValues[i] = now;
				return;
			}
		}
		if (g_timerCount < MAX_TIMERS)
		{
			g_timerRefIDs[g_timerCount] = refID;
			g_timerValues[g_timerCount] = now;
			g_timerCount++;
		}
	}

	typedef void* (__thiscall *GetPackageOwner_t)(void* controller);
	GetPackageOwner_t GetPackageOwner = (GetPackageOwner_t)kAddr_GetPackageOwner;

	uint32_t GetRefID(void* actor) { return *(uint32_t*)((char*)actor + 0x0C); }
	uint32_t GetTypeID(void* form) { return *(uint32_t*)((char*)form + 0x04); }

	float GetActorValue(void* actor, uint32_t avCode)
	{
		//ActorValueOwner at Actor+0xA4, vtable[3]=GetActorValue
		void* avOwner = (char*)actor + 0xA4;
		void** vtbl = *(void***)avOwner;
		if (!vtbl) return 100.0f;

		typedef float(__thiscall* GetAV_t)(void*, uint32_t);
		return ((GetAV_t)vtbl[3])(avOwner, avCode);
	}

	bool HasCrippledLimb(void* actor)
	{
		//check if actor ignores crippled limbs
		if (GetActorValue(actor, kAVCode_IgnoreCrippledLimbs) > 0.0f)
			return false;

		//check each limb condition AV
		for (uint32_t av = kAVCode_PerceptionCondition; av <= kAVCode_RightMobilityCondition; av++)
		{
			if (GetActorValue(actor, av) <= 0.0f)
				return true;
		}
		return false;
	}

	bool HasRestoreLimbsEffect(void* alchItem)
	{
		//tList head at AlchemyItem+0x40
		void* listHead = (char*)alchItem + 0x40;
		void* data = *(void**)listHead;
		void* next = *(void**)((char*)listHead + 4);

		while (true)
		{
			if (data)
			{
				void* setting = *(void**)((char*)data + 0x14);
				if (setting)
				{
					uint32_t settingRefID = *(uint32_t*)((char*)setting + 0x0C);
					if (settingRefID == kFormID_RestoreLimbsEffect)
						return true;
				}
			}
			if (!next) break;
			data = *(void**)next;
			next = *(void**)((char*)next + 4);
		}
		return false;
	}

	void* FindDoctorsBagInInventory(void* actor)
	{
		void* extraDataList = (char*)actor + 0x44;

		typedef void* (__thiscall* GetExtra_t)(void*, uint32_t);
		void* extraCC = ((GetExtra_t)kAddr_GetExtraData)(extraDataList, 0x15);
		if (!extraCC) return nullptr;

		void* data = *(void**)((char*)extraCC + 0x0C);
		if (!data) return nullptr;

		void* objList = *(void**)data;
		if (!objList) return nullptr;

		void* nodeData = *(void**)objList;
		void* nodeNext = *(void**)((char*)objList + 4);

		int count = 0;
		while (true)
		{
			if (nodeData)
			{
				void* form = *(void**)((char*)nodeData + 0x08);
				if (form && GetTypeID(form) == 0x2F) //AlchemyItem
				{
					if (HasRestoreLimbsEffect(form))
						return form;
				}
			}
			if (!nodeNext) break;
			nodeData = *(void**)nodeNext;
			nodeNext = *(void**)((char*)nodeNext + 4);
			if (++count > 500) break;
		}
		return nullptr;
	}

	void UseDoctorsBag(void* actor, void* item)
	{
		typedef bool(__thiscall* Fn)(void*, void*, void*, bool);
		((Fn)kAddr_DrinkPotion)(actor, item, nullptr, true);
	}

	void Check(void* combatState)
	{
		if (!g_enabled || !combatState)
			return;

		void* controller = *(void**)((char*)combatState + 0x1C4);
		if (!controller) return;

		void* actor = GetPackageOwner(controller);
		if (!actor) return;

		uint32_t refID = GetRefID(actor);

		if (!HasCrippledLimb(actor))
			return;

		if (!IsTimerReady(refID))
			return;

		void* doctorsBag = FindDoctorsBagInInventory(actor);
		if (!doctorsBag)
			return;

		UseDoctorsBag(actor, doctorsBag);
		ResetTimer(refID);
	}
}

void NPCDoctorsBagUse_Init(float useTimer)
{
	NPCDoctorsBagUse::g_fUseTimer = useTimer;
	NPCDoctorsBagUse::g_enabled = true;
}

void NPCDoctorsBagUse_Check(void* combatState)
{
	NPCDoctorsBagUse::Check(combatState);
}
