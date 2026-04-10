//standalone commands that do stuff (not event handlers)

#include "ImperativeCommands.h"
#define FORMUTILS_USE_NVSE_TYPES
#include "internal/FormUtils.h"
#include "internal/EngineFunctions.h"
#include "internal/BSSpinLock.h"
#include "internal/Detours.h"
#include "internal/ScopedLock.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/GameData.h"
#include "nvse/GameProcess.h"
#include "nvse/GameExtraData.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include <vector>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <cmath>

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"
extern NVSEArrayVarInterface* g_arrInterface;

using namespace FormUtils;

static CRITICAL_SECTION g_crouchLock;
static volatile LONG g_crouchLockInit = 0;
static std::set<UInt32> g_crouchDisabledActors;

static void EnsureCrouchLock() {
	InitCriticalSectionOnce(&g_crouchLockInit, &g_crouchLock);
}

static bool IsCrouchDisabled(UInt32 refID) {
	if (!refID || g_crouchLockInit != 2)
		return false;

	ScopedLock lock(&g_crouchLock);
	return g_crouchDisabledActors.count(refID) != 0;
}

namespace
{
	//xNVSE EventManager - not in older NVSE headers
	constexpr UInt32 kInterface_EventManager_v2 = 8;

	struct EventManagerInterfaceEx {
		bool (*RegisterEvent)(const char* name, UInt8 numParams, UInt8* paramTypes, UInt32 flags);
		bool (*DispatchEvent)(const char* eventName, TESObjectREFR* thisObj, ...);

		enum DispatchReturn : int8_t {
			kRetn_UnknownEvent = -2,
			kRetn_GenericError = -1,
			kRetn_Normal = 0,
			kRetn_EarlyBreak,
			kRetn_Deferred,
		};
		using DispatchCallback = bool (*)(NVSEArrayVarInterface::Element& result, void* anyData);

		DispatchReturn (*DispatchEventAlt)(const char* eventName, DispatchCallback resultCallback, void* anyData, TESObjectREFR* thisObj, ...);
	};

	struct Setting
	{
		void* vtbl;
		union
		{
			UInt32 uint;
			SInt32 i;
			float f;
			char* str;
		} data;
		const char* name;
	};

	using QueueUIMessage_t = bool (*)(const char* msgText, UInt32 iconType, const char* iconPath, const char* soundPath, float displayTime, UInt8 unk5);
	using InventoryRefCreateEntry_t = TESObjectREFR* (__stdcall *)(TESObjectREFR* container, TESForm* itemForm, SInt32 countDelta, ExtraDataList* xData);
	constexpr UInt32 kNVSEData_InventoryReferenceCreateEntry = 7;
	constexpr UInt32 kAVCode_PerceptionCondition = 0x19;
	constexpr UInt32 kAVCode_RightMobilityCondition = 0x1E;
	constexpr UInt32 kAVCode_IgnoreCrippledLimbs = 0x48;

	static EventManagerInterfaceEx* g_eventInterface = nullptr;
	static InventoryRefCreateEntry_t g_inventoryRefCreateEntry = nullptr;
	static QueueUIMessage_t s_queueUIMessage = reinterpret_cast<QueueUIMessage_t>(0x7052F0); //QueueUIMessage
	static Setting* g_sFullHealth = reinterpret_cast<Setting*>(0x11D2AF0);
	static BGSDefaultObjectManager** g_defaultObjectManager = reinterpret_cast<BGSDefaultObjectManager**>(0x11CA80C);

	struct RadioData
	{
		void* voiceList;
		UInt32 unk04;
		UInt32 offset;
		UInt32 soundTimeRemaining;
	};

	struct RadioEntry
	{
		TESObjectREFR* radioRef;
		RadioData data;
	};

	using GetRadioEntryFromActivator_t = RadioEntry* (__cdecl*)(void*);
	using DisableNPCRadio_t = void(__cdecl*)(Actor*);
	using SetNPCRadio_t = void(__cdecl*)(Actor*, TESObjectREFR*);

	struct RadioSoundKey
	{
		UInt32 soundKey;
		UInt8 byte04;
		UInt8 pad05[3];
		UInt32 unk08;
	};

	struct DynamicRadio
	{
		TESObjectREFR* ref;
		RadioSoundKey sound;
		RadioSoundKey radioStaticSound;
		UInt8 isActive;
		UInt8 pad[3];
	};

	struct ExtraRadioDataLite //kExtraData_RadioData (0x68)
	{
		UInt8 pad00[0x10];
		UInt32 rangeType;
		float staticPerc;
		TESObjectREFR* positionRef;
	};

	struct BSGameSound
	{
		void* vtbl;
		UInt32 mapKey;
		UInt32 audioFlags;
		UInt32 flags00C;
		UInt32 stateFlags;
		UInt32 duration;
		UInt16 staticAttenuation;
		UInt16 unk01A;
		UInt16 unk01C;
		UInt16 unk01E;
		UInt16 unk020;
		UInt16 unk022;
		float volume;
		float flt028;
		float flt02C;
		UInt32 unk030;
		UInt16 baseSamplingFreq;
		char filePath[254];
	};

	struct SoundMapEntry
	{
		SoundMapEntry* next;
		UInt32 key;
		void* data;
	};

	struct SoundMap
	{
		void* vtbl;
		UInt32 numBuckets;
		SoundMapEntry** buckets;
		UInt32 numItems;

		BSGameSound* Lookup(UInt32 key) const
		{
			if (!buckets || !numBuckets) return nullptr;
			for (SoundMapEntry* entry = buckets[key % numBuckets]; entry; entry = entry->next)
			{
				if (entry->key == key)
					return reinterpret_cast<BSGameSound*>(entry->data);
			}
			return nullptr;
		}
	};

	static RadioEntry** g_currentRadio = reinterpret_cast<RadioEntry**>(0x11DD42C);
	static tList<DynamicRadio>* g_dynamicRadios = reinterpret_cast<tList<DynamicRadio>*>(0x11DD58C);
	static UInt8* g_radioEnabled = reinterpret_cast<UInt8*>(0x11DD434);
	static char* g_currentSongPath = reinterpret_cast<char*>(0x11DD448);
	static SoundMap* g_playingSounds = reinterpret_cast<SoundMap*>(0x11F6EF0 + 0x54);
	static GetRadioEntryFromActivator_t s_getRadioEntryFromActivator = reinterpret_cast<GetRadioEntryFromActivator_t>(0x832830);
	static DisableNPCRadio_t s_disableNPCRadio = reinterpret_cast<DisableNPCRadio_t>(0x835980);
	static SetNPCRadio_t s_setNPCRadio = reinterpret_cast<SetNPCRadio_t>(0x835810);

	static const char* GetSoundPath(UInt32 soundKey)
	{
		if (!soundKey || soundKey == 0xFFFFFFFF || !g_playingSounds)
			return "<none>";
		BSGameSound* sound = g_playingSounds->Lookup(soundKey);
		if (!sound || !sound->filePath[0])
			return "<unresolved>";
		return sound->filePath;
	}

	static bool IsActorRef(TESObjectREFR* ref)
	{
		if (!ref || !ref->baseForm) return false;
		return ref->baseForm->typeID == kFormType_Creature || ref->baseForm->typeID == kFormType_NPC;
	}

	static bool HasCrippledLimb(Actor* actor)
	{
		float ignoreFlag = actor->avOwner.Fn_03(kAVCode_IgnoreCrippledLimbs);
		if (ignoreFlag != 0.0f)
			return false;

		for (UInt32 avCode = kAVCode_PerceptionCondition; avCode <= kAVCode_RightMobilityCondition; avCode++)
		{
			float condition = actor->avOwner.Fn_03(avCode);
			if (condition <= 0.0f)
				return true;
		}

		return false;
	}

	static double GetActorHealthPercent(Actor* actor)
	{
		if (!actor)
			return 1.0;

		const float baseHealth = actor->avOwner.Fn_01(eActorVal_Health);
		if (baseHealth <= 0.0f)
			return 1.0;

		const float currentHealth = actor->avOwner.Fn_03(eActorVal_Health);
		return static_cast<double>(currentHealth / baseHealth);
	}

	static bool CanUseAidItemVanilla(Actor* actor, TESForm* item)
	{
		if (!actor || !item)
			return false;

		BGSDefaultObjectManager* defObjMgr = g_defaultObjectManager ? *g_defaultObjectManager : nullptr;
		if (!defObjMgr)
			return true;

		auto showBlockedMessage = []()
		{
			if (g_sFullHealth && g_sFullHealth->data.str)
				s_queueUIMessage(g_sFullHealth->data.str, 0, nullptr, nullptr, 2.0f, 0);
		};

		if (item == defObjMgr->defaultObjects.asStruct.Stimpak || item == defObjMgr->defaultObjects.asStruct.SuperStimpak)
		{
			double healthPercent = GetActorHealthPercent(actor);
			if (healthPercent >= 1.0)
			{
				showBlockedMessage();
				return false;
			}
		}
		else if (item == defObjMgr->defaultObjects.asStruct.DoctorsBag)
		{
			if (!HasCrippledLimb(actor))
			{
				showBlockedMessage();
				return false;
			}
		}

		return true;
	}

	static bool EventResultAsBool(NVSEArrayVarInterface::Element& result)
	{
		switch (result.GetType())
		{
		case NVSEArrayVarInterface::Element::kType_Numeric:
			return result.Number() != 0.0;
		case NVSEArrayVarInterface::Element::kType_Form:
			return result.Form() != nullptr;
		case NVSEArrayVarInterface::Element::kType_Array:
			return result.Array() != nullptr;
		case NVSEArrayVarInterface::Element::kType_String:
			return result.String() && result.String()[0] != '\0';
		default:
			return false;
		}
	}

	static bool CanUseItemRef(TESObjectREFR* invRef)
	{
		if (!g_eventInterface || !invRef || !invRef->baseForm)
			return true;

		PlayerCharacter* player = PlayerCharacter::GetSingleton();
		if (!player)
			return true;

		UInt32 shouldActivate = 1;

		auto resultCallback = [](NVSEArrayVarInterface::Element& result, void* shouldActivateAddr) -> bool
		{
			UInt32& shouldActivateRef = *static_cast<UInt32*>(shouldActivateAddr);
			if (shouldActivateRef && result.IsValid())
				shouldActivateRef = EventResultAsBool(result) ? 1 : 0;
			return true;
		};

		auto retn = g_eventInterface->DispatchEventAlt(
			"ShowOff:OnPreActivateInventoryItem",
			resultCallback,
			&shouldActivate,
			player,
			invRef->baseForm,
			invRef,
			&shouldActivate,
			static_cast<UInt32>(0));

		UInt32 isSpecialActivation = 0;
		g_eventInterface->DispatchEventAlt(
			"ShowOff:OnPreActivateInventoryItemAlt",
			resultCallback,
			&shouldActivate,
			player,
			invRef->baseForm,
			invRef,
			&shouldActivate,
			static_cast<UInt32>(0),
			isSpecialActivation);

		if (retn == EventManagerInterfaceEx::kRetn_UnknownEvent)
			return true;

		return shouldActivate != 0;
	}
}

