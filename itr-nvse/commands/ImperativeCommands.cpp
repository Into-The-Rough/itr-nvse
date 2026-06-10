//standalone commands that do stuff (not event handlers)

#include "ImperativeCommands.h"
#define FORMUTILS_USE_NVSE_TYPES
#include "internal/FormUtils.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/CallTemplates.h"
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
#include <cmath>

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"
extern NVSEArrayVarInterface* g_arrInterface;

using namespace FormUtils;

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

	using InventoryRefCreateEntry_t = TESObjectREFR* (__stdcall *)(TESObjectREFR* container, TESForm* itemForm, SInt32 countDelta, ExtraDataList* xData);
	constexpr UInt32 kNVSEData_InventoryReferenceCreateEntry = 7;
	constexpr UInt32 kAVCode_PerceptionCondition = 0x19;
	constexpr UInt32 kAVCode_RightMobilityCondition = 0x1E;
	constexpr UInt32 kAVCode_IgnoreCrippledLimbs = 0x48;

	static EventManagerInterfaceEx* g_eventInterface = nullptr;
	static InventoryRefCreateEntry_t g_inventoryRefCreateEntry = nullptr;
	static Setting* g_sFullHealth = reinterpret_cast<Setting*>(0x11D2AF0);
	static BGSDefaultObjectManager** g_defaultObjectManager = reinterpret_cast<BGSDefaultObjectManager**>(0x11CA80C);

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
				Engine::QueueUIMessage(g_sFullHealth->data.str, 0, nullptr, nullptr, 2.0f, false);
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
		auto retnAlt = g_eventInterface->DispatchEventAlt(
			"ShowOff:OnPreActivateInventoryItemAlt",
			resultCallback,
			&shouldActivate,
			player,
			invRef->baseForm,
			invRef,
			&shouldActivate,
			static_cast<UInt32>(0),
			isSpecialActivation);

		//unknown events never invoke the callback, only treat the result as authoritative if either event exists
		if (retn == EventManagerInterfaceEx::kRetn_UnknownEvent &&
			retnAlt == EventManagerInterfaceEx::kRetn_UnknownEvent)
			return true;

		return shouldActivate != 0;
	}
}

static ParamInfo kParams_GetRefsSortedByDistance[7] = {
	{ "maxDistance",      kParamType_Float,   0 },
	{ "formType",         kParamType_Integer, 1 },
	{ "cellDepth",        kParamType_Integer, 1 },
	{ "includeTakenRefs", kParamType_Integer, 1 },
	{ "maxHeadingAngle",  kParamType_Float,   1 },
	{ "limit",            kParamType_Integer, 1 },
	{ "baseForm",         kParamType_AnyForm, 1 },
};

DEFINE_COMMAND_PLUGIN(GetRefsSortedByDistance, "Returns array of refs sorted by distance from player", 0, 7, kParams_GetRefsSortedByDistance);

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
	float maxHeadingAngle = 0;
	UInt32 limit = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &maxDistance, &formType, &cellDepth, &includeTakenRefs, &maxHeadingAngle, &limit, &baseForm))
		return true;

	if (maxDistance <= 0)
	{
		if (IsConsoleMode()) Console_Print("GetRefsSortedByDistance >> maxDistance must be > 0");
		return true;
	}

	PlayerCharacter* player = PlayerCharacter::GetSingleton();
	if (!player || !player->parentCell) return true;

	float maxDistSq = maxDistance * maxDistance;

	constexpr float kPi = 3.14159265358979323846f;
	const bool useHeading = maxHeadingAngle > 0.0f && maxHeadingAngle < 180.0f;
	const float maxHeadingRad = maxHeadingAngle * (kPi / 180.0f);
	const float playerRotZ = player->rotZ;

	struct RefWithDist {
		TESObjectREFR* ref;
		float distance;
	};
	std::vector<RefWithDist> refs;
	if (limit > 0) refs.reserve(limit);

	auto HeapCmp = [](const RefWithDist& a, const RefWithDist& b) { return a.distance < b.distance; };

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

			if (useHeading)
			{
				//FNV: rotZ measured from +Y, clockwise (matches atan2(dx,dy))
				float dx = refr->posX - player->posX;
				float dy = refr->posY - player->posY;
				float delta = atan2f(dx, dy) - playerRotZ;
				while (delta > kPi)  delta -= 2.0f * kPi;
				while (delta < -kPi) delta += 2.0f * kPi;
				if (fabsf(delta) > maxHeadingRad) continue;
			}

			float dist = sqrtf(distSq);

			if (limit > 0)
			{
				if (refs.size() < limit)
				{
					refs.push_back({ refr, dist });
					std::push_heap(refs.begin(), refs.end(), HeapCmp);
				}
				else if (dist < refs.front().distance)
				{
					std::pop_heap(refs.begin(), refs.end(), HeapCmp);
					refs.back() = { refr, dist };
					std::push_heap(refs.begin(), refs.end(), HeapCmp);
				}
			}
			else
			{
				refs.push_back({ refr, dist });
			}
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

	DataHandler* dataHandler = *(DataHandler**)g_dataHandlerPtr;
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

