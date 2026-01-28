//NPCs evaluate shooting destructible objects (cars, barrels) near enemies as a tactical option

#include "AttackExplosiveObject.h"
#include <Windows.h>
#include <cstdint>
#include <cmath>

extern void Log(const char* fmt, ...);

namespace AttackExplosiveObject
{
	//settings
	static float g_fMinDamageRatio = 1.5f;
	static uint32_t g_iEvaluationCooldownMs = 2000;
	static bool g_bRequireLineOfSight = true;

	//9951824 decimal = 0x97DA50 hex
	constexpr uint32_t kAddr_CombatControllerUpdate = 0x97DA50;
	//9940624 decimal = 0x97AE90 hex
	constexpr uint32_t kAddr_GetPackageOwner = 0x97AE90;
	//4684032 decimal = 0x477900 hex
	constexpr uint32_t kAddr_ExplosionGetRadius = 0x477900;
	//6977776 decimal = 0x6A78F0 hex
	constexpr uint32_t kAddr_ExplosionGetDamage = 0x6A78F0;
	//9958096 decimal = 0x97F2B0 hex
	constexpr uint32_t kAddr_HasLineOfSight = 0x97F2B0;

	constexpr uint32_t kDestructibleFlag = 0x1000000;

	struct NiVector3 { float x, y, z; };
	struct tListNode { void* data; tListNode* next; };
	struct tList { tListNode first; };

	struct TESForm
	{
		void* vtbl;
		uint32_t unk04;
		uint32_t flags;
		uint32_t refID;
		uint8_t pad10[8];
		uint8_t typeID;
	};

	struct TESObjectREFR
	{
		uint8_t pad[0x30];
		float posX, posY, posZ;
		uint8_t pad3C[4];
		void* parentCell;
	};

	struct Actor : TESObjectREFR {};

	struct TESObjectCELL
	{
		uint8_t pad[0xAC];
		tList objectList;
	};

	struct DestructionStage
	{
		uint8_t healthPercent;
		uint8_t pad[3];
		void* pExplosion;
	};

	struct DestructibleData
	{
		uint32_t health;
		uint8_t numStages;
		uint8_t pad[3];
		DestructionStage** stagesArray;
	};

	struct BGSDestructibleObjectForm
	{
		void* vtbl;
		DestructibleData* pData;
	};

	struct CombatState
	{
		uint8_t pad[0x1C4];
		void* pCombatController;
	};

	struct CombatController
	{
		uint8_t pad[0xDC];
		void* pCurrentTarget;
	};

	typedef Actor* (__thiscall *GetPackageOwner_t)(void*);
	typedef float (__thiscall *GetExplosionRadius_t)(void*);
	typedef float (__thiscall *GetExplosionDamage_t)(void*);
	typedef bool (__thiscall *HasLineOfSight_t)(void*, TESObjectREFR*);

	GetPackageOwner_t GetPackageOwner = (GetPackageOwner_t)kAddr_GetPackageOwner;
	GetExplosionRadius_t GetExplosionRadius = (GetExplosionRadius_t)kAddr_ExplosionGetRadius;
	GetExplosionDamage_t GetExplosionDamage = (GetExplosionDamage_t)kAddr_ExplosionGetDamage;
	HasLineOfSight_t HasLineOfSight = (HasLineOfSight_t)kAddr_HasLineOfSight;

	static uint8_t g_originalBytes[6];
	static uint8_t g_trampoline[16];
	typedef void (__thiscall *CombatControllerUpdate_t)(void*);
	static CombatControllerUpdate_t Original = nullptr;

	//simple cooldown tracking per actor
	struct CooldownEntry { uint32_t refID; DWORD lastCheck; };
	static CooldownEntry g_cooldowns[64];
	static int g_cooldownCount = 0;

	bool CheckCooldown(uint32_t refID)
	{
		DWORD now = GetTickCount();
		for (int i = 0; i < g_cooldownCount; i++)
		{
			if (g_cooldowns[i].refID == refID)
			{
				if (now - g_cooldowns[i].lastCheck < g_iEvaluationCooldownMs)
					return false;
				g_cooldowns[i].lastCheck = now;
				return true;
			}
		}
		if (g_cooldownCount < 64)
		{
			g_cooldowns[g_cooldownCount].refID = refID;
			g_cooldowns[g_cooldownCount].lastCheck = now;
			g_cooldownCount++;
		}
		return true;
	}

	float GetDistance(TESObjectREFR* a, TESObjectREFR* b)
	{
		float dx = a->posX - b->posX;
		float dy = a->posY - b->posY;
		float dz = a->posZ - b->posZ;
		return sqrtf(dx*dx + dy*dy + dz*dz);
	}

	bool IsDestructible(TESObjectREFR* ref)
	{
		TESForm* form = (TESForm*)ref;
		return (form->flags & kDestructibleFlag) != 0;
	}

	//get the base form's destructible object form
	//baseForm is at offset 0x20 in TESObjectREFR
	BGSDestructibleObjectForm* GetDestructibleForm(TESObjectREFR* ref)
	{
		void* baseForm = *(void**)((uint8_t*)ref + 0x20);
		if (!baseForm) return nullptr;
		//BGSDestructibleObjectForm is component at varying offsets depending on form type
		//for static objects, try offset 0x48 (common for STAT with destructible)
		//this is simplified - real implementation would use RTTI or known offsets per type
		return (BGSDestructibleObjectForm*)((uint8_t*)baseForm + 0x48);
	}