static ParamInfo kParams_GetRefsSortedByDistance[5] = {
	{ "maxDistance",      kParamType_Float,   0 },
	{ "formType",         kParamType_Integer, 1 },
	{ "cellDepth",        kParamType_Integer, 1 },
	{ "includeTakenRefs", kParamType_Integer, 1 },
	{ "baseForm",         kParamType_AnyForm, 1 },
};

DEFINE_COMMAND_PLUGIN(GetRefsSortedByDistance, "Returns array of refs sorted by distance from player", 0, 5, kParams_GetRefsSortedByDistance);

enum {
	kFormTypeFilter_AnyType = kFilter_AnyType,
	kFormTypeFilter_Actor = kFilter_Actor,
	kFormTypeFilter_InventoryItem = kFilter_InventoryItem,
};

static bool IsTakenRef(TESObjectREFR* refr)
{
	if (!refr->IsDeleted()) return false;
	UInt8 formType = refr->baseForm->typeID;
	return FormUtils::IsInventoryItemType(formType);
}

static bool MatchesBaseForm(TESObjectREFR* refr, TESForm* baseForm)
{
	if (!baseForm) return true;
	return refr->baseForm == baseForm;
}

static bool MatchesFormType(TESObjectREFR* refr, UInt32 formType, bool includeTakenRefs)
{
	if (!refr || !refr->baseForm) return false;
	if (!includeTakenRefs && IsTakenRef(refr)) return false;

	UInt8 baseType = refr->baseForm->typeID;

	switch (formType)
	{
		case kFormTypeFilter_AnyType:
			return true;
		case kFormTypeFilter_Actor:
			if (refr->baseForm->refID == 7) return false;
			return baseType == kFormType_Creature || baseType == kFormType_NPC;
		case kFormTypeFilter_InventoryItem:
			return FormUtils::IsInventoryItemType(baseType);
		default:
			if (baseType == kFormType_NPC && refr->baseForm->refID == 7) return false;
			return baseType == formType;
	}
}

bool Cmd_GetRefsSortedByDistance_Execute(COMMAND_ARGS)
{
	*result = 0;

	float maxDistance = 0;
	UInt32 formType = kFormTypeFilter_AnyType;
	SInt32 cellDepth = 0;
	UInt32 includeTakenRefs = 0;
	TESForm* baseForm = nullptr;

	if (!ExtractArgs(EXTRACT_ARGS, &maxDistance, &formType, &cellDepth, &includeTakenRefs, &baseForm))
		return true;

	if (maxDistance <= 0)
	{
		if (IsConsoleMode()) Console_Print("GetRefsSortedByDistance >> maxDistance must be > 0");
		return true;
	}

	PlayerCharacter* player = PlayerCharacter::GetSingleton();
	if (!player || !player->parentCell) return true;

	float maxDistSq = maxDistance * maxDistance;

	struct RefWithDist {
		TESObjectREFR* ref;
		float distance;
	};
	std::vector<RefWithDist> refs;

	TESObjectCELL* playerCell = player->parentCell;

	if (cellDepth == -1) cellDepth = 5;

	auto ProcessCell = [&](TESObjectCELL* cell)
	{
		if (!cell) return;
		for (auto iter = cell->objectList.Begin(); !iter.End(); ++iter)
		{
			TESObjectREFR* refr = iter.Get();
			if (!refr || refr == player) continue;
			if (!MatchesFormType(refr, formType, includeTakenRefs != 0)) continue;
			if (!MatchesBaseForm(refr, baseForm)) continue;

			float distSq = FormUtils::CalcDistanceSquared(refr, (TESObjectREFR*)player);
			if (distSq > maxDistSq) continue;

			refs.push_back({ refr, sqrtf(distSq) });
		}
	};

	ProcessCell(playerCell);

	TESWorldSpace* world = playerCell->worldSpace;
	if (world && world->cellMap && cellDepth > 0 && !playerCell->IsInterior() && playerCell->coords)
	{
		SInt32 baseX = (SInt32)playerCell->coords->x;
		SInt32 baseY = (SInt32)playerCell->coords->y;

		for (SInt32 dx = -cellDepth; dx <= cellDepth; dx++)
		{
			for (SInt32 dy = -cellDepth; dy <= cellDepth; dy++)
			{
				if (dx == 0 && dy == 0) continue;
				UInt32 key = ((baseX + dx) << 16) | ((baseY + dy) & 0xFFFF);
				TESObjectCELL* cell = world->cellMap->Lookup(key);
				ProcessCell(cell);
			}
		}
	}

	std::sort(refs.begin(), refs.end(), [](const RefWithDist& a, const RefWithDist& b) {
		return a.distance < b.distance;
	});

	NVSEArrayVarInterface::Array* arr = g_arrInterface->CreateArray(nullptr, 0, scriptObj);
	for (const auto& item : refs)
	{
		NVSEArrayVarInterface::Element elem(item.ref);
		g_arrInterface->AppendElement(arr, elem);
	}

	g_arrInterface->AssignCommandResult(arr, result);

	if (IsConsoleMode())
	{
		Console_Print("GetRefsSortedByDistance >> Found %d refs within %.1f units", refs.size(), maxDistance);
	}

	return true;
}

typedef TESObjectREFR* (*_PlaceAtMe)(TESObjectREFR*, TESForm*, UInt32, UInt32, UInt32, float);
static const _PlaceAtMe PlaceAtMe = (_PlaceAtMe)0x5C4B30;

static ParamInfo kParams_Duplicate[1] = {
	{ "count", kParamType_Integer, 1 },
};

DEFINE_COMMAND_PLUGIN(Duplicate, "Duplicates the reference and returns the new ref", 1, 1, kParams_Duplicate);

bool Cmd_Duplicate_Execute(COMMAND_ARGS)
{
	*result = 0;

	UInt32 count = 1;

	ExtractArgs(EXTRACT_ARGS, &count);

	if (count < 1) count = 1;

	if (!thisObj || !thisObj->baseForm)
	{
		if (IsConsoleMode())
			Console_Print("Duplicate >> No reference selected");
		return true;
	}

	TESObjectREFR* lastRef = nullptr;
	UInt32 created = 0;

	for (UInt32 i = 0; i < count; i++)
	{
		TESObjectREFR* newRef = PlaceAtMe(
			thisObj,
			thisObj->baseForm,
			1,
			0,
			0,
			1.0f);

		if (newRef)
		{
			lastRef = newRef;
			created++;
		}
	}

	if (lastRef)
	{
		*((UInt32*)result) = lastRef->refID;
		if (IsConsoleMode())
			Console_Print("Duplicate >> Created %d ref(s), last: %08X", created, lastRef->refID);
	}
	else
	{
		if (IsConsoleMode())
			Console_Print("Duplicate >> Failed to create reference");
	}

	return true;
}

typedef bool (__thiscall *_ConditionList_Evaluate)(void* conditionList, TESObjectREFR* runOnRef, TESForm* arg2, bool* result, bool arg4);
static const _ConditionList_Evaluate ConditionList_Evaluate = (_ConditionList_Evaluate)0x680C60;

typedef SInt32 (__thiscall *_GetActorValue)(void* actorValueOwner, UInt32 avCode);
static const _GetActorValue GetActorValue = (_GetActorValue)0x66EF50;

typedef SInt32 (__thiscall *_GetItemCount)(TESObjectREFR* container, TESForm* item);
static const _GetItemCount GetItemCount = (_GetItemCount)0x575610;

static ParamInfo kParams_GetAvailableRecipes[1] = {
	{ "category", kParamType_AnyForm, 1 },
};

DEFINE_COMMAND_PLUGIN(GetAvailableRecipes, "Returns array of recipes player can craft", 0, 1, kParams_GetAvailableRecipes);

bool Cmd_GetAvailableRecipes_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESForm* categoryFilter = nullptr;
	ExtractArgs(EXTRACT_ARGS, &categoryFilter);

	if (categoryFilter && categoryFilter->typeID != kFormType_RecipeCategory)
		categoryFilter = nullptr;

	PlayerCharacter* player = PlayerCharacter::GetSingleton();
	if (!player) return true;

	DataHandler* dataHandler = *(DataHandler**)0x011C3F2C;
	if (!dataHandler) return true;

	std::vector<TESForm*> availableRecipes;
	tList<TESRecipe>* recipeList = &dataHandler->recipeList;

	for (auto iter = recipeList->Begin(); !iter.End(); ++iter)
	{
		TESRecipe* recipe = iter.Get();
		if (!recipe) continue;

		if (categoryFilter)
		{
			TESRecipeCategory* cat = recipe->category;
			TESRecipeCategory* subCat = recipe->subCategory;
			if (cat != categoryFilter && subCat != categoryFilter)
				continue;
		}

		void* conditionList = &recipe->conditions;
		bool evalResult = false;
		bool conditionsPassed = ConditionList_Evaluate(conditionList, player, nullptr, &evalResult, false);
		if (!conditionsPassed)
			continue;

		if (recipe->reqSkill != (UInt32)-1 && recipe->reqSkillLevel > 0)
		{
			void* actorValueOwner = (void*)((UInt8*)player + 0xA4);
			SInt32 playerSkill = GetActorValue(actorValueOwner, recipe->reqSkill);
			if (playerSkill < (SInt32)recipe->reqSkillLevel)
				continue;
		}

		bool hasAllInputs = true;
		for (auto inputIter = recipe->inputs.Begin(); !inputIter.End(); ++inputIter)
		{
			ComponentEntry* component = inputIter.Get();
			if (!component || !component->item)
				continue;

			UInt32 playerCount = GetItemCount(player, component->item);
			if (playerCount < component->quantity)
			{
				hasAllInputs = false;
				break;
			}
		}
		if (!hasAllInputs)
			continue;

		availableRecipes.push_back(recipe);
	}

	if (!availableRecipes.empty() && g_arrInterface)
	{
		NVSEArrayVarInterface::Array* arr = g_arrInterface->CreateArray(nullptr, 0, scriptObj);
		for (TESForm* recipe : availableRecipes)
		{
			NVSEArrayVarInterface::Element elem(recipe);
			g_arrInterface->AppendElement(arr, elem);
		}
		g_arrInterface->AssignCommandResult(arr, result);
	}

	if (IsConsoleMode())
	{
		Console_Print("GetAvailableRecipes >> Found %d craftable recipes", availableRecipes.size());
	}

	return true;
}

//debug command to dump CombatTarget memory for offset verification
static ParamInfo kParams_DumpCombatTarget[1] = {
	{ "target", kParamType_Actor, 0 },
};

DEFINE_COMMAND_PLUGIN(DumpCombatTarget, "Dumps CombatTarget structure for offset verification", 1, 1, kParams_DumpCombatTarget);

