//standalone commands that do stuff (not event handlers)

#include "ImperativeCommands.h"
#define FORMUTILS_USE_NVSE_TYPES
#include "internal/FormUtils.h"
#include "internal/EngineFunctions.h"
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
	//kNVSE EventManager - not in older NVSE headers
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

//Update3D to properly reload model after resurrection
typedef void (__thiscall *_TESObjectREFR_Set3D)(TESObjectREFR*, void*, bool);
static const _TESObjectREFR_Set3D TESObjectREFR_Set3D = (_TESObjectREFR_Set3D)0x94EB40;

static void** g_modelLoader = (void**)0x11C3B3C;
typedef void (__thiscall *_ModelLoader_QueueReference)(void*, TESObjectREFR*, UInt32, bool);
static const _ModelLoader_QueueReference ModelLoader_QueueReference = (_ModelLoader_QueueReference)0x444850;

DEFINE_COMMAND_PLUGIN(ResurrectAll, "Resurrects all dead actors in high process", 0, 0, nullptr);

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

	if (!thisObj) return true;

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

		Log("SetRaceAlt: cloned %08X -> %08X for ref %08X",
			origNPC->refID, cloneForm->refID, thisObj->refID);
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

bool ImperativeCommands_Init(void* nvsePtr)
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

void ImperativeCommands_RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_IsRadioPlaying);
}

void ImperativeCommands_RegisterCommands2(void* nvsePtr)
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

void ImperativeCommands_RegisterCommands3(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_UseAidItem);
}

void ImperativeCommands_RegisterCommands4(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetCreatureCombatSkill);
	nvse->RegisterCommand(&kCommandInfo_ResurrectAll);
	nvse->RegisterCommand(&kCommandInfo_ForceReload);
}

void ImperativeCommands_RegisterCommands5(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetRaceAlt);
}