typedef void* (__thiscall *_GetCombatController)(Actor*);
typedef void* (__thiscall *_GetCombatTargetForActor)(void* combatGroup, Actor* target);
typedef bool (__thiscall *_ActorIsDead)(Actor*, bool);
static const _GetCombatController GetCombatController = (_GetCombatController)0x8A02D0;
static const _GetCombatTargetForActor GetCombatTargetForActor = (_GetCombatTargetForActor)0x9865D0;
static const _ActorIsDead ActorIsDead = (_ActorIsDead)0x8844F0;

#ifdef _DEBUG
//debug command to dump CombatTarget memory for offset verification
static ParamInfo kParams_DumpCombatTarget[1] = {
	{ "target", kParamType_Actor, 0 },
};

DEFINE_COMMAND_PLUGIN(DumpCombatTarget, "Dumps CombatTarget structure for offset verification", 1, 1, kParams_DumpCombatTarget);

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

	Console_Print("DumpCombatTarget >> CombatTarget at %p", combatTarget);

	//try to interpret known fields
	UInt8* bytes = (UInt8*)combatTarget;
	Actor** pTarget = (Actor**)bytes;
	Console_Print("  +00 pTarget: %p (expected %p)", *pTarget, target);

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
#endif

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

DEFINE_COMMAND_PLUGIN(UseAidItem, "Uses an aid item (AlchemyItem) on the calling actor", 1, 1, kParams_UseAidItem);
DEFINE_COMMAND_PLUGIN(SetCreatureCombatSkill, "Sets creature combat skill (0-255)", 0, 2, kParams_SetCreatureCombatSkill);

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

static void** g_modelLoader = (void**)0x11C3B3C;
typedef void (__thiscall *_ModelLoader_QueueReference)(void*, TESObjectREFR*, UInt32, bool);
static const _ModelLoader_QueueReference ModelLoader_QueueReference = (_ModelLoader_QueueReference)0x444850;

static void TESObjectREFR_Set3D(TESObjectREFR* ref, void* niNode, bool unloadArt)
{
	if (!ref) return;
	auto* vtbl = *(UInt32**)ref;
	if (!vtbl) return;
	auto fn = reinterpret_cast<void(__thiscall*)(TESObjectREFR*, void*, bool)>(vtbl[0x1CC / 4]);
	fn(ref, niNode, unloadArt);
}

//ForceReload - forces actor to play reload animation and refill ammo
typedef char (__thiscall *_ActorReload)(Actor*, TESObjectWEAP*, UInt32, bool);
static const _ActorReload ActorReload = (_ActorReload)0x8A8420;

typedef bool (__thiscall *_ItemChangeHasWeaponMod)(void*, UInt32);
static const _ItemChangeHasWeaponMod ItemChangeHasWeaponMod = (_ItemChangeHasWeaponMod)0x4BDA70;