typedef void* (__thiscall *_GetCombatController)(Actor*);
typedef void* (__thiscall *_GetCombatTargetForActor)(void* combatGroup, Actor* target);
typedef bool (__thiscall *_CombatGroupCanAddTarget)(void* combatGroup, Actor* target);
typedef bool (__thiscall *_ActorIsDeadForForceCombatTarget)(Actor*, bool);
typedef bool (__thiscall *_ActorCanAttackActor)(Actor*, Actor*);
typedef UInt8 (__thiscall *_CharacterIsGuardForForceCombatTarget)(Character*);
typedef void (__thiscall *_CombatControllerSetTarget)(void* combatController, Actor* target);
typedef void (__thiscall *_CombatControllerAddCombatTarget)(void* combatController, Actor* target, SInt32 a3, SInt32 a4, float a5, float a6);
typedef void* (__thiscall *_CombatManagerAddCombatant)(void* combatManager, Actor* actor, Actor* target, SInt32 a4, SInt32 a5);
typedef void (__thiscall *_ActorStartCombat)(Actor* actor, Actor* target, void* combatGroup, bool ignoreActorLimit, bool isAggressor, bool a6, UInt32 a7, bool a8, TESPackage* package);
typedef void (__thiscall *_ActorPutCreatedPackage)(Actor* actor, void* package, UInt32 unk1, UInt32 unk2);
typedef void (__thiscall *_ProcessComputeLastTimeProcessed)(void* process);
typedef void (__thiscall *_ProcessSavePackageToExtraData)(void* process, Actor* actor);
typedef void (__thiscall *_CombatControllerSetByte0C4)(void* combatController);
static const _GetCombatController GetCombatController = (_GetCombatController)0x8A02D0;
static const _GetCombatTargetForActor GetCombatTargetForActor = (_GetCombatTargetForActor)0x9865D0;
static const _CombatGroupCanAddTarget CombatGroupCanAddTarget = (_CombatGroupCanAddTarget)0x9866D0;
static const _ActorIsDeadForForceCombatTarget ActorIsDeadForForceCombatTarget = (_ActorIsDeadForForceCombatTarget)0x8844F0;
static const _ActorCanAttackActor ActorCanAttackActor = (_ActorCanAttackActor)0x8B0670;
static const _CharacterIsGuardForForceCombatTarget CharacterIsGuardForForceCombatTarget = (_CharacterIsGuardForForceCombatTarget)0x8D1ED0;
static const _CombatControllerSetTarget CombatControllerSetTarget = (_CombatControllerSetTarget)0x980830;
static const _CombatControllerAddCombatTarget CombatControllerAddCombatTarget = (_CombatControllerAddCombatTarget)0x97F930;
static const _CombatManagerAddCombatant CombatManagerAddCombatant = (_CombatManagerAddCombatant)0x992110;
static const _ActorStartCombat ActorStartCombat = (_ActorStartCombat)0x89FCF0;
static const _ActorPutCreatedPackage ActorPutCreatedPackage = (_ActorPutCreatedPackage)0x87EAC0;
static const _ProcessComputeLastTimeProcessed ProcessComputeLastTimeProcessed = (_ProcessComputeLastTimeProcessed)0x907650;
static const _ProcessSavePackageToExtraData ProcessSavePackageToExtraData = (_ProcessSavePackageToExtraData)0x9130F0;
static const _CombatControllerSetByte0C4 CombatControllerSetByte0C4 = (_CombatControllerSetByte0C4)0x8A0250;
static void** g_combatManager = reinterpret_cast<void**>(0x11F1958);

bool Cmd_DumpCombatTarget_Execute(COMMAND_ARGS)
{
	*result = 0;

	Actor* target = nullptr;

	if (!ExtractArgs(EXTRACT_ARGS, &target))
	{
		Console_Print("DumpCombatTarget >> ExtractArgs failed");
		return true;
	}

	if (!thisObj || !target || !IsActorRef(thisObj))
	{
		Console_Print("DumpCombatTarget >> Call on actor ref with target as param");
		return true;
	}

	Actor* observer = (Actor*)thisObj;

	//get combat controller via direct function call
	void* combatController = GetCombatController(observer);
	if (!combatController)
	{
		Console_Print("DumpCombatTarget >> Observer has no combat controller");
		return true;
	}

	//combatGroup is at offset 0x80 in CombatController
	void* combatGroup = *(void**)((UInt8*)combatController + 0x80);
	if (!combatGroup)
	{
		Console_Print("DumpCombatTarget >> No combat group");
		return true;
	}

	void* combatTarget = GetCombatTargetForActor(combatGroup, target);
	if (!combatTarget)
	{
		Console_Print("DumpCombatTarget >> No CombatTarget for target actor");
		return true;
	}

	Console_Print("DumpCombatTarget >> CombatTarget at %08X", combatTarget);

	//try to interpret known fields
	UInt8* bytes = (UInt8*)combatTarget;
	Actor** pTarget = (Actor**)bytes;
	Console_Print("  +00 pTarget: %08X (expected %08X)", *pTarget, target);

	SInt32 detectionLevel = *(SInt32*)(bytes + 0x04);
	Console_Print("  +04 detectionLevel: %d", detectionLevel);

	//BGSWorldLocation at +08 (kLastSeenLocation) - 0x10 bytes
	float* lastSeenPos = (float*)(bytes + 0x08);
	Console_Print("  +08 kLastSeenLocation: %.1f, %.1f, %.1f", lastSeenPos[0], lastSeenPos[1], lastSeenPos[2]);

	//BGSWorldLocation at +18 (kDetectedLocation)
	float* detectedPos = (float*)(bytes + 0x18);
	Console_Print("  +18 kDetectedLocation: %.1f, %.1f, %.1f", detectedPos[0], detectedPos[1], detectedPos[2]);

	//BGSWorldLocation at +28 (kLastFullyVisibleLocation)
	float* fullyVisPos = (float*)(bytes + 0x28);
	Console_Print("  +28 kLastFullyVisibleLocation: %.1f, %.1f, %.1f", fullyVisPos[0], fullyVisPos[1], fullyVisPos[2]);

	//BGSWorldLocation at +38 (kInitialTargetLocation)
	float* initialPos = (float*)(bytes + 0x38);
	Console_Print("  +38 kInitialTargetLocation: %.1f, %.1f, %.1f", initialPos[0], initialPos[1], initialPos[2]);

	//counts at +48
	UInt16 searchCount = *(UInt16*)(bytes + 0x48);
	UInt16 attackerCount = *(UInt16*)(bytes + 0x4A);
	UInt8 inLOSCount = *(UInt8*)(bytes + 0x4C);
	UInt8 inFullLOSCount = *(UInt8*)(bytes + 0x4D);
	Console_Print("  +48 searchCount: %d, attackerCount: %d", searchCount, attackerCount);
	Console_Print("  +4C inLOSCount: %d, inFullLOSCount: %d", inLOSCount, inFullLOSCount);

	//timestamps at +50, +54, +58, +5C, +60, +64
	float* timestamps = (float*)(bytes + 0x50);
	Console_Print("  +50 timestamps: %.2f, %.2f, %.2f, %.2f, %.2f, %.2f",
		timestamps[0], timestamps[1], timestamps[2], timestamps[3], timestamps[4], timestamps[5]);

	//also print target's actual position for comparison
	Console_Print("  Target actual pos: %.1f, %.1f, %.1f", target->posX, target->posY, target->posZ);

	*result = 1;
	return true;
}

//helper to get CombatTarget for observer/target pair
static void* GetCombatTargetData(Actor* observer, Actor* target)
{
	if (!observer || !target) return nullptr;

	void* combatController = GetCombatController(observer);
	if (!combatController) return nullptr;

	void* combatGroup = *(void**)((UInt8*)combatController + 0x80);
	if (!combatGroup) return nullptr;

	return GetCombatTargetForActor(combatGroup, target);
}

namespace
{
	using EvaluateCombatTargets_t = Actor* (__thiscall*)(void* combatGroup, Actor* actor);
	using CanAttackActor_t = bool (__thiscall*)(Actor* actor, Actor* target);

	enum class ForceCombatTargetResult
	{
		kSuccess,
		kInvalidArgs,
		kNoProcess,
		kActorDead,
		kHookFailed,
		kNoCombatController,
		kNoCombatGroup,
		kCannotAddTarget,
		kAddTargetFailed,
	};

	static CRITICAL_SECTION g_forceCombatTargetLock;
	static volatile LONG g_forceCombatTargetLockInit = 0;
	static std::unordered_map<UInt32, UInt32> g_forcedCombatTargets;
	static Detours::JumpDetour s_forceCombatTargetDetour;
	static Detours::JumpDetour s_forceCombatCanAttackDetour;
	static EvaluateCombatTargets_t s_evaluateCombatTargetsOriginal = nullptr;
	static CanAttackActor_t s_canAttackActorOriginal = nullptr;
	static bool g_forceCombatTargetHookInstalled = false;
	static UInt32 GetForcedCombatTargetRefID(UInt32 actorRefID);

	static bool IsForcedCombatTargetPair(Actor* actor, Actor* target)
	{
		if (!actor || !target || actor == target)
			return false;
		if (ActorIsDeadForForceCombatTarget(actor, false) || ActorIsDeadForForceCombatTarget(target, false))
			return false;
		return GetForcedCombatTargetRefID(actor->refID) == target->refID;
	}

	static void* TryAddCombatantBootstrap(Actor* actor, Actor* target, bool ignoreActorLimit)
	{
		if (!actor || !target || !g_combatManager || !*g_combatManager)
			return nullptr;

		if (!ActorCanAttackActor(actor, target))
			return nullptr;

		void* combatController = CombatManagerAddCombatant(*g_combatManager, actor, target, 0, 0);
		if (!combatController)
			return nullptr;

		if (ignoreActorLimit)
			CombatControllerSetByte0C4(combatController);

		void* process = Engine::Actor_GetProcess(actor);
		if (process)
		{
			ProcessComputeLastTimeProcessed(process);
			ProcessSavePackageToExtraData(process, actor);
		}

		ActorPutCreatedPackage(actor, combatController, 0, 1);
		actor->unk104 = 1;
		return combatController;
	}

	static void EnsureForceCombatTargetLockInit()
	{
		InitCriticalSectionOnce(&g_forceCombatTargetLockInit, &g_forceCombatTargetLock);
	}

	static UInt32 GetForcedCombatTargetRefID(UInt32 actorRefID)
	{
		if (!actorRefID || g_forceCombatTargetLockInit != 2)
			return 0;

		ScopedLock lock(&g_forceCombatTargetLock);
		auto it = g_forcedCombatTargets.find(actorRefID);
		return it != g_forcedCombatTargets.end() ? it->second : 0;
	}

	static void SetForcedCombatTargetRefID(UInt32 actorRefID, UInt32 targetRefID)
	{
		if (!actorRefID || !targetRefID)
			return;

		EnsureForceCombatTargetLockInit();
		ScopedLock lock(&g_forceCombatTargetLock);
		g_forcedCombatTargets[actorRefID] = targetRefID;
	}

	static void ClearForcedCombatTargetRefID(UInt32 actorRefID)
	{
		if (!actorRefID || g_forceCombatTargetLockInit != 2)
			return;

		ScopedLock lock(&g_forceCombatTargetLock);
		g_forcedCombatTargets.erase(actorRefID);
	}