	void* FindExplosionForDestructible(TESObjectREFR* ref)
	{
		BGSDestructibleObjectForm* destForm = GetDestructibleForm(ref);
		if (!destForm || !destForm->pData) return nullptr;
		DestructibleData* data = destForm->pData;
		if (!data->stagesArray || data->numStages == 0) return nullptr;

		//find first stage with an explosion
		for (uint8_t i = 0; i < data->numStages; i++)
		{
			DestructionStage* stage = data->stagesArray[i];
			if (stage && stage->pExplosion)
				return stage->pExplosion;
		}
		return nullptr;
	}

	struct ExplosiveTarget
	{
		TESObjectREFR* ref;
		void* explosion;
		float damage;
		float distFromTarget;
	};

	ExplosiveTarget* FindBestExplosiveNearTarget(Actor* actor, Actor* target, void* controller)
	{
		static ExplosiveTarget result;
		result.ref = nullptr;
		result.explosion = nullptr;
		result.damage = 0;
		result.distFromTarget = 0;

		TESObjectCELL* cell = (TESObjectCELL*)((TESObjectREFR*)actor)->parentCell;
		if (!cell) return nullptr;

		float bestScore = 0;

		for (tListNode* node = &cell->objectList.first; node; node = node->next)
		{
			TESObjectREFR* ref = (TESObjectREFR*)node->data;
			if (!ref) continue;
			if (ref == (TESObjectREFR*)actor || ref == (TESObjectREFR*)target) continue;

			if (!IsDestructible(ref)) continue;

			void* explosion = FindExplosionForDestructible(ref);
			if (!explosion) continue;

			float radius = GetExplosionRadius(explosion);
			float damage = GetExplosionDamage(explosion);
			if (radius <= 0 || damage <= 0) continue;

			float distFromTarget = GetDistance(ref, (TESObjectREFR*)target);
			if (distFromTarget > radius) continue; //target not in blast radius

			//check line of sight to the destructible
			if (g_bRequireLineOfSight)
			{
				if (!HasLineOfSight(controller, ref))
					continue;
			}

			//score based on damage potential vs distance
			float score = damage / (distFromTarget + 1.0f);
			if (score > bestScore)
			{
				bestScore = score;
				result.ref = ref;
				result.explosion = explosion;
				result.damage = damage;
				result.distFromTarget = distFromTarget;
			}
		}

		return result.ref ? &result : nullptr;
	}

	//temporarily set explosive as target by modifying the combat controller's target
	//the game will handle actual targeting through normal combat AI
	//we just need to signal that this target should be considered
	void SetExplosiveTarget(void* controller, TESObjectREFR* explosive)
	{
		//store the explosive ref in a way the combat AI can pick it up
		//for now, just log that we found a good target
		//actual target override would require deeper hooks into the targeting system
		Log("AttackExplosiveObject: Found good explosive target at %.0f,%.0f,%.0f",
		    explosive->posX, explosive->posY, explosive->posZ);
	}

	void __fastcall Hook_CombatControllerUpdate(void* controller, void* edx)
	{
		//call original first
		Original(controller);

		Actor* actor = GetPackageOwner(controller);
		if (!actor) return;

		uint32_t refID = *(uint32_t*)((uint8_t*)actor + 0x0C);
		//skip player
		if (refID == 0x14) return;

		//check cooldown
		if (!CheckCooldown(refID)) return;

		//get current target from controller
		Actor* target = (Actor*)*(void**)((uint8_t*)controller + 0xDC);
		if (!target) return;

		//find best explosive near target
		ExplosiveTarget* explosive = FindBestExplosiveNearTarget(actor, target, controller);
		if (!explosive) return;

		//get actor's current DPS from combat state
		//simplified: just check if explosive damage is significant
		if (explosive->damage < 50.0f) return;

		//found a worthwhile explosive target
		SetExplosiveTarget(controller, explosive->ref);
	}

	void WriteJmp(uint32_t src, uint32_t dst)
	{
		DWORD oldProtect;
		VirtualProtect((void*)src, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)src = 0xE9;
		*(uint32_t*)(src + 1) = dst - src - 5;
		*(uint8_t*)(src + 5) = 0x90;
		VirtualProtect((void*)src, 6, oldProtect, &oldProtect);
	}

	void Init()
	{
		//save original bytes
		memcpy(g_originalBytes, (void*)kAddr_CombatControllerUpdate, 6);

		//build trampoline
		DWORD oldProtect;
		VirtualProtect(g_trampoline, sizeof(g_trampoline), PAGE_EXECUTE_READWRITE, &oldProtect);
		memcpy(g_trampoline, g_originalBytes, 6);
		g_trampoline[6] = 0xE9;
		*(uint32_t*)(g_trampoline + 7) = (kAddr_CombatControllerUpdate + 6) - (uint32_t)(g_trampoline + 6) - 5;

		Original = (CombatControllerUpdate_t)(void*)g_trampoline;

		WriteJmp(kAddr_CombatControllerUpdate, (uint32_t)Hook_CombatControllerUpdate);
		Log("AttackExplosiveObject installed at 0x%X", kAddr_CombatControllerUpdate);
	}
}

void AttackExplosiveObject_Init()
{
	AttackExplosiveObject::Init();
}