//Actor::Reload calls TESObjectWEAP::GetEquippedAmmo which returns HighProcess->pCurrentAmmo
//if set, else the weapon's default Ammo (not the AmmoList). On save-load the actor's
//pCurrentAmmo is null, so a reload fails when the only inventory ammo is an override from
//the weapon's AmmoList (e.g. Max Charge packs). Pre-seed pCurrentAmmo with the correct
//entry the same way InventoryChanges::GetAmmoForWeapon resolves it.
typedef void* (__cdecl *_GetInventoryChanges)(TESObjectREFR*);
static const _GetInventoryChanges GetInventoryChanges = (_GetInventoryChanges)0x4BF220;

typedef TESForm* (__thiscall *_GetAmmoForWeapon)(void*, TESObjectWEAP*, bool*);
static const _GetAmmoForWeapon GetAmmoForWeapon = (_GetAmmoForWeapon)0x4C7300;

typedef void* (__thiscall *_GetInventoryItem)(void*, TESForm*, UInt32);
static const _GetInventoryItem GetInventoryItem = (_GetInventoryItem)0x4D0650;

DEFINE_COMMAND_PLUGIN(ForceReload, "Forces actor to reload their weapon", 1, 0, nullptr);

bool Cmd_ForceReload_Execute(COMMAND_ARGS)
{
	*result = 0;

	if (!thisObj || !IsActorRef(thisObj)) return true;

	Actor* actor = (Actor*)thisObj;
	if (ActorIsDead(actor, false)) return true;

	UInt32 pProcess = *(UInt32*)((UInt8*)actor + 0x68);
	if (!pProcess) return true;

	UInt32 processLevel = *(UInt32*)(pProcess + 0x28);
	if (processLevel != 0) return true;

	if (!thisObj->GetNiNode()) return true;

	UInt32 vtable = *(UInt32*)pProcess;
	if (!vtable) return true;

	typedef UInt32 (__thiscall *GetCurrentWeapon_t)(UInt32);
	GetCurrentWeapon_t GetCurrentWeapon = (GetCurrentWeapon_t)(*(UInt32*)(vtable + 82 * 4));
	UInt32 weaponInfo = GetCurrentWeapon(pProcess);
	if (!weaponInfo) return true;

	TESObjectWEAP* weapon = (TESObjectWEAP*)(*(UInt32*)(weaponInfo + 0x08));
	if (!weapon) return true;

	typedef UInt32 (__thiscall *GetAmmoInfo_t)(UInt32);
	GetAmmoInfo_t GetAmmoInfo = (GetAmmoInfo_t)(*(UInt32*)(vtable + 83 * 4));
	UInt32 ammoInfo = GetAmmoInfo(pProcess);

	void* invChanges = nullptr;
	TESForm* correctAmmo = nullptr;
	bool hasAmmo = false; //out-param sink, engine writes it unconditionally
	if (!ammoInfo) {
		invChanges = GetInventoryChanges(thisObj);
	}
	if (invChanges) {
		correctAmmo = GetAmmoForWeapon(invChanges, weapon, &hasAmmo);
	}

	if (!ammoInfo && correctAmmo && invChanges) {
		void* newEntry = GetInventoryItem(invChanges, correctAmmo, 0);
		if (newEntry) {
			typedef void (__thiscall *SetAmmoInfo_t)(UInt32, void*);
			SetAmmoInfo_t SetAmmoInfo = (SetAmmoInfo_t)(*(UInt32*)(vtable + 90 * 4));
			*(SInt32*)((UInt8*)newEntry + 0x04) = 0;
			SetAmmoInfo(pProcess, newEntry);
		}
	}
	else if (ammoInfo) {
		*(SInt32*)(ammoInfo + 0x04) = 0;
	}

	bool hasExtendedMag = ItemChangeHasWeaponMod((void*)weaponInfo, 11);
	char reloadResult = ActorReload(actor, weapon, 2, hasExtendedMag);
	*result = reloadResult ? 1 : 0;

	return true;
}