	static Actor* __fastcall Hook_EvalueCombatTargets(void* combatGroup, void*, Actor* actor)
	{
		if (!s_evaluateCombatTargetsOriginal)
			return nullptr;

		if (!combatGroup || !actor)
			return s_evaluateCombatTargetsOriginal(combatGroup, actor);

		UInt32 forcedTargetRefID = GetForcedCombatTargetRefID(actor->refID);
		if (!forcedTargetRefID)
			return s_evaluateCombatTargetsOriginal(combatGroup, actor);

		TESForm* forcedForm = (TESForm*)Engine::LookupFormByID(forcedTargetRefID);
		Actor* forcedTarget = forcedForm && IsActorRef((TESObjectREFR*)forcedForm) ? (Actor*)forcedForm : nullptr;
		if (!forcedTarget || forcedTarget == actor || ActorIsDeadForForceCombatTarget(forcedTarget, false))
		{
			ClearForcedCombatTargetRefID(actor->refID);
			return s_evaluateCombatTargetsOriginal(combatGroup, actor);
		}

		if (GetCombatTargetForActor(combatGroup, forcedTarget))
			return forcedTarget;

		return s_evaluateCombatTargetsOriginal(combatGroup, actor);
	}

	static bool __fastcall Hook_CanAttackActor(Actor* actor, void*, Actor* target)
	{
		if (IsForcedCombatTargetPair(actor, target))
			return true;
		if (!s_canAttackActorOriginal)
			return false;
		return s_canAttackActorOriginal(actor, target);
	}

	static bool EnsureForceCombatTargetHookInstalled()
	{
		if (g_forceCombatTargetHookInstalled)
			return true;

		EnsureForceCombatTargetLockInit();

		if (!s_forceCombatTargetDetour.WriteRelJump(0x986C60, Hook_EvalueCombatTargets, 10))
			return false;

		s_evaluateCombatTargetsOriginal = s_forceCombatTargetDetour.GetTrampoline<EvaluateCombatTargets_t>();
		if (!s_evaluateCombatTargetsOriginal)
		{
			s_forceCombatTargetDetour.Remove();
			return false;
		}

		if (!s_forceCombatCanAttackDetour.WriteRelJump(0x8B0670, Hook_CanAttackActor, 6))
		{
			s_forceCombatTargetDetour.Remove();
			s_evaluateCombatTargetsOriginal = nullptr;
			return false;
		}

		s_canAttackActorOriginal = s_forceCombatCanAttackDetour.GetTrampoline<CanAttackActor_t>();
		if (!s_canAttackActorOriginal)
		{
			s_forceCombatCanAttackDetour.Remove();
			s_forceCombatTargetDetour.Remove();
			s_evaluateCombatTargetsOriginal = nullptr;
			return false;
		}

		g_forceCombatTargetHookInstalled = true;
		return true;
	}

	static const char* ForceCombatTargetResultToString(ForceCombatTargetResult result)
	{
		switch (result)
		{
		case ForceCombatTargetResult::kSuccess:
			return "success";
		case ForceCombatTargetResult::kInvalidArgs:
			return "invalid actor/target";
		case ForceCombatTargetResult::kNoProcess:
			return "actor or target has no current process";
		case ForceCombatTargetResult::kActorDead:
			return "actor or target is dead";
		case ForceCombatTargetResult::kHookFailed:
			return "target-selection hook unavailable";
		case ForceCombatTargetResult::kNoCombatController:
			return "failed to create or get combat controller";
		case ForceCombatTargetResult::kNoCombatGroup:
			return "combat controller has no combat group";
		case ForceCombatTargetResult::kCannotAddTarget:
			return "combat group rejected target (likely faction/aggression relation)";
		case ForceCombatTargetResult::kAddTargetFailed:
			return "target was not added to combat group";
		default:
			return "unknown";
		}
	}

	static ForceCombatTargetResult TryForceCombatTarget(Actor* actor, Actor* target)
	{
		if (!actor || !target || actor == target)
			return ForceCombatTargetResult::kInvalidArgs;
		if (!Engine::Actor_GetProcess(actor) || !Engine::Actor_GetProcess(target))
			return ForceCombatTargetResult::kNoProcess;
		if (ActorIsDeadForForceCombatTarget(actor, false) || ActorIsDeadForForceCombatTarget(target, false))
			return ForceCombatTargetResult::kActorDead;
		if (!EnsureForceCombatTargetHookInstalled())
			return ForceCombatTargetResult::kHookFailed;
		SetForcedCombatTargetRefID(actor->refID, target->refID);

		void* combatController = GetCombatController(actor);
		if (!combatController)
		{
			const bool ignoreActorLimit = true;
			bool isGuard = actor->baseForm && actor->baseForm->typeID == kFormType_NPC
				&& CharacterIsGuardForForceCombatTarget((Character*)actor) != 0;
			ActorStartCombat(actor, target, nullptr, ignoreActorLimit, !isGuard, false, 0, true, nullptr);
			combatController = GetCombatController(actor);
			if (!combatController)
				combatController = TryAddCombatantBootstrap(actor, target, ignoreActorLimit);
		}

		if (!combatController)
		{
			ClearForcedCombatTargetRefID(actor->refID);
			return ForceCombatTargetResult::kNoCombatController;
		}

		void* combatGroup = *(void**)((UInt8*)combatController + 0x80);
		if (!combatGroup)
		{
			ClearForcedCombatTargetRefID(actor->refID);
			return ForceCombatTargetResult::kNoCombatGroup;
		}

		if (!GetCombatTargetForActor(combatGroup, target))
		{
			if (!CombatGroupCanAddTarget(combatGroup, target))
			{
				ClearForcedCombatTargetRefID(actor->refID);
				return ForceCombatTargetResult::kCannotAddTarget;
			}
			CombatControllerAddCombatTarget(combatController, target, 0, 0, 0.0f, 0.0f);
		}

		if (!GetCombatTargetForActor(combatGroup, target))
		{
			ClearForcedCombatTargetRefID(actor->refID);
			return ForceCombatTargetResult::kAddTargetFailed;
		}

		CombatControllerSetTarget(combatController, target);
		return ForceCombatTargetResult::kSuccess;
	}

	static void ClearForcedCombatTarget(Actor* actor)
	{
		if (!actor)
			return;

		ClearForcedCombatTargetRefID(actor->refID);

		void* combatController = GetCombatController(actor);
		if (!combatController)
			return;

		void* combatGroup = *(void**)((UInt8*)combatController + 0x80);
		if (!combatGroup || !s_evaluateCombatTargetsOriginal)
			return;

		CombatControllerSetTarget(combatController, s_evaluateCombatTargetsOriginal(combatGroup, actor));
	}
}

//helper to create position array from CombatTarget offset
static bool CreatePositionArray(COMMAND_ARGS, void* combatTarget, UInt32 offset)
{
	if (!combatTarget || !g_arrInterface) return false;

	float* pos = (float*)((UInt8*)combatTarget + offset);

	NVSEArrayVarInterface::Array* arr = g_arrInterface->CreateArray(nullptr, 0, scriptObj);
	g_arrInterface->AppendElement(arr, NVSEArrayVarInterface::Element(pos[0]));
	g_arrInterface->AppendElement(arr, NVSEArrayVarInterface::Element(pos[1]));
	g_arrInterface->AppendElement(arr, NVSEArrayVarInterface::Element(pos[2]));
	g_arrInterface->AssignCommandResult(arr, result);

	return true;
}

//combat target location getters - all use same params
static ParamInfo kParams_CombatTargetLocation[1] = {
	{ "target", kParamType_Actor, 0 },
};

DEFINE_COMMAND_PLUGIN(GetTargetLastSeenLocation, "Returns array [x,y,z] of where observer last saw target", 1, 1, kParams_CombatTargetLocation);
DEFINE_COMMAND_PLUGIN(GetTargetDetectedLocation, "Returns array [x,y,z] of where observer detected target (sound/event)", 1, 1, kParams_CombatTargetLocation);
DEFINE_COMMAND_PLUGIN(GetTargetLastFullyVisibleLocation, "Returns array [x,y,z] of where observer last had full LOS to target", 1, 1, kParams_CombatTargetLocation);
DEFINE_COMMAND_PLUGIN(GetTargetInitialLocation, "Returns array [x,y,z] of where observer first spotted target", 1, 1, kParams_CombatTargetLocation);

bool Cmd_GetTargetLastSeenLocation_Execute(COMMAND_ARGS)
{
	*result = 0;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target) || !IsActorRef(thisObj)) return true;

	void* ct = GetCombatTargetData((Actor*)thisObj, target);
	CreatePositionArray(PASS_COMMAND_ARGS, ct, 0x08);
	return true;
}

bool Cmd_GetTargetDetectedLocation_Execute(COMMAND_ARGS)
{
	*result = 0;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target) || !IsActorRef(thisObj)) return true;

	void* ct = GetCombatTargetData((Actor*)thisObj, target);
	CreatePositionArray(PASS_COMMAND_ARGS, ct, 0x18);
	return true;
}

bool Cmd_GetTargetLastFullyVisibleLocation_Execute(COMMAND_ARGS)
{
	*result = 0;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target) || !IsActorRef(thisObj)) return true;

	void* ct = GetCombatTargetData((Actor*)thisObj, target);
	CreatePositionArray(PASS_COMMAND_ARGS, ct, 0x28);
	return true;
}

bool Cmd_GetTargetInitialLocation_Execute(COMMAND_ARGS)
{
	*result = 0;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target) || !IsActorRef(thisObj)) return true;

	void* ct = GetCombatTargetData((Actor*)thisObj, target);
	CreatePositionArray(PASS_COMMAND_ARGS, ct, 0x38);
	return true;
}

static ParamInfo kParams_SetCreatureCombatSkill[2] = {
	{ "value",    kParamType_Integer,  0 },
	{ "creature", kParamType_AnyForm,  1 },
};

static ParamInfo kParams_UseAidItem[1] = {
	{ "item", kParamType_AnyForm, 0 },
};

DEFINE_COMMAND_PLUGIN(ChangeRadioTrack, "Forces active radio station to advance to next track", 0, 0, nullptr);
DEFINE_COMMAND_PLUGIN(IsRadioPlaying, "Returns 1 if any pip-boy or ambient radio is currently playing", 0, 0, nullptr);
DEFINE_COMMAND_PLUGIN(UseAidItem, "Uses an aid item (AlchemyItem) on the calling actor", 1, 1, kParams_UseAidItem);
DEFINE_COMMAND_PLUGIN(SetCreatureCombatSkill, "Sets creature combat skill (0-255)", 0, 2, kParams_SetCreatureCombatSkill);

//resolve TalkingActivator or Activator->radioStation from a base form
static void* ResolveRadioActivator(TESForm* baseForm)
{
	if (!baseForm) return nullptr;
	if (baseForm->typeID == kFormType_TalkingActivator)
		return baseForm;
	if (baseForm->typeID == kFormType_Activator)
	{
		auto* actBase = static_cast<TESObjectACTI*>(baseForm);
		if (actBase->radioStation)
			return actBase->radioStation;
		return baseForm;
	}
	return nullptr;
}

static void StopRadioSound(RadioSoundKey* key)
{
	RadioSoundKey copy = *key;
	Engine::BSSoundHandle_Stop(&copy);
	key->soundKey = 0xFFFFFFFF;
}

