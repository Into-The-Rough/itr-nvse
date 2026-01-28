//standalone commands that do stuff (not event handlers)

#include "ImperativeCommands.h"
#define FORMUTILS_USE_NVSE_TYPES
#include "internal/FormUtils.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/GameForms.h"
#include "nvse/GameData.h"
#include "nvse/GameProcess.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include <vector>
#include <algorithm>
#include <cmath>

extern const _ExtractArgs ExtractArgs;
extern bool IsConsoleMode();
extern void Console_Print(const char* fmt, ...);
extern NVSEArrayVarInterface* g_arrInterface;
extern void Log(const char* fmt, ...);

using namespace FormUtils;

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
	if (world && cellDepth > 0 && !playerCell->IsInterior() && playerCell->coords)
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

typedef bool (__thiscall *_ClampToGround)(TESObjectREFR*);
static const _ClampToGround RefClampToGround = (_ClampToGround)0x576470;

typedef void (__thiscall *_SetLocationOnReference)(TESObjectREFR*, float*);
static const _SetLocationOnReference SetLocationOnReference = (_SetLocationOnReference)0x575830;

DEFINE_COMMAND_PLUGIN(ClampToGround, "Clamps the reference to the ground", 1, 0, nullptr);

bool Cmd_ClampToGround_Execute(COMMAND_ARGS)
{
	*result = 0;

	if (!thisObj)
	{
		if (IsConsoleMode())
			Console_Print("ClampToGround >> No reference selected");
		return true;
	}

	if (RefClampToGround(thisObj))
	{
		SetLocationOnReference(thisObj, &thisObj->posX);
		*result = 1;
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
static const _GetCombatController GetCombatController = (_GetCombatController)0x8A02D0;
static const _GetCombatTargetForActor GetCombatTargetForActor = (_GetCombatTargetForActor)0x9865D0;

bool Cmd_DumpCombatTarget_Execute(COMMAND_ARGS)
{
	*result = 0;

	Actor* target = nullptr;

	Log("DumpCombatTarget: ExtractArgs...");
	if (!ExtractArgs(EXTRACT_ARGS, &target))
	{
		Log("DumpCombatTarget: ExtractArgs failed");
		Console_Print("DumpCombatTarget >> ExtractArgs failed");
		return true;
	}

	Log("DumpCombatTarget: thisObj=%08X target=%08X", thisObj, target);
	if (!thisObj || !target)
	{
		Console_Print("DumpCombatTarget >> Call on observer ref with target as param");
		return true;
	}

	Actor* observer = (Actor*)thisObj;
	Log("DumpCombatTarget: observer refID=%08X", observer->refID);

	//get combat controller via direct function call
	void* combatController = GetCombatController(observer);
	Log("DumpCombatTarget: combatController=%08X", combatController);

	if (!combatController)
	{
		Console_Print("DumpCombatTarget >> Observer has no combat controller");
		return true;
	}

	//combatGroup is at offset 0x80 in CombatController
	void* combatGroup = *(void**)((UInt8*)combatController + 0x80);
	Log("DumpCombatTarget: combatGroup=%08X", combatGroup);
	if (!combatGroup)
	{
		Console_Print("DumpCombatTarget >> No combat group");
		return true;
	}

	void* combatTarget = GetCombatTargetForActor(combatGroup, target);
	Log("DumpCombatTarget: combatTarget=%08X", combatTarget);
	if (!combatTarget)
	{
		Console_Print("DumpCombatTarget >> No CombatTarget for target actor");
		return true;
	}

	Console_Print("DumpCombatTarget >> CombatTarget at %08X", combatTarget);

	//dump raw bytes
	UInt8* bytes = (UInt8*)combatTarget;
	Log("CombatTarget dump at %08X:", combatTarget);
	for (int i = 0; i < 0x68; i += 16)
	{
		Log("  +%02X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
			i,
			bytes[i+0], bytes[i+1], bytes[i+2], bytes[i+3],
			bytes[i+4], bytes[i+5], bytes[i+6], bytes[i+7],
			bytes[i+8], bytes[i+9], bytes[i+10], bytes[i+11],
			bytes[i+12], bytes[i+13], bytes[i+14], bytes[i+15]);
	}

	//try to interpret known fields
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

//helper to create position array from CombatTarget offset
static bool CreatePositionArray(COMMAND_ARGS, void* combatTarget, UInt32 offset)
{
	if (!combatTarget) return false;

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
	if (!ExtractArgs(EXTRACT_ARGS, &target)) return true;

	void* ct = GetCombatTargetData((Actor*)thisObj, target);
	CreatePositionArray(PASS_COMMAND_ARGS, ct, 0x08);
	return true;
}

bool Cmd_GetTargetDetectedLocation_Execute(COMMAND_ARGS)
{
	*result = 0;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target)) return true;

	void* ct = GetCombatTargetData((Actor*)thisObj, target);
	CreatePositionArray(PASS_COMMAND_ARGS, ct, 0x18);
	return true;
}

bool Cmd_GetTargetLastFullyVisibleLocation_Execute(COMMAND_ARGS)
{
	*result = 0;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target)) return true;

	void* ct = GetCombatTargetData((Actor*)thisObj, target);
	CreatePositionArray(PASS_COMMAND_ARGS, ct, 0x28);
	return true;
}

bool Cmd_GetTargetInitialLocation_Execute(COMMAND_ARGS)
{
	*result = 0;
	Actor* target = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &target)) return true;

	void* ct = GetCombatTargetData((Actor*)thisObj, target);
	CreatePositionArray(PASS_COMMAND_ARGS, ct, 0x38);
	return true;
}

bool ImperativeCommands_Init(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;

	nvse->SetOpcodeBase(0x4021);
	/*4021*/ nvse->RegisterTypedCommand(&kCommandInfo_GetRefsSortedByDistance, kRetnType_Array);
	/*4022*/ nvse->RegisterTypedCommand(&kCommandInfo_Duplicate, kRetnType_Form);
	/*4023*/ nvse->RegisterTypedCommand(&kCommandInfo_GetAvailableRecipes, kRetnType_Array);
	/*4024*/ nvse->RegisterCommand(&kCommandInfo_ClampToGround);
	/*4025*/ nvse->RegisterCommand(&kCommandInfo_DumpCombatTarget);
	/*4026*/ nvse->RegisterTypedCommand(&kCommandInfo_GetTargetLastSeenLocation, kRetnType_Array);
	/*4027*/ nvse->RegisterTypedCommand(&kCommandInfo_GetTargetDetectedLocation, kRetnType_Array);
	/*4028*/ nvse->RegisterTypedCommand(&kCommandInfo_GetTargetLastFullyVisibleLocation, kRetnType_Array);
	/*4029*/ nvse->RegisterTypedCommand(&kCommandInfo_GetTargetInitialLocation, kRetnType_Array);
	Log("Registered ImperativeCommands at 0x4021-0x4029");

	return true;
}