//TESDataHandler::CreateFormFromID - allocates a blank form of given type
typedef TESForm* (*_CreateFormInstance)(UInt8 type);
static const _CreateFormInstance CreateFormInstance = (_CreateFormInstance)0x465110;

//DataHandler::DoAddForm - registers form in game DB, assigns runtime 0xFF formID
typedef UInt32 (__thiscall *_DataHandler_DoAddForm)(void*, TESForm*);
static const _DataHandler_DoAddForm DataHandler_DoAddForm = (_DataHandler_DoAddForm)0x4603B0;

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

		if (*g_dataHandlerPtr)
			DataHandler_DoAddForm(*g_dataHandlerPtr, cloneForm);

		targetNPC = (TESNPC*)cloneForm;
		//runtime 0xFF clone is not serialized, the swap does not survive save/load,
		//callers must reapply on load (documented in FEATURES.md)
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

	return true;
}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterTypedCommand(&kCommandInfo_GetRefsSortedByDistance, kRetnType_Array);
	nvse->RegisterTypedCommand(&kCommandInfo_Duplicate, kRetnType_Form);
	nvse->RegisterTypedCommand(&kCommandInfo_GetAvailableRecipes, kRetnType_Array);
}

void RegisterCommands2(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
#ifdef _DEBUG
	nvse->RegisterCommand(&kCommandInfo_DumpCombatTarget);
#endif
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

void RegisterCommands4(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetCreatureCombatSkill);
}

void RegisterCommands5(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_ForceReload);
}

void RegisterCommands6(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetRaceAlt);
}

//--- SetOnContactWatch / GetOnContactWatch ---

}

#include "handlers/OnContactHandler.h"

namespace ImperativeCommands {

static ParamInfo kParams_ContactWatch[2] = {
	{"watch", kParamType_Integer, 0},
	{"target", kParamType_AnyForm, 1},
};
DEFINE_COMMAND_PLUGIN(SetOnContactWatch, "Enable/disable contact event watching for a ref or base form", 0, 2, kParams_ContactWatch);

bool Cmd_SetOnContactWatch_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 watch = 0;
	TESForm* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &watch, &target)) return true;

	UInt32 formID = 0;
	bool targetIsBaseForm = false;
	if (target) {
		if (target->IsReference())
			formID = target->refID;
		else {
			formID = target->refID;
			targetIsBaseForm = true;
		}
	} else {
		if (!thisObj) return true;
		formID = thisObj->refID;
	}

	if (!formID) return true;

	if (targetIsBaseForm) {
		if (watch)
			OnContactHandler::AddBaseWatch(formID);
		else
			OnContactHandler::RemoveBaseWatch(formID);
	} else {
		if (watch)
			OnContactHandler::AddWatch(formID);
		else
			OnContactHandler::RemoveWatch(formID);
	}

	*result = 1;
	if (IsConsoleMode())
		Console_Print("SetOnContactWatch >> %s %s (0x%08X)",
			watch ? "watching" : "unwatching",
			targetIsBaseForm ? "base form" : "ref",
			formID);
	return true;
}

static ParamInfo kParams_GetContactWatch[1] = {
	{"target", kParamType_AnyForm, 1},
};
DEFINE_COMMAND_PLUGIN(GetOnContactWatch, "Check if a ref or base form is being watched for contacts", 0, 1, kParams_GetContactWatch);

bool Cmd_GetOnContactWatch_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target)) return true;

	if (target) {
		if (target->IsReference())
			*result = OnContactHandler::IsRefWatched(target->refID) ? 1.0 : 0.0;
		else
			*result = OnContactHandler::IsBaseWatched(target->refID) ? 1.0 : 0.0;
		return true;
	}

	if (!thisObj) return true;
	*result = OnContactHandler::IsRefWatched(thisObj->refID) ? 1.0 : 0.0;
	return true;
}

void RegisterCommands7(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetOnContactWatch);
	nvse->RegisterCommand(&kCommandInfo_GetOnContactWatch);
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

void RegisterCommands8(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_RefillAmmo);
}

}