static bool AdvanceDynamicRadios(double* result)
{
	if (!g_dynamicRadios)
		return false;

	UInt32 stoppedCount = 0, advancedCount = 0, reseatCount = 0;

	for (auto iter = g_dynamicRadios->Begin(); !iter.End(); ++iter)
	{
		DynamicRadio* dr = iter.Get();
		if (!dr || !dr->isActive)
			continue;

		TESObjectREFR* stationRef = nullptr;
		Actor* sourceActor = nullptr;
		void* activator = nullptr;

		if (dr->ref)
		{
			stationRef = dr->ref;
			TESForm* baseForm = dr->ref->baseForm;
			if (baseForm)
			{
				UInt8 baseType = baseForm->typeID;
				if (baseType == kFormType_NPC || baseType == kFormType_Creature)
					sourceActor = static_cast<Actor*>(dr->ref);
				activator = ResolveRadioActivator(baseForm);
			}

			auto* xRadio = reinterpret_cast<ExtraRadioDataLite*>(
				Engine::BaseExtraList_GetByType(&dr->ref->extraDataList, kExtraData_RadioData));
			if (xRadio && xRadio->positionRef)
			{
				stationRef = xRadio->positionRef;
				activator = ResolveRadioActivator(stationRef->baseForm);
			}
		}

		if (activator)
		{
			RadioEntry* entry = s_getRadioEntryFromActivator(activator);
			if (entry)
			{
				entry->data.soundTimeRemaining = 1;
				advancedCount++;
			}
		}

		UInt32 soundKey = dr->sound.soundKey;
		UInt32 staticKey = dr->radioStaticSound.soundKey;

		if (soundKey && soundKey != 0xFFFFFFFF)
		{
			StopRadioSound(&dr->sound);
			stoppedCount++;
		}
		if (staticKey && staticKey != 0xFFFFFFFF)
		{
			StopRadioSound(&dr->radioStaticSound);
			stoppedCount++;
		}

		//if station entry couldn't be advanced directly, reseat the radio
		if (!advancedCount && stationRef)
		{
			Actor* reseatActor = sourceActor ? sourceActor : PlayerCharacter::GetSingleton();
			if (reseatActor && s_disableNPCRadio && s_setNPCRadio)
			{
				s_disableNPCRadio(reseatActor);
				s_setNPCRadio(reseatActor, stationRef);
				reseatCount++;
			}
		}
	}

	if (stoppedCount || advancedCount || reseatCount)
	{
		if (IsConsoleMode())
			Console_Print("ChangeRadioTrack >> stopped=%d advanced=%d reseat=%d",
				stoppedCount, advancedCount, reseatCount);
		*result = 1;
		return true;
	}

	if (IsConsoleMode())
		Console_Print("ChangeRadioTrack >> no active ambient radio");
	return false;
}

static bool AdvanceCurrentRadioTrack(double* result)
{
	*result = 0;

	if (!g_currentRadio)
		return true;

	UInt8 radioEnabled = g_radioEnabled ? *g_radioEnabled : 0;
	RadioEntry* radio = radioEnabled ? *g_currentRadio : nullptr;

	//pip-boy radio disabled or no entry - try ambient/dynamic radios
	if (!radio)
	{
		AdvanceDynamicRadios(result);
		return true;
	}

	//force one tick remaining so the engine advances to next track
	radio->data.soundTimeRemaining = 1;
	*result = 1;
	return true;
}

bool Cmd_ChangeRadioTrack_Execute(COMMAND_ARGS)
{
	return AdvanceCurrentRadioTrack(result);
}

bool Cmd_IsRadioPlaying_Execute(COMMAND_ARGS)
{
	*result = 0;

	// Pip-boy radio song playback path.
	if (g_currentSongPath && g_currentSongPath[0])
	{
		*result = 1;
		return true;
	}

	//pip-boy radio voice queue
	if (g_radioEnabled && *g_radioEnabled && g_currentRadio && *g_currentRadio)
	{
		if ((*g_currentRadio)->data.soundTimeRemaining)
		{
			*result = 1;
			return true;
		}
	}

	// Ambient/dynamic radio path.
	if (!g_dynamicRadios)
		return true;

	for (auto iter = g_dynamicRadios->Begin(); !iter.End(); ++iter)
	{
		DynamicRadio* dr = iter.Get();
		if (!dr)
			continue;

		if (!dr->isActive)
			continue;

		UInt32 soundKey = dr->sound.soundKey;
		UInt32 staticKey = dr->radioStaticSound.soundKey;
		if ((soundKey && soundKey != 0xFFFFFFFF) || (staticKey && staticKey != 0xFFFFFFFF))
		{
			*result = 1;
			return true;
		}
	}

	return true;
}

bool Cmd_UseAidItem_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESForm* item = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &item))
		return true;

	if (!item || item->typeID != kFormType_AlchemyItem)
		return true;

	if (!IsActorRef(thisObj))
		return true;

	Actor* actor = static_cast<Actor*>(thisObj);
	BSExtraData* xContainerChanges = (BSExtraData*)Engine::BaseExtraList_GetByType(&actor->extraDataList, kExtraData_ContainerChanges);
	ExtraContainerChanges* xChanges = static_cast<ExtraContainerChanges*>(xContainerChanges);
	if (!xChanges || !xChanges->data || !xChanges->data->objList)
		return true;

	ExtraContainerChanges::EntryData* entry = xChanges->data->objList->Find(ItemInEntryDataListMatcher(item));
	if (!entry)
		return true;

	if (!CanUseAidItemVanilla(actor, item))
		return true;

	if (g_eventInterface && g_inventoryRefCreateEntry)
	{
		ExtraDataList* xData = nullptr;
		if (entry->extendData)
		{
			auto it = entry->extendData->Begin();
			if (!it.End())
				xData = it.Get();
		}
		TESObjectREFR* invRef = g_inventoryRefCreateEntry(thisObj, entry->type, entry->countDelta, xData);
		if (invRef && !CanUseItemRef(invRef))
			return true;
	}

	ThisStdCall(0x88C650, actor, item, 1, 0, 1, 0, 1); //Actor::EquipItem
	*result = 1;
	return true;
}

bool Cmd_SetCreatureCombatSkill_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 value = 0;
	TESForm* form = nullptr;

	if (!ExtractArgs(EXTRACT_ARGS, &value, &form)) return true;

	if (!form && thisObj && thisObj->baseForm)
		form = thisObj->baseForm;

	if (!form || form->typeID != kFormType_Creature) return true;

	TESCreature* creature = (TESCreature*)form;
	creature->combatSkill = (value > 255) ? 255 : (UInt8)value;
	*result = 1;

	return true;
}

static ActorProcessManager* g_actorProcessManager = (ActorProcessManager*)0x11E0E80;
typedef void (__thiscall *_ActorResurrect)(Actor*, bool, bool, bool);
static const _ActorResurrect ActorResurrect = (_ActorResurrect)0x89F780;
static BSSpinLock* g_processListsActorLock = (BSSpinLock*)0x11F11A0;
typedef void* (__cdecl *_FormHeap_Allocate)(UInt32);
typedef void (__cdecl *_FormHeap_Free)(void*);
typedef void (__thiscall *_BaseExtraList_Copy)(void*, void*);
static const _FormHeap_Allocate s_formHeapAllocate = (_FormHeap_Allocate)0x401000;
static const _FormHeap_Free s_formHeapFree = (_FormHeap_Free)0x401030;
static const _BaseExtraList_Copy BaseExtraList_Copy = (_BaseExtraList_Copy)0x411EC0;

static void** g_modelLoader = (void**)0x11C3B3C;
typedef void (__thiscall *_ModelLoader_QueueReference)(void*, TESObjectREFR*, UInt32, bool);
static const _ModelLoader_QueueReference ModelLoader_QueueReference = (_ModelLoader_QueueReference)0x444850;
typedef NiNode* (__thiscall *_TESObjectREFR_Get3D)(TESObjectREFR*);
static const _TESObjectREFR_Get3D TESObjectREFR_Get3D = (_TESObjectREFR_Get3D)0x43FCD0;
static constexpr UInt32 kExtraDataListVtbl = 0x010143E8;
static constexpr UInt32 kExtraContainerChangesVtbl = 0x01015BB8;

enum ResurrectActorExFlags : UInt32
{
	kResurrectActorEx_ResetInventory = 1 << 0,
};

struct ResurrectActorExTrace
{
	UInt32 refID;
	UInt32 flags;
	UInt32 frameIndex;
	float lastPosZ;
};

struct ResurrectActorExEntrySnapshot
{
	TESForm* type = nullptr;
	SInt32 countDelta = 0;
	std::vector<ExtraDataList*> extraLists;
};

struct ResurrectActorExInventorySnapshot
{
	float unk2 = 0.0f;
	float unk3 = 0.0f;
	UInt8 byte10 = 0;
	std::vector<ResurrectActorExEntrySnapshot> entries;
};

static std::vector<ResurrectActorExTrace> s_resurrectActorExTraces;
static constexpr UInt32 kResurrectActorExTraceFrames = 15;

static ParamInfo kParams_ResurrectActorEx[1] = {
	{ "flags", kParamType_Integer, 1 },
};

DEFINE_COMMAND_PLUGIN(ResurrectActorEx, "Resurrect actor with flags: 1=reset inventory", 1, 1, kParams_ResurrectActorEx);
DEFINE_COMMAND_PLUGIN(ResurrectAll, "Resurrects all dead actors in high process", 0, 0, nullptr);

static void TESObjectREFR_Set3D(TESObjectREFR* ref, void* niNode, bool unloadArt)
{
	if (!ref) return;
	auto* vtbl = *(UInt32**)ref;
	if (!vtbl) return;
	auto fn = reinterpret_cast<void(__thiscall*)(TESObjectREFR*, void*, bool)>(vtbl[0x1CC / 4]);
	fn(ref, niNode, unloadArt);
}

static bool BaseExtraList_HasType(const BaseExtraList* list, UInt32 type)
{
	if (!list) return false;
	UInt32 index = (type >> 3);
	UInt8 bitMask = 1 << (type % 8);
	return (list->m_presenceBitfield[index] & bitMask) != 0;
}

static void BaseExtraList_MarkType(BaseExtraList* list, UInt32 type, bool cleared)
{
	if (!list) return;
	UInt32 index = (type >> 3);
	UInt8 bitMask = 1 << (type % 8);
	UInt8& flag = list->m_presenceBitfield[index];
	if (cleared)
		flag &= ~bitMask;
	else
		flag |= bitMask;
}

static BSExtraData* BaseExtraList_GetByTypeLocal(BaseExtraList* list, UInt32 type)
{
	if (!list || !BaseExtraList_HasType(list, type)) return nullptr;
	for (BSExtraData* traverse = list->m_data; traverse; traverse = traverse->next)
		if (traverse->type == type)
			return traverse;
	return nullptr;
}

static bool BaseExtraList_RemoveLocal(BaseExtraList* list, BSExtraData* toRemove, bool freeData)
{
	if (!list || !toRemove || !BaseExtraList_HasType(list, toRemove->type))
		return false;

	bool removed = false;
	if (list->m_data == toRemove)
	{
		list->m_data = toRemove->next;
		removed = true;
	}
	else
	{
		for (BSExtraData* traverse = list->m_data; traverse; traverse = traverse->next)
		{
			if (traverse->next == toRemove)
			{
				traverse->next = toRemove->next;
				removed = true;
				break;
			}
		}
	}

	if (!removed)
		return false;

	BaseExtraList_MarkType(list, toRemove->type, true);
	if (freeData)
		s_formHeapFree(toRemove);
	return true;
}

