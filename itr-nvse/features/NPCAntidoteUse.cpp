//allows NPCs to use antidotes when poisoned during combat

#include "NPCAntidoteUse.h"
#include <Windows.h>
#include <unordered_map>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace NPCAntidoteUse
{
	//config
	float g_fCombatItemCureTimer = 10.0f;
	float g_fCureHealthThreshold = 25.0f; //don't cure if health below this (prioritize stimpak)

	//NVAntivenomEffect "Cure Animal Poison" [MGEF:000E2C6D]
	constexpr uint32_t kFormID_CurePoisonEffect = 0xE2C6D;

	//addresses
	constexpr uint32_t kAddr_UpdateLowCall = 0x997CE5;
	constexpr uint32_t kAddr_UpdateLow = 0x998CE0;
	constexpr uint32_t kAddr_DrinkPotion = 0x8C2780;
	constexpr uint32_t kAddr_GetPackageOwner = 0x97AE90;
	constexpr uint32_t kAddr_g_gameTime = 0x11AC3A0;

	//spell types
	enum { kSpellType_Poison = 5 };

	//minimal structures
	struct TESForm { void* vtbl; uint32_t typeID; uint32_t flags; uint32_t refID; };
	struct EffectSetting : TESForm {};
	struct EffectItem { uint32_t magnitude; uint32_t area; uint32_t duration; uint32_t range; uint32_t actorValue; EffectSetting* setting; };

	struct EffectNode { EffectItem* data; EffectNode* next; };
	struct EffectItemList { void* vtbl; EffectNode head; };

	struct MagicItem { void* vtbl; char pad[0x08]; EffectItemList list; };
	struct AlchemyItem : MagicItem { };

	struct ActiveEffect { void* vtbl; uint8_t pad04[0x44]; uint32_t spellType; };
	struct ActiveEffectNode { ActiveEffect* data; ActiveEffectNode* next; };
	struct ActiveEffectList { ActiveEffectNode head; };

	struct MagicTarget { void* vtbl; uint8_t pad[0x04]; ActiveEffectList* GetEffectList() { return *(ActiveEffectList**)((char*)this + 0x04); } };

	struct Actor : TESForm
	{
		uint8_t pad10[0x100 - 0x10];
		MagicTarget magicTarget; //0x100

		float GetHealthPercent() {
			typedef float(__thiscall* Fn)(Actor*);
			return ((Fn)0x8A4B30)(this) * 100.0f;
		}
	};

	struct ItemEntry { void* vtbl; TESForm* type; };
	struct ItemEntryNode { ItemEntry* data; ItemEntryNode* next; };
	struct ItemEntryList { ItemEntryNode head; };
	struct InventoryChanges { ItemEntryList itemList; };

	struct CombatController { uint8_t pad[0xBC]; Actor* packageOwner; };
	struct CombatState { uint8_t pad[0x1C4]; CombatController* pCombatController; };

	//timer tracking: refID -> last use game time
	std::unordered_map<uint32_t, float> g_lastCureTime;

	float GetGameTime()
	{
		return *(float*)kAddr_g_gameTime;
	}

	bool IsTimerReady(uint32_t refID)
	{
		auto it = g_lastCureTime.find(refID);
		if (it == g_lastCureTime.end())
			return true;
		return (GetGameTime() - it->second) >= g_fCombatItemCureTimer;
	}

	void ResetTimer(uint32_t refID)
	{
		g_lastCureTime[refID] = GetGameTime();
	}

	bool IsPoisoned(Actor* actor)
	{
		ActiveEffectList* effList = actor->magicTarget.GetEffectList();
		if (!effList) return false;

		ActiveEffectNode* node = &effList->head;
		while (node && node->data)
		{
			if (node->data->spellType == kSpellType_Poison)
				return true;
			node = node->next;
		}
		return false;
	}

	bool HasCurePoisonEffect(AlchemyItem* item)
	{
		EffectNode* node = &item->list.head;
		while (node && node->data)
		{
			if (node->data->setting && node->data->setting->refID == kFormID_CurePoisonEffect)
				return true;
			node = node->next;
		}
		return false;
	}

	AlchemyItem* FindAntidoteInInventory(Actor* actor)
	{
		//get inventory changes at actor+0x7C (TESObjectREFR offset)
		InventoryChanges* inv = *(InventoryChanges**)((char*)actor + 0x114);
		if (!inv) return nullptr;

		ItemEntryNode* node = &inv->itemList.head;
		while (node && node->data)
		{
			TESForm* form = node->data->type;
			if (form && form->typeID == 0x2F) //kFormType_AlchemyItem
			{
				AlchemyItem* alch = (AlchemyItem*)form;
				if (HasCurePoisonEffect(alch))
					return alch;
			}
			node = node->next;
		}
		return nullptr;
	}

	void DrinkPotion(Actor* actor, AlchemyItem* potion)
	{
		typedef void(__thiscall* Fn)(Actor*, AlchemyItem*, bool, bool);
		((Fn)kAddr_DrinkPotion)(actor, potion, false, true);
	}

	void CheckAndUseCure(CombatState* combatState)
	{
		if (!combatState || !combatState->pCombatController)
			return;

		Actor* actor = combatState->pCombatController->packageOwner;
		if (!actor)
			return;

		//skip if health is critical (let stimpak system handle it)
		if (actor->GetHealthPercent() < g_fCureHealthThreshold)
			return;

		//check if poisoned
		if (!IsPoisoned(actor))
			return;

		//check timer
		if (!IsTimerReady(actor->refID))
			return;

		//find antidote
		AlchemyItem* antidote = FindAntidoteInInventory(actor);
		if (!antidote)
			return;

		//use it
		DrinkPotion(actor, antidote);
		ResetTimer(actor->refID);
	}

	//hook
	static uint32_t s_originalUpdateLow = 0;

	void __fastcall Hook_UpdateLow(CombatState* combatState, void* edx)
	{
		//call original
		typedef void(__thiscall* Fn)(CombatState*);
		((Fn)s_originalUpdateLow)(combatState);

		//our antidote check
		CheckAndUseCure(combatState);
	}

	void WriteRelCall(uint32_t src, uint32_t dst)
	{
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		s_originalUpdateLow = *(uint32_t*)(src + 1) + src + 5; //read original target
		*(uint8_t*)src = 0xE8;
		*(uint32_t*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
	}

	void Init(float cureTimer, float healthThreshold)
	{
		g_fCombatItemCureTimer = cureTimer;
		g_fCureHealthThreshold = healthThreshold;
		WriteRelCall(kAddr_UpdateLowCall, (uint32_t)Hook_UpdateLow);
		Log("NPCAntidoteUse installed (timer=%.1f, healthThreshold=%.1f)", cureTimer, healthThreshold);
	}
}

void NPCAntidoteUse_Init()
{
	NPCAntidoteUse::Init(10.0f, 25.0f);
}