static bool BaseExtraList_RemoveByTypeLocal(BaseExtraList* list, UInt32 type, bool freeData)
{
	return BaseExtraList_RemoveLocal(list, BaseExtraList_GetByTypeLocal(list, type), freeData);
}

static bool BaseExtraList_AddLocal(BaseExtraList* list, BSExtraData* toAdd)
{
	if (!list || !toAdd || BaseExtraList_HasType(list, toAdd->type))
		return false;

	toAdd->next = list->m_data;
	list->m_data = toAdd;
	BaseExtraList_MarkType(list, toAdd->type, false);
	return true;
}

template <class TList, class TItem>
static void AppendListItem(TList* list, TItem* item)
{
	if (!list || !item) return;

	using Node = typename TList::_Node;
	Node* head = list->Head();
	if (!head->item)
	{
		head->item = item;
		head->next = nullptr;
		return;
	}

	Node* node = head;
	while (node->next)
		node = node->next;

	Node* newNode = static_cast<Node*>(s_formHeapAllocate(sizeof(Node)));
	memset(newNode, 0, sizeof(Node));
	newNode->item = item;
	node->next = newNode;
}

static ExtraDataList* CreateEmptyExtraDataList()
{
	auto* list = static_cast<ExtraDataList*>(s_formHeapAllocate(sizeof(ExtraDataList)));
	memset(list, 0, sizeof(ExtraDataList));
	*(UInt32*)list = kExtraDataListVtbl;
	return list;
}

static ExtraContainerChanges* CreateEmptyExtraContainerChanges(TESObjectREFR* owner)
{
	auto* xChanges = static_cast<ExtraContainerChanges*>(s_formHeapAllocate(sizeof(ExtraContainerChanges)));
	memset(xChanges, 0, sizeof(ExtraContainerChanges));
	*(UInt32*)xChanges = kExtraContainerChangesVtbl;
	xChanges->type = kExtraData_ContainerChanges;

	xChanges->data = static_cast<ExtraContainerChanges::Data*>(s_formHeapAllocate(sizeof(ExtraContainerChanges::Data)));
	memset(xChanges->data, 0, sizeof(ExtraContainerChanges::Data));
	xChanges->data->owner = owner;
	return xChanges;
}

static ExtraContainerChanges::EntryDataList* CreateEmptyEntryDataList()
{
	auto* list = static_cast<ExtraContainerChanges::EntryDataList*>(s_formHeapAllocate(sizeof(ExtraContainerChanges::EntryDataList)));
	memset(list, 0, sizeof(ExtraContainerChanges::EntryDataList));
	return list;
}

static ExtraContainerChanges::ExtendDataList* CreateEmptyExtendDataList()
{
	auto* list = static_cast<ExtraContainerChanges::ExtendDataList*>(s_formHeapAllocate(sizeof(ExtraContainerChanges::ExtendDataList)));
	memset(list, 0, sizeof(ExtraContainerChanges::ExtendDataList));
	return list;
}

static ExtraContainerChanges::EntryData* CreateEntryData(TESForm* form, SInt32 countDelta)
{
	auto* entry = static_cast<ExtraContainerChanges::EntryData*>(s_formHeapAllocate(sizeof(ExtraContainerChanges::EntryData)));
	memset(entry, 0, sizeof(ExtraContainerChanges::EntryData));
	entry->type = form;
	entry->countDelta = countDelta;
	return entry;
}

static ExtraDataList* CloneExtraDataList(ExtraDataList* source)
{
	if (!source) return nullptr;
	auto* copy = CreateEmptyExtraDataList();
	BaseExtraList_Copy(copy, source);
	return copy;
}

static void FreeExtraDataListOwned(ExtraDataList* xData)
{
	if (!xData) return;
	for (UInt32 type = 0; type < 0xFF; type++)
		BaseExtraList_RemoveByTypeLocal(xData, type, true);
	s_formHeapFree(xData);
}

template <class TList, class TItem, class FreeItemFn>
static void FreeListOwned(TList* list, FreeItemFn&& freeItem)
{
	if (!list) return;

	using Node = typename TList::_Node;
	Node* node = list->Head();
	while (node)
	{
		Node* next = node->next;
		if (node->item)
			freeItem(node->item);
		if (node != list->Head())
				s_formHeapFree(node);
		node = next;
	}
		s_formHeapFree(list);
}

static void FreeEntryDataOwned(ExtraContainerChanges::EntryData* entry)
{
	if (!entry) return;
	if (entry->extendData)
	{
		FreeListOwned<ExtraContainerChanges::ExtendDataList, ExtraDataList>(
			entry->extendData,
			[](ExtraDataList* xData) { FreeExtraDataListOwned(xData); });
	}
	s_formHeapFree(entry);
}

static void FreeInventorySnapshot(ResurrectActorExInventorySnapshot& snapshot)
{
	for (auto& entry : snapshot.entries)
	{
		for (auto* xData : entry.extraLists)
			FreeExtraDataListOwned(xData);
		entry.extraLists.clear();
	}
	snapshot.entries.clear();
}

static ResurrectActorExInventorySnapshot CaptureInventorySnapshot(Actor* actor)
{
	ResurrectActorExInventorySnapshot snapshot;
	if (!actor) return snapshot;

	auto* xChanges = static_cast<ExtraContainerChanges*>(BaseExtraList_GetByTypeLocal(&actor->extraDataList, kExtraData_ContainerChanges));
	if (!xChanges || !xChanges->data || !xChanges->data->objList)
		return snapshot;

	snapshot.unk2 = xChanges->data->unk2;
	snapshot.unk3 = xChanges->data->unk3;
	snapshot.byte10 = xChanges->data->byte10;

	for (auto entryIter = xChanges->data->objList->Begin(); !entryIter.End(); ++entryIter)
	{
		auto* entry = entryIter.Get();
		if (!entry || !entry->type)
			continue;

		ResurrectActorExEntrySnapshot entrySnapshot;
		entrySnapshot.type = entry->type;
		entrySnapshot.countDelta = entry->countDelta;

		if (entry->extendData)
		{
			for (auto xDataIter = entry->extendData->Begin(); !xDataIter.End(); ++xDataIter)
			{
				if (auto* xData = xDataIter.Get())
					entrySnapshot.extraLists.push_back(CloneExtraDataList(xData));
			}
		}

		snapshot.entries.push_back(std::move(entrySnapshot));
	}

	return snapshot;
}

static void RestoreInventorySnapshot(Actor* actor, ResurrectActorExInventorySnapshot& snapshot)
{
	if (!actor) return;

	auto* xChanges = static_cast<ExtraContainerChanges*>(BaseExtraList_GetByTypeLocal(&actor->extraDataList, kExtraData_ContainerChanges));
	if (!xChanges)
	{
		xChanges = CreateEmptyExtraContainerChanges(actor);
		BaseExtraList_AddLocal(&actor->extraDataList, xChanges);
	}
	else if (!xChanges->data)
	{
		xChanges->data = static_cast<ExtraContainerChanges::Data*>(s_formHeapAllocate(sizeof(ExtraContainerChanges::Data)));
		memset(xChanges->data, 0, sizeof(ExtraContainerChanges::Data));
	}

	if (xChanges->data->objList)
		FreeListOwned<ExtraContainerChanges::EntryDataList, ExtraContainerChanges::EntryData>(
			xChanges->data->objList,
			[](ExtraContainerChanges::EntryData* entry) { FreeEntryDataOwned(entry); });

	xChanges->data->owner = actor;
	xChanges->data->unk2 = snapshot.unk2;
	xChanges->data->unk3 = snapshot.unk3;
	xChanges->data->byte10 = snapshot.byte10;
	xChanges->data->objList = snapshot.entries.empty() ? nullptr : CreateEmptyEntryDataList();

	for (auto& snapshotEntry : snapshot.entries)
	{
		auto* entry = CreateEntryData(snapshotEntry.type, snapshotEntry.countDelta);
		if (!snapshotEntry.extraLists.empty())
		{
			entry->extendData = CreateEmptyExtendDataList();
			for (auto* xData : snapshotEntry.extraLists)
				AppendListItem(entry->extendData, xData);
			snapshotEntry.extraLists.clear();
		}
		AppendListItem(xChanges->data->objList, entry);
	}

	snapshot.entries.clear();
}

void ImperativeCommands::Update()
{
	for (size_t i = 0; i < s_resurrectActorExTraces.size();)
	{
		auto& trace = s_resurrectActorExTraces[i];
		auto* form = static_cast<TESForm*>(Engine::LookupFormByID(trace.refID));
		if (!form || !IsActorRef(reinterpret_cast<TESObjectREFR*>(form)))
		{
			s_resurrectActorExTraces.erase(s_resurrectActorExTraces.begin() + i);
			continue;
		}

		auto* actor = static_cast<Actor*>(form);
		trace.lastPosZ = actor->posZ;
		trace.frameIndex++;

		if (trace.frameIndex >= kResurrectActorExTraceFrames)
		{
			s_resurrectActorExTraces.erase(s_resurrectActorExTraces.begin() + i);
			continue;
		}

		++i;
	}

}

void ImperativeCommands::ClearState()
{
	s_resurrectActorExTraces.clear();
	if (g_crouchLockInit == 2) {
		ScopedLock lock(&g_crouchLock);
		g_crouchDisabledActors.clear();
	}
	if (g_forceCombatTargetLockInit == 2)
	{
		ScopedLock lock(&g_forceCombatTargetLock);
		g_forcedCombatTargets.clear();
	}
}

//ForceReload - forces actor to play reload animation and refill ammo
typedef bool (__thiscall *_ActorIsDead)(Actor*, bool);
static const _ActorIsDead ActorIsDead = (_ActorIsDead)0x8844F0;

typedef char (__thiscall *_ActorReload)(Actor*, TESObjectWEAP*, UInt32, bool);
static const _ActorReload ActorReload = (_ActorReload)0x8A8420;

typedef bool (__thiscall *_ItemChangeHasWeaponMod)(void*, UInt32);
static const _ItemChangeHasWeaponMod ItemChangeHasWeaponMod = (_ItemChangeHasWeaponMod)0x4BDA70;

DEFINE_COMMAND_PLUGIN(ForceReload, "Forces actor to reload their weapon", 1, 0, nullptr);

bool Cmd_ForceReload_Execute(COMMAND_ARGS)
{
	*result = 0;

	if (!thisObj || !IsActorRef(thisObj)) return true;

	Actor* actor = (Actor*)thisObj;
	if (ActorIsDead(actor, false)) return true;

	UInt32 pProcess = *(UInt32*)((UInt8*)actor + 0x68);
	if (!pProcess) return true;

	//processLevel at +0x28, must be 0 for HighProcess
	UInt32 processLevel = *(UInt32*)(pProcess + 0x28);
	if (processLevel != 0) return true;

	if (!thisObj->GetNiNode()) return true;

	UInt32 vtable = *(UInt32*)pProcess;
	if (!vtable) return true;

	//vtable[82] = GetCurrentWeapon
	typedef UInt32 (__thiscall *GetCurrentWeapon_t)(UInt32);
	GetCurrentWeapon_t GetCurrentWeapon = (GetCurrentWeapon_t)(*(UInt32*)(vtable + 82 * 4));
	UInt32 weaponInfo = GetCurrentWeapon(pProcess);
	if (!weaponInfo) return true;

	TESObjectWEAP* weapon = (TESObjectWEAP*)(*(UInt32*)(weaponInfo + 0x08));
	if (!weapon) return true;

	//vtable[83] = GetAmmoInfo, returns AmmoInfo* with count at +0x04
	typedef UInt32 (__thiscall *GetAmmoInfo_t)(UInt32);
	GetAmmoInfo_t GetAmmoInfo = (GetAmmoInfo_t)(*(UInt32*)(vtable + 83 * 4));
	UInt32 ammoInfo = GetAmmoInfo(pProcess);

	//set current ammo count to 0 to bypass "clip is full" check in Actor::Reload
	if (ammoInfo) {
		*(SInt32*)(ammoInfo + 0x04) = 0;
	}

	bool hasExtendedMag = ItemChangeHasWeaponMod((void*)weaponInfo, 11);
	char reloadResult = ActorReload(actor, weapon, 2, hasExtendedMag);
	*result = reloadResult ? 1 : 0;

	return true;
}

bool Cmd_ResurrectAll_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 count = 0;

	PlayerCharacter* player = PlayerCharacter::GetSingleton();
	if (!player || !player->parentCell) return true;

	auto ProcessCell = [&](TESObjectCELL* cell)
	{
		if (!cell) return;
		for (auto iter = cell->objectList.Begin(); !iter.End(); ++iter)
		{
			TESObjectREFR* refr = iter.Get();
			if (!refr || refr == player) continue;

			UInt8 baseType = refr->baseForm ? refr->baseForm->typeID : 0;
			if (baseType != kFormType_Creature && baseType != kFormType_NPC) continue;

			Actor* actor = (Actor*)refr;
			if (actor->lifeState != 2) continue;

			//clear 3D first so resurrection doesn't reuse dismembered model
			TESObjectREFR_Set3D(refr, nullptr, true);

			ActorResurrect(actor, true, true, false);

			//queue model reload
			if (*g_modelLoader)
				ModelLoader_QueueReference(*g_modelLoader, refr, 1, false);

			count++;
		}
	};

	ProcessCell(player->parentCell);

	TESWorldSpace* world = player->parentCell->worldSpace;
	if (world && world->cellMap && !player->parentCell->IsInterior() && player->parentCell->coords)
	{
		SInt32 baseX = (SInt32)player->parentCell->coords->x;
		SInt32 baseY = (SInt32)player->parentCell->coords->y;

		for (SInt32 dx = -1; dx <= 1; dx++)
		{
			for (SInt32 dy = -1; dy <= 1; dy++)
			{
				if (dx == 0 && dy == 0) continue;
				UInt32 key = ((baseX + dx) << 16) | ((baseY + dy) & 0xFFFF);
				TESObjectCELL* cell = world->cellMap->Lookup(key);
				ProcessCell(cell);
			}
		}
	}

	*result = count;

	if (IsConsoleMode())
		Console_Print("ResurrectAll >> Resurrected %d actors", count);

	return true;
}

bool Cmd_ResurrectActorEx_Execute(COMMAND_ARGS)
{
	*result = 0;

	UInt32 flags = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &flags))
		return true;

	if (!IsActorRef(thisObj))
		return true;

	Actor* actor = static_cast<Actor*>(thisObj);
	const UInt32 normalizedFlags = flags & kResurrectActorEx_ResetInventory;
	const bool resetInventory = (normalizedFlags & kResurrectActorEx_ResetInventory) != 0;
	const bool has3D = TESObjectREFR_Get3D(thisObj) != nullptr;
	ResurrectActorExInventorySnapshot inventorySnapshot;

	if (!resetInventory)
	{
		inventorySnapshot = CaptureInventorySnapshot(actor);
	}

	if (has3D)
	{
		TESObjectREFR_Set3D(thisObj, nullptr, true);
	}

	{
		BSSpinLockScope actorLock(g_processListsActorLock);
		ActorResurrect(actor, true, has3D, false);
	}

	if (!resetInventory)
	{
		RestoreInventorySnapshot(actor, inventorySnapshot);
	}
	else
	{
		FreeInventorySnapshot(inventorySnapshot);
	}

	if (*g_modelLoader)
		ModelLoader_QueueReference(*g_modelLoader, thisObj, 1, false);

	s_resurrectActorExTraces.erase(
		std::remove_if(
			s_resurrectActorExTraces.begin(),
			s_resurrectActorExTraces.end(),
			[&](const ResurrectActorExTrace& trace) { return trace.refID == actor->refID; }),
		s_resurrectActorExTraces.end());
	s_resurrectActorExTraces.push_back({ actor->refID, normalizedFlags, 0, actor->posZ });

	*result = 1;
	return true;
}

//TESDataHandler::CreateFormFromID - allocates a blank form of given type
typedef TESForm* (*_CreateFormInstance)(UInt8 type);
static const _CreateFormInstance CreateFormInstance = (_CreateFormInstance)0x465110;

//DataHandler::DoAddForm - registers form in game DB, assigns runtime 0xFF formID
typedef UInt32 (__thiscall *_DataHandler_DoAddForm)(void*, TESForm*);
static const _DataHandler_DoAddForm DataHandler_DoAddForm = (_DataHandler_DoAddForm)0x4603B0;
static void** g_dataHandler = (void**)0x11C3F2C;

static ParamInfo kParams_SetRaceAlt[1] = {
	{ "race", kParamType_Race, 0 },
};

DEFINE_COMMAND_PLUGIN(SetRaceAlt, "Sets race on a per-reference basis by cloning the base NPC", 1, 1, kParams_SetRaceAlt);

bool Cmd_SetRaceAlt_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESForm* raceForm = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &raceForm))
		return true;

	if (!thisObj || !thisObj->baseForm || !raceForm)
	{
		if (IsConsoleMode())
			Console_Print("SetRaceAlt >> Need an NPC ref and a race");
		return true;
	}

	if (thisObj->baseForm->typeID != kFormType_NPC)
	{
		if (IsConsoleMode())
			Console_Print("SetRaceAlt >> Not an NPC ref");
		return true;
	}

	TESNPC* origNPC = (TESNPC*)thisObj->baseForm;
	TESRace* newRace = (TESRace*)raceForm;
	TESNPC* targetNPC = origNPC;

	//if base is already a runtime clone (0xFF prefix), reuse it
	if ((origNPC->refID >> 24) == 0xFF)
	{
		targetNPC = origNPC;
	}
	else
	{
		TESForm* cloneForm = CreateFormInstance(kFormType_NPC);
		if (!cloneForm)
		{
			if (IsConsoleMode())
				Console_Print("SetRaceAlt >> Failed to create NPC form");
			return true;
		}

		//virtual CopyFrom copies all NPC data (AI, spells, race, facegen, etc)
		cloneForm->CopyFrom(origNPC);

		if (*g_dataHandler)
			DataHandler_DoAddForm(*g_dataHandler, cloneForm);

		targetNPC = (TESNPC*)cloneForm;
		thisObj->baseForm = cloneForm;

	}

	targetNPC->race.race = newRace;

	//refresh 3D
	TESObjectREFR_Set3D(thisObj, nullptr, true);
	if (*g_modelLoader)
		ModelLoader_QueueReference(*g_modelLoader, thisObj, 1, false);

	*result = 1;

	if (IsConsoleMode())
		Console_Print("SetRaceAlt >> Set race to %s on %08X (base %08X)",
			newRace->fullName.name.m_data ? newRace->fullName.name.m_data : "???",
			thisObj->refID, targetNPC->refID);

	return true;
}

namespace ImperativeCommands {
bool Init(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;

	g_eventInterface = reinterpret_cast<EventManagerInterfaceEx*>(nvse->QueryInterface(kInterface_EventManager_v2));
	g_inventoryRefCreateEntry = nullptr;

	NVSEDataInterface* dataInterface = reinterpret_cast<NVSEDataInterface*>(nvse->QueryInterface(kInterface_Data));
	if (dataInterface)
	{
		g_inventoryRefCreateEntry = reinterpret_cast<InventoryRefCreateEntry_t>(
			dataInterface->GetFunc(kNVSEData_InventoryReferenceCreateEntry));
	}

	EnsureForceCombatTargetLockInit();
	EnsureForceCombatTargetHookInstalled();

	return true;
}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_IsRadioPlaying);
}

void RegisterCommands2(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterTypedCommand(&kCommandInfo_GetRefsSortedByDistance, kRetnType_Array);
	nvse->RegisterTypedCommand(&kCommandInfo_Duplicate, kRetnType_Form);
	nvse->RegisterTypedCommand(&kCommandInfo_GetAvailableRecipes, kRetnType_Array);
	nvse->RegisterCommand(&kCommandInfo_ChangeRadioTrack);
	nvse->RegisterCommand(&kCommandInfo_DumpCombatTarget);
	nvse->RegisterTypedCommand(&kCommandInfo_GetTargetLastSeenLocation, kRetnType_Array);
	nvse->RegisterTypedCommand(&kCommandInfo_GetTargetDetectedLocation, kRetnType_Array);
	nvse->RegisterTypedCommand(&kCommandInfo_GetTargetLastFullyVisibleLocation, kRetnType_Array);
	nvse->RegisterTypedCommand(&kCommandInfo_GetTargetInitialLocation, kRetnType_Array);
}

void RegisterCommands3(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_UseAidItem);
}

void RegisterCommands6(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_ResurrectActorEx);
}

void RegisterCommands4(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetCreatureCombatSkill);
	nvse->RegisterCommand(&kCommandInfo_ResurrectAll);
	nvse->RegisterCommand(&kCommandInfo_ForceReload);
}

void RegisterCommands5(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetRaceAlt);
}

//--- ForceCrouch / DisableCrouching ---
//
//hooks Actor::SetMovementFlag (0x8B39F0) and CombatController::SetShouldSneak
//(0x981520) at function level to cover all callers

typedef void (__thiscall *_SetShouldSneak)(void*, bool);
typedef void (__thiscall *_SetMovementFlag)(Actor*, UInt32);
static _SetShouldSneak OrigSetShouldSneak = nullptr;
static _SetMovementFlag OrigSetMovementFlag = nullptr;

//trampoline for Actor::SetMovementFlag - strips 0x400 for disabled actors
static void __fastcall Hook_SetMovementFlag(Actor* actor, void* edx, UInt32 flags) {
	if ((flags & 0x400) && IsCrouchDisabled(actor->refID))
		flags &= ~0x400;
	OrigSetMovementFlag(actor, flags);
}

//trampoline for CombatController::SetShouldSneak - forces false for disabled actors
static void __fastcall Hook_SetShouldSneak(void* cc, void* edx, bool shouldSneak) {
	if (shouldSneak) {
		Actor* owner = *(Actor**)((UInt8*)cc + 0xBC);
		if (owner && IsCrouchDisabled(owner->refID))
			shouldSneak = false;
	}
	OrigSetShouldSneak(cc, shouldSneak);
}

static UInt8 g_moveFlagTrampoline[12];
static UInt8 g_sneakTrampoline[12];

//both targets have 7-byte prologues: push ebp; mov ebp,esp; push ecx; mov [ebp-4],ecx
static bool WriteTrampoline(UInt32 target, void* hook, UInt8* trampBuf, void** origOut) {
	const UInt32 stolen = 7;
	memcpy(trampBuf, (void*)target, stolen);
	trampBuf[stolen] = 0xE9;
	*(UInt32*)(trampBuf + stolen + 1) = (target + stolen) - ((UInt32)trampBuf + stolen + 5);
	DWORD old;
	VirtualProtect(trampBuf, stolen + 5, PAGE_EXECUTE_READWRITE, &old);
	*origOut = trampBuf;
	VirtualProtect((void*)target, stolen, PAGE_EXECUTE_READWRITE, &old);
	*(UInt8*)target = 0xE9;
	*(UInt32*)(target + 1) = (UInt32)hook - target - 5;
	*(UInt8*)(target + 5) = 0x90;
	*(UInt8*)(target + 6) = 0x90;
	VirtualProtect((void*)target, stolen, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void*)target, stolen);
	return true;
}

static bool g_crouchHookInstalled = false;
static bool InstallCrouchHooks() {
	if (g_crouchHookInstalled) return true;
	EnsureCrouchLock();

	if (!WriteTrampoline(0x8B39F0, Hook_SetMovementFlag, g_moveFlagTrampoline, (void**)&OrigSetMovementFlag))
		return false;
	if (!WriteTrampoline(0x981520, Hook_SetShouldSneak, g_sneakTrampoline, (void**)&OrigSetShouldSneak))
		return false;

	g_crouchHookInstalled = true;
	return true;
}

static ParamInfo kParams_ForceCrouch[1] = {
	{"crouch", kParamType_Integer, 0},
};
DEFINE_COMMAND_PLUGIN(ForceCrouch, "Force actor to crouch (1) or stand (0)", 1, 1, kParams_ForceCrouch);

bool Cmd_ForceCrouch_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 crouch = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &crouch)) return true;
	if (!IsActorRef(thisObj)) return true;

	auto* actor = (Actor*)thisObj;
	typedef UInt32 (__thiscall *_GetFlags)(Actor*);
	auto GetMoveFlags = (_GetFlags)0x8846E0;

	if (!InstallCrouchHooks()) return true;

	void* cc = GetCombatController(actor);
	if (cc) {
		auto SetSneak = OrigSetShouldSneak ? OrigSetShouldSneak : (_SetShouldSneak)0x981520;
		SetSneak(cc, (bool)crouch);
	}
	*(UInt8*)((UInt8*)actor + 0x125) = crouch ? 1 : 0; //bForceSneak
	//read-modify-write to preserve other movement bits
	UInt32 flags = GetMoveFlags(actor);
	if (crouch)
		flags |= 0x400;
	else
		flags &= ~0x400;
	auto SetMoveFlag = OrigSetMovementFlag ? OrigSetMovementFlag : (_SetMovementFlag)0x8B39F0;
	SetMoveFlag(actor, flags);

	*result = 1;
	if (IsConsoleMode())
		Console_Print("ForceCrouch >> %s", crouch ? "crouch" : "stand");
	return true;
}

static ParamInfo kParams_DisableCrouching[1] = {
	{"disable", kParamType_Integer, 0},
};
DEFINE_COMMAND_PLUGIN(DisableCrouching, "Prevent actor from crouching (1=disable, 0=enable)", 1, 1, kParams_DisableCrouching);

bool Cmd_DisableCrouching_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 disable = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &disable)) return true;
	if (!IsActorRef(thisObj)) return true;

	if (!InstallCrouchHooks()) return true;

	auto* actor = (Actor*)thisObj;
	if (disable) {
		{
			ScopedLock lock(&g_crouchLock);
			g_crouchDisabledActors.insert(actor->refID);
		}
		//force stand immediately
		void* cc = GetCombatController(actor);
		if (cc) {
			auto SetSneak = OrigSetShouldSneak ? OrigSetShouldSneak : (_SetShouldSneak)0x981520;
			SetSneak(cc, false);
		}
		*(UInt8*)((UInt8*)actor + 0x125) = 0;
	} else {
		ScopedLock lock(&g_crouchLock);
		g_crouchDisabledActors.erase(actor->refID);
	}

	*result = 1;
	if (IsConsoleMode())
		Console_Print("DisableCrouching >> %s", disable ? "disabled" : "enabled");
	return true;
}

void RegisterCommands7(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_ForceCrouch);
	nvse->RegisterCommand(&kCommandInfo_DisableCrouching);
}

//--- SetOnContactWatch / GetOnContactWatch ---

}

#include "handlers/OnContactHandler.h"

namespace ImperativeCommands {

static ParamInfo kParams_ContactWatch[1] = {
	{"watch", kParamType_Integer, 0},
};
DEFINE_COMMAND_PLUGIN(SetOnContactWatch, "Enable/disable contact event watching for a ref", 1, 1, kParams_ContactWatch);

bool Cmd_SetOnContactWatch_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 watch = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &watch)) return true;
	if (!thisObj) return true;

	if (watch)
		OnContactHandler::AddWatch(thisObj->refID);
	else
		OnContactHandler::RemoveWatch(thisObj->refID);

	*result = 1;
	if (IsConsoleMode())
		Console_Print("SetOnContactWatch >> %s (0x%08X)", watch ? "watching" : "unwatching", thisObj->refID);
	return true;
}

DEFINE_COMMAND_PLUGIN(GetOnContactWatch, "Check if a ref is being watched for contacts", 1, 0, nullptr);

bool Cmd_GetOnContactWatch_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (!thisObj) return true;
	*result = OnContactHandler::IsWatched(thisObj->refID) ? 1.0 : 0.0;
	return true;
}

void RegisterCommands8(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetOnContactWatch);
	nvse->RegisterCommand(&kCommandInfo_GetOnContactWatch);
}

static ParamInfo kParams_ForceCombatTarget[1] = {
	{"target", kParamType_Actor, 1},
};
DEFINE_COMMAND_PLUGIN(ForceCombatTarget, "Force actor to target a specific combat target; pass 0 to clear", 1, 1, kParams_ForceCombatTarget);

bool Cmd_ForceCombatTarget_Execute(COMMAND_ARGS)
{
	*result = 0;

	if (!thisObj || !IsActorRef(thisObj))
		return true;

	Actor* actor = (Actor*)thisObj;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target))
		return true;

	if (!target)
	{
		ClearForcedCombatTarget(actor);
		*result = 1;
		if (IsConsoleMode())
			Console_Print("ForceCombatTarget >> cleared");
		return true;
	}

	ForceCombatTargetResult forceResult = TryForceCombatTarget(actor, target);
	if (forceResult != ForceCombatTargetResult::kSuccess)
	{
		if (IsConsoleMode())
			Console_Print("ForceCombatTarget >> failed: %s", ForceCombatTargetResultToString(forceResult));
		return true;
	}

	*result = 1;
	if (IsConsoleMode())
		Console_Print("ForceCombatTarget >> %08X", target->refID);
	return true;
}

void RegisterCommands9(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_ForceCombatTarget);
}

//RefillAmmo - adds ammo to actor's inventory and fills their clip
//ported from ShowOff-NVSE RefillPlayerAmmo, generalized for any actor
typedef double (__thiscall *_GetRegenRate)(void*, bool);
static const _GetRegenRate GetWeaponRegenRate = (_GetRegenRate)0x709430;

typedef SInt32 (__thiscall *_GetClipSize)(void*, bool);
static const _GetClipSize GetClipSize = (_GetClipSize)0x4FE160;

typedef void* (__thiscall *_GetDefaultAmmo)(void*);
static const _GetDefaultAmmo GetDefaultAmmo = (_GetDefaultAmmo)0x474920;

static ParamInfo kParams_RefillAmmo[1] = {
	{ "count", kParamType_Integer, 0 },
};

DEFINE_COMMAND_PLUGIN(RefillAmmo, "Adds ammo and fills clip for calling ref", 1, 1, kParams_RefillAmmo);

bool Cmd_RefillAmmo_Execute(COMMAND_ARGS)
{
	*result = 0;
	SInt32 count = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &count)) return true;
	if (count <= 0) return true;
	if (!thisObj || !IsActorRef(thisObj)) return true;

	Actor* actor = (Actor*)thisObj;
	if (ActorIsDead(actor, false)) return true;

	UInt32 pProcess = *(UInt32*)((UInt8*)actor + 0x68);
	if (!pProcess) return true;
	if (*(UInt32*)(pProcess + 0x28) != 0) return true; //must be HighProcess

	UInt32 vtable = *(UInt32*)pProcess;
	if (!vtable) return true;

	//vtable[82] = GetCurrentWeapon
	typedef UInt32 (__thiscall *GetCurrentWeapon_t)(UInt32);
	UInt32 weaponInfo = ((GetCurrentWeapon_t)(*(UInt32*)(vtable + 82 * 4)))(pProcess);
	if (!weaponInfo) return true;

	void* weapon = *(void**)(weaponInfo + 0x08);
	if (!weapon) return true;

	//reject regen weapons
	bool hasRegen = ItemChangeHasWeaponMod((void*)weaponInfo, 6); //kWeaponModEffect_RegenerateAmmo
	if (hasRegen && GetWeaponRegenRate(weapon, true) > 0.0)
		return true;

	bool hasExtendedClip = ItemChangeHasWeaponMod((void*)weaponInfo, 2); //kWeaponModEffect_IncreaseClipCapacity

	//vtable[83] = GetAmmoInfo
	typedef UInt32 (__thiscall *GetAmmoInfo_t)(UInt32);
	UInt32 ammoInfo = ((GetAmmoInfo_t)(*(UInt32*)(vtable + 83 * 4)))(pProcess);

	if (ammoInfo)
	{
		void* ammoForm = *(void**)(ammoInfo + 0x08);
		if (!ammoForm) return true;

		//AddItem via vtable[0x64]
		typedef void (__thiscall *AddItem_t)(void*, void*, void*, UInt32);
		((AddItem_t)(*(UInt32*)(*(UInt32*)actor + 0x64 * 4)))(actor, ammoForm, nullptr, count);

		SInt32 clipMax = GetClipSize(weapon, hasExtendedClip);
		SInt32 currentCount = *(SInt32*)(ammoInfo + 0x04);
		SInt32 toAdd = clipMax - currentCount;
		if (toAdd > count) toAdd = count;
		if (toAdd > 0) *(SInt32*)(ammoInfo + 0x04) = currentCount + toAdd;
	}
	else
	{
		//no ammo loaded, find default ammo from weapon form
		//BGSAmmoForm at TESObjectWEAP+0xA4
		void* defaultAmmo = GetDefaultAmmo((char*)weapon + 0xA4);
		if (!defaultAmmo) return true;

		typedef void (__thiscall *AddItem_t)(void*, void*, void*, UInt32);
		((AddItem_t)(*(UInt32*)(*(UInt32*)actor + 0x64 * 4)))(actor, defaultAmmo, nullptr, count);

		//force reload since weapon was empty
		ActorReload(actor, (TESObjectWEAP*)weapon, 2, hasExtendedClip);
	}

	*result = 1;
	return true;
}

void RegisterCommands10(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_RefillAmmo);
}

}
