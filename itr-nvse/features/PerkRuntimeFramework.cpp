#include "PerkRuntimeFramework.h"

#include "internal/CallTemplates.h"
#include "internal/globals.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameData.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"
#include "nvse/PluginAPI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern const _ExtractArgs ExtractArgs;
extern NVSEArrayVarInterface* g_arrInterface;
extern void Log(const char* fmt, ...);

char* CopyCString(const char* src)
{
	using FormHeapAllocate_t = void* (__cdecl*)(UInt32);
	const auto FormHeapAllocate = reinterpret_cast<FormHeapAllocate_t>(0x401000);

	const size_t size = src ? strlen(src) : 0;
	auto* result = static_cast<char*>(FormHeapAllocate(static_cast<UInt32>(size + 1)));
	if (!result)
		return nullptr;

	result[size] = 0;
	if (size)
		memcpy(result, src, size);
	return result;
}

namespace
{
	using Array = NVSEArrayVarInterface::Array;
	using Element = NVSEArrayVarInterface::Element;

	constexpr UInt32 kDataHandler = 0x011C3F2C;
	constexpr UInt32 kConditionFlag_OR = 0x01;
	constexpr UInt32 kConditionFlag_UseGlobal = 0x04;

	constexpr UInt32 kCond_GetActorValue = 14;
	constexpr UInt32 kCond_GetItemCount = 47;
	constexpr UInt32 kCond_GetLevel = 80;
	constexpr UInt32 kCond_GetWeaponAnimType = 108;
	constexpr UInt32 kCond_IsWeaponSkillType = 109;
	constexpr UInt32 kCond_GetEquipped = 182;
	constexpr UInt32 kCond_GetBaseActorValue = 277;
	constexpr UInt32 kCond_GetIsID = 72;
	constexpr UInt32 kCond_IsInList = 372;
	constexpr UInt32 kCond_IsWeaponInList = 399;
	constexpr UInt32 kCond_HasPerk = 449;
	constexpr UInt32 kCond_GetPermanentActorValue = 495;
	constexpr UInt8 kMaxEntryPointFilterTabs = 10;

	constexpr UInt16 kPerkEntryType_Quest = 0x0C24;
	constexpr UInt16 kPerkEntryType_Ability = 0x0B27;
	constexpr UInt16 kPerkEntryType_EntryPoint = 0x0D16;
	constexpr UInt32 kVtbl_BGSEntryPointPerkEntry = 0x1046D0C;

	constexpr UInt32 kFlag_IncludeHidden = 1 << 0;
	constexpr UInt32 kFlag_IncludeNonPlayable = 1 << 1;
	constexpr UInt32 kFlag_IncludeTraits = 1 << 2;
	constexpr UInt32 kFlag_IncludeOwnedMaxRank = 1 << 3;
	constexpr UInt32 kFlag_IncludeNonLevelUp = 1 << 4;
	constexpr UInt32 kFlag_IncludeContextual = 1 << 5;
	constexpr UInt32 kFlag_IncludeUnknown = 1 << 6;
	constexpr UInt32 kFlag_AllEntryPoints = 1 << 7;
	constexpr UInt32 kConsoleDumpLimit = 32;

	enum class Source : UInt8
	{
		PerkDataRequirement,
		TopLevelRequirement,
		EntryPointFilter,
	};

	enum class Kind : UInt8
	{
		OwnedMaxRank,
		Trait,
		Hidden,
		NonPlayable,
		NonLevelUp,
		MenuFiltered,
		EngineRequirements,
		Scripted,
		ActorValue,
		BaseActorValue,
		PermanentActorValue,
		Level,
		HasPerk,
		ItemCount,
		FormID,
		FormList,
		WeaponList,
		Equipped,
		WeaponAnimType,
		WeaponSkillType,
		Opaque,
	};

	enum class EvalState : UInt8
	{
		Pass,
		Fail,
		Indeterminate,
	};

	struct ConditionDescriptor
	{
		Source source = Source::TopLevelRequirement;
		Condition* raw = nullptr;
		UInt32 perkFormID = 0;
		UInt32 rawFunctionId = 0;
		UInt32 commandOpcode = 0;
		UInt32 argValue = 0;
		UInt32 rank = 0;
		SInt32 entryPoint = -1;
		SInt32 entryFunction = -1;
		SInt32 tabIndex = -1;
		UInt32 runOn = 0;
		TESObjectREFR* runOnReference = nullptr;
		UInt8 comparison = 0;
		float comparisonValue = 0.0f;
		TESGlobal* comparisonGlobal = nullptr;
		TESForm* argForm = nullptr;
		bool isOr = false;
		bool usesGlobalComparison = false;
		Kind kind = Kind::Opaque;
	};

	struct RequirementGroup
	{
		bool any = false;
		std::vector<ConditionDescriptor> descriptors;
	};

	struct EntryDescriptor
	{
		BGSPerkEntry* raw = nullptr;
		bool isEntryPoint = false;
		UInt32 rank = 0;
		UInt16 type = 0;
		SInt32 entryPoint = -1;
		SInt32 function = -1;
		UInt8 conditionTabs = 0;
		std::vector<ConditionDescriptor> filters;
	};

	struct PerkDescriptor
	{
		BGSPerk* perk = nullptr;
		UInt32 formID = 0;
		std::string name;
		UInt32 minLevel = 0;
		UInt32 maxRank = 0;
		bool trait = false;
		bool playable = false;
		bool hidden = false;
		std::vector<RequirementGroup> requirements;
		std::vector<EntryDescriptor> entries;
	};

	struct StateDelta
	{
		bool hasLevel = false;
		double level = 0.0;
		std::unordered_map<UInt32, double> av;
		std::unordered_map<UInt32, double> avDelta;
		std::unordered_map<UInt32, double> itemsAdded;
		std::unordered_map<UInt32, double> itemsRemoved;
		std::unordered_set<UInt32> perksAdded;
		std::unordered_set<UInt32> perksRemoved;
	};

	struct EvalContext
	{
		PlayerCharacter* player = nullptr;
		StateDelta delta;
	};

	struct EvaluatedCondition
	{
		EvalState state = EvalState::Indeterminate;
		double currentValue = 0.0;
		double requiredValue = 0.0;
		double missingValue = 0.0;
	};

	struct EvaluatedGroup
	{
		EvalState state = EvalState::Pass;
		std::vector<std::pair<ConditionDescriptor, EvaluatedCondition>> failed;
		std::vector<ConditionDescriptor> indeterminate;
	};

	struct EligibilityResult
	{
		BGSPerk* perk = nullptr;
		std::string name;
		UInt32 ownedRank = 0;
		UInt32 nextRank = 0;
		UInt32 maxRank = 0;
		bool eligible = false;
		std::string status = "unknown";
		std::vector<std::pair<ConditionDescriptor, EvaluatedCondition>> blockers;
		std::vector<ConditionDescriptor> indeterminate;
	};

	struct EntryMatch
	{
		const EntryDescriptor* entry = nullptr;
		std::string match;
		bool contextual = false;
		bool unknown = false;
	};

	struct ApplicablePerkResult
	{
		EligibilityResult eligibility;
		std::vector<EntryMatch> matches;
		bool relevant = false;
		bool owned = false;
		bool contextual = false;
		bool unknown = false;
		std::string status;
	};

	std::vector<PerkDescriptor> g_perks;
	std::unordered_map<UInt32, size_t> g_perksByFormID;

	const char* SourceName(Source source)
	{
		switch (source)
		{
			case Source::PerkDataRequirement: return "PerkDataRequirement";
			case Source::TopLevelRequirement: return "TopLevelRequirement";
			default: return "EntryPointFilter";
		}
	}

	const char* KindName(Kind kind)
	{
		switch (kind)
		{
			case Kind::OwnedMaxRank: return "OwnedMaxRank";
			case Kind::Trait: return "Trait";
			case Kind::Hidden: return "Hidden";
			case Kind::NonPlayable: return "NonPlayable";
			case Kind::NonLevelUp: return "NonLevelUp";
			case Kind::MenuFiltered: return "MenuFiltered";
			case Kind::EngineRequirements: return "EngineRequirements";
			case Kind::Scripted: return "Scripted";
			case Kind::ActorValue: return "ActorValue";
			case Kind::BaseActorValue: return "BaseActorValue";
			case Kind::PermanentActorValue: return "PermanentActorValue";
			case Kind::Level: return "Level";
			case Kind::HasPerk: return "HasPerk";
			case Kind::ItemCount: return "ItemCount";
			case Kind::FormID: return "FormID";
			case Kind::FormList: return "FormList";
			case Kind::WeaponList: return "WeaponList";
			case Kind::Equipped: return "Equipped";
			case Kind::WeaponAnimType: return "WeaponAnimType";
			case Kind::WeaponSkillType: return "WeaponSkillType";
			default: return "Opaque";
		}
	}

	const char* FunctionName(UInt32 functionId)
	{
		switch (functionId)
		{
			case 0: return "PerkData";
			case kCond_GetActorValue: return "GetActorValue";
			case kCond_GetItemCount: return "GetItemCount";
			case kCond_GetLevel: return "GetLevel";
			case kCond_GetWeaponAnimType: return "GetWeaponAnimType";
			case kCond_IsWeaponSkillType: return "IsWeaponSkillType";
			case kCond_GetEquipped: return "GetEquipped";
			case kCond_GetBaseActorValue: return "GetBaseActorValue";
			case kCond_GetIsID: return "GetIsID";
			case kCond_IsInList: return "IsInList";
			case kCond_IsWeaponInList: return "IsWeaponInList";
			case kCond_HasPerk: return "HasPerk";
			case kCond_GetPermanentActorValue: return "GetPermanentActorValue";
			default: return "Opaque";
		}
	}

	const char* RunOnName(UInt32 runOn)
	{
		switch (runOn)
		{
			case 0: return "Subject";
			case 1: return "Target";
			case 2: return "Reference";
			case 3: return "CombatTarget";
			case 4: return "LinkedReference";
			default: return "Unknown";
		}
	}

	const char* EntryPointName(SInt32 entryPoint)
	{
		switch (entryPoint)
		{
			case 0: return "CalculateWeaponDamage";
			case 1: return "CalculateMyCriticalHitChance";
			case 2: return "CalculateMyCriticalHitDamage";
			case 3: return "CalculateWeaponAttackAPCost";
			case 4: return "CalculateMineExplodeChance";
			case 5: return "AdjustRangePenalty";
			case 6: return "AdjustLimbDamage";
			case 7: return "CalculateWeaponRange";
			case 8: return "CalculateToHitChance";
			case 9: return "AdjustExperiencePoints";
			case 10: return "AdjustGainedSkillPoints";
			case 11: return "AdjustBookSkillPoints";
			case 12: return "ModifyRecoveredHealth";
			case 13: return "CalculateInventoryAPCost";
			case 14: return "GetDisposition";
			case 15: return "GetShouldAttack";
			case 16: return "GetShouldAssist";
			case 17: return "CalculateBuyPrice";
			case 18: return "GetBadKarma";
			case 19: return "GetGoodKarma";
			case 20: return "IgnoreLockedTerminal";
			case 21: return "AddLeveledListOnDeath";
			case 22: return "GetMaxCarryWeight";
			case 23: return "ModifyAddictionChance";
			case 24: return "ModifyAddictionDuration";
			case 25: return "ModifyPositiveChemDuration";
			case 26: return "AdjustDrinkingRadiation";
			case 27: return "Activate";
			case 28: return "MysteriousStranger";
			case 29: return "HasParalyzingPalm";
			case 30: return "HackingScienceBonus";
			case 31: return "IgnoreRunningDuringDetection";
			case 32: return "IgnoreBrokenLock";
			case 33: return "HasConcentratedFire";
			case 34: return "CalculateGunSpread";
			case 35: return "PlayerKillAPReward";
			case 36: return "ModifyEnemyCriticalHitChance";
			case 37: return "ReloadSpeed";
			case 38: return "EquipSpeed";
			case 39: return "ActionPointRegen";
			case 40: return "ActionPointCost";
			case 41: return "MissFortune";
			case 42: return "ModifyRunSpeed";
			case 43: return "ModifyAttackSpeed";
			case 44: return "ModifyRadiationConsumed";
			case 45: return "HasPipHacker";
			case 46: return "HasMeltdown";
			case 47: return "SeeEnemyHealth";
			case 48: return "HasJuryRigging";
			case 49: return "ModifyThreatRange";
			case 50: return "ModifyThreat";
			case 51: return "HasFastTravelAlways";
			case 52: return "KnockdownChance";
			case 53: return "ModifyWeaponStrengthReq";
			case 54: return "ModifyAimingMoveSpeed";
			case 55: return "ModifyLightItems";
			case 56: return "ModifyDamageThresholdDefender";
			case 57: return "ModifyChanceforAmmoItem";
			case 58: return "ModifyDamageThresholdAttacker";
			case 59: return "ModifyThrowingVelocity";
			case 60: return "ChanceForItemOnFire";
			case 61: return "HasUnarmedForwardPowerAttack";
			case 62: return "HasUnarmedBackPowerAttack";
			case 63: return "HasUnarmedCrouchedPowerAttack";
			case 64: return "HasUnarmedCounterAttack";
			case 65: return "HasUnarmedLeftPowerAttack";
			case 66: return "HasUnarmedRightPowerAttack";
			case 67: return "VATSHelperChance";
			case 68: return "ModifyItemDamage";
			case 69: return "HasImprovedDetection";
			case 70: return "HasImprovedSpotting";
			case 71: return "HasImprovedItemDetection";
			case 72: return "AdjustExplosionRadius";
			case 73: return "AdjustHeavyWeaponWeight";
			default: return "Unknown";
		}
	}

	const char* ComparisonName(UInt8 comparison)
	{
		switch (comparison)
		{
			case 0: return "==";
			case 1: return "!=";
			case 2: return ">";
			case 3: return ">=";
			case 4: return "<";
			case 5: return "<=";
			default: return "?";
		}
	}

	const char* ActorValueName(UInt32 av)
	{
		switch (av)
		{
			case 5: return "Strength";
			case 6: return "Perception";
			case 7: return "Endurance";
			case 8: return "Charisma";
			case 9: return "Intelligence";
			case 10: return "Agility";
			case 11: return "Luck";
			case 32: return "Barter";
			case 33: return "BigGuns";
			case 34: return "EnergyWeapons";
			case 35: return "Explosives";
			case 36: return "Lockpick";
			case 37: return "Medicine";
			case 38: return "MeleeWeapons";
			case 39: return "Repair";
			case 40: return "Science";
			case 41: return "Guns";
			case 42: return "Sneak";
			case 43: return "Speech";
			case 44: return "Survival";
			case 45: return "Unarmed";
			default: return nullptr;
		}
	}

	std::string FormatAVName(UInt32 av)
	{
		const char* name = ActorValueName(av);
		if (name)
			return name;

		char buf[32];
		sprintf_s(buf, "AV%u", av);
		return buf;
	}

	std::string FormName(TESForm* form)
	{
		if (!form)
			return "";

		if (form->typeID == kFormType_Perk)
		{
			auto* perk = static_cast<BGSPerk*>(form);
			if (perk->fullName.name.m_data)
				return perk->fullName.name.m_data;
		}

		const char* name = form->GetName();
		return name ? name : "";
	}

	UInt32 NormalizeFunctionOpcode(UInt32 rawFunctionId)
	{
		return rawFunctionId < 0x1000 ? rawFunctionId + 0x1000 : rawFunctionId;
	}

	UInt32 ConditionFunctionId(UInt32 functionId)
	{
		return functionId >= 0x1000 ? functionId - 0x1000 : functionId;
	}

	ConditionDescriptor ParseCondition(Condition* condition, Source source, UInt32 perkFormID, UInt32 rank, SInt32 entryPoint, SInt32 entryFunction, SInt32 tabIndex)
	{
		ConditionDescriptor descriptor;
		descriptor.source = source;
		descriptor.raw = condition;
		descriptor.perkFormID = perkFormID;
		descriptor.rank = rank;
		descriptor.entryPoint = entryPoint;
		descriptor.entryFunction = entryFunction;
		descriptor.tabIndex = tabIndex;

		if (!condition)
			return descriptor;

		descriptor.rawFunctionId = ConditionFunctionId(condition->function);
		descriptor.commandOpcode = NormalizeFunctionOpcode(descriptor.rawFunctionId);
		descriptor.argValue = condition->parameter1.number;
		descriptor.runOn = condition->runOn;
		descriptor.runOnReference = condition->reference;
		descriptor.comparison = (condition->type >> 5) & 0x07;
		descriptor.comparisonValue = condition->comparisonValue.value;
		descriptor.isOr = (condition->type & kConditionFlag_OR) != 0;
		descriptor.usesGlobalComparison = (condition->type & kConditionFlag_UseGlobal) != 0;
		descriptor.comparisonGlobal = nullptr;

		switch (descriptor.rawFunctionId)
		{
			case kCond_GetActorValue:
				descriptor.kind = Kind::ActorValue;
				break;
			case kCond_GetBaseActorValue:
				descriptor.kind = Kind::BaseActorValue;
				break;
			case kCond_GetPermanentActorValue:
				descriptor.kind = Kind::PermanentActorValue;
				break;
			case kCond_GetLevel:
				descriptor.kind = Kind::Level;
				break;
			case kCond_GetWeaponAnimType:
				descriptor.kind = Kind::WeaponAnimType;
				break;
			case kCond_IsWeaponSkillType:
				descriptor.kind = Kind::WeaponSkillType;
				break;
			case kCond_GetEquipped:
				descriptor.kind = Kind::Equipped;
				descriptor.argForm = reinterpret_cast<TESForm*>(condition->parameter1.pointer);
				break;
			case kCond_GetIsID:
				descriptor.kind = Kind::FormID;
				descriptor.argForm = reinterpret_cast<TESForm*>(condition->parameter1.pointer);
				break;
			case kCond_IsInList:
				descriptor.kind = Kind::FormList;
				descriptor.argForm = reinterpret_cast<TESForm*>(condition->parameter1.pointer);
				break;
			case kCond_IsWeaponInList:
				descriptor.kind = Kind::WeaponList;
				descriptor.argForm = reinterpret_cast<TESForm*>(condition->parameter1.pointer);
				break;
			case kCond_HasPerk:
				descriptor.kind = Kind::HasPerk;
				descriptor.argForm = reinterpret_cast<TESForm*>(condition->parameter1.pointer);
				break;
			case kCond_GetItemCount:
				descriptor.kind = Kind::ItemCount;
				descriptor.argForm = reinterpret_cast<TESForm*>(condition->parameter1.pointer);
				break;
			default:
				descriptor.kind = Kind::Opaque;
				break;
		}

		return descriptor;
	}

	std::vector<RequirementGroup> ParseRequirementGroups(tList<Condition>& conditions, UInt32 perkFormID)
	{
		std::vector<RequirementGroup> groups;
		RequirementGroup current;

		for (auto iter = conditions.Begin(); !iter.End(); ++iter)
		{
			Condition* condition = iter.Get();
			if (!condition)
				continue;

			auto descriptor = ParseCondition(condition, Source::TopLevelRequirement, perkFormID, 0, -1, -1, -1);
			current.any = current.any || descriptor.isOr;
			current.descriptors.push_back(descriptor);

			if (!descriptor.isOr)
			{
				groups.push_back(current);
				current = RequirementGroup();
			}
		}

		if (!current.descriptors.empty())
			groups.push_back(current);

		return groups;
	}

	ConditionDescriptor MinLevelRequirement(const PerkDescriptor& perk)
	{
		ConditionDescriptor descriptor;
		descriptor.source = Source::PerkDataRequirement;
		descriptor.perkFormID = perk.formID;
		descriptor.rawFunctionId = kCond_GetLevel;
		descriptor.commandOpcode = NormalizeFunctionOpcode(kCond_GetLevel);
		descriptor.comparison = 3;
		descriptor.comparisonValue = static_cast<float>(perk.minLevel);
		descriptor.kind = Kind::Level;
		return descriptor;
	}

	ConditionDescriptor MetadataRequirement(const PerkDescriptor& perk, Kind kind)
	{
		ConditionDescriptor descriptor;
		descriptor.source = Source::PerkDataRequirement;
		descriptor.perkFormID = perk.formID;
		descriptor.kind = kind;
		return descriptor;
	}

	EvaluatedCondition FailedRequirement(double currentValue, double requiredValue, double missingValue = 0.0)
	{
		EvaluatedCondition evaluated;
		evaluated.state = EvalState::Fail;
		evaluated.currentValue = currentValue;
		evaluated.requiredValue = requiredValue;
		evaluated.missingValue = missingValue;
		return evaluated;
	}

	void AddBlocker(EligibilityResult& result, const ConditionDescriptor& descriptor, const EvaluatedCondition& evaluated)
	{
		result.blockers.push_back({ descriptor, evaluated });
	}

	bool EngineLevelUpMenuBaseFilter(BGSPerk* perk, PlayerCharacter* player)
	{
		if (!perk || !player)
			return false;

		using SexFilter_t = bool(__thiscall*)(BGSPerk*, TESObjectREFR*);
		using PlayableFilter_t = bool(__thiscall*)(BGSPerk*);
		const auto SexFilter = reinterpret_cast<SexFilter_t>(0x5EBA00);
		const auto PlayableFilter = reinterpret_cast<PlayableFilter_t>(0x5EB5E0);
		return SexFilter(perk, player) && PlayableFilter(perk);
	}

	bool EngineActorHasRequirements(BGSPerk* perk, PlayerCharacter* player)
	{
		if (!perk || !player)
			return false;

		using GetActorHasRequirements_t = bool(__thiscall*)(BGSPerk*, Actor*);
		const auto GetActorHasRequirements = reinterpret_cast<GetActorHasRequirements_t>(0x785150);
		return GetActorHasRequirements(perk, player);
	}

	void AppendEntryConditions(std::vector<ConditionDescriptor>& out, tList<Condition>& conditions, UInt32 perkFormID, UInt32 rank, SInt32 entryPoint, SInt32 function, SInt32 tabIndex)
	{
		for (auto iter = conditions.Begin(); !iter.End(); ++iter)
		{
			Condition* condition = iter.Get();
			if (condition)
				out.push_back(ParseCondition(condition, Source::EntryPointFilter, perkFormID, rank, entryPoint, function, tabIndex));
		}
	}

	bool IsEntryPointPerkEntry(BGSPerkEntry* entry)
	{
		return entry && entry->vtbl == kVtbl_BGSEntryPointPerkEntry;
	}

	std::vector<EntryDescriptor> ParseEntries(BGSPerk* perk)
	{
		std::vector<EntryDescriptor> entries;
		if (!perk)
			return entries;

		for (auto iter = perk->entries.Begin(); !iter.End(); ++iter)
		{
			BGSPerkEntry* entry = iter.Get();
			if (!entry)
				continue;

			EntryDescriptor descriptor;
			descriptor.raw = entry;
			descriptor.isEntryPoint = IsEntryPointPerkEntry(entry);
			descriptor.rank = static_cast<UInt32>(entry->rank) + 1;
			descriptor.type = entry->type;

			if (descriptor.isEntryPoint)
			{
				auto* ep = static_cast<BGSEntryPointPerkEntry*>(entry);
				descriptor.entryPoint = ep->entryPoint;
				descriptor.function = ep->function;
				descriptor.conditionTabs = ep->conditionTabs;

				if (ep->conditions)
				{
					auto* tabs = reinterpret_cast<tList<Condition>*>(ep->conditions);
					const UInt8 count = std::min<UInt8>(descriptor.conditionTabs, kMaxEntryPointFilterTabs);
					for (UInt8 i = 0; i < count; i++)
						AppendEntryConditions(descriptor.filters, tabs[i], perk->refID, descriptor.rank, descriptor.entryPoint, descriptor.function, i + 1);
				}
			}
			else if (entry->type == kPerkEntryType_Quest || entry->type == kPerkEntryType_Ability)
			{
				descriptor.entryPoint = -1;
				descriptor.function = -1;
			}

			entries.push_back(descriptor);
		}

		return entries;
	}

	void AddStringMapValue(std::vector<const char*>& keys, std::vector<Element>& values, const char* key, const Element& value)
	{
		keys.push_back(key);
		values.push_back(value);
	}

	Array* CreateStringMap(Script* scriptObj, const std::vector<const char*>& keys, const std::vector<Element>& values)
	{
		if (!g_arrInterface || keys.empty())
			return g_arrInterface ? g_arrInterface->CreateStringMap(nullptr, nullptr, 0, scriptObj) : nullptr;

		return g_arrInterface->CreateStringMap(const_cast<const char**>(keys.data()), values.data(), static_cast<UInt32>(keys.size()), scriptObj);
	}

	Array* CreateArray(Script* scriptObj)
	{
		return g_arrInterface ? g_arrInterface->CreateArray(nullptr, 0, scriptObj) : nullptr;
	}

	void AssignArrayResult(Array* arr, double* result)
	{
		if (g_arrInterface && arr)
			g_arrInterface->AssignCommandResult(arr, result);
	}

	void ParseFormIDSet(Array* arr, std::unordered_set<UInt32>& out)
	{
		if (!g_arrInterface || !arr)
			return;

		const UInt32 size = g_arrInterface->GetArraySize(arr);
		if (!size)
			return;

		std::vector<Element> values(size);
		g_arrInterface->GetElements(arr, values.data(), nullptr);
		for (auto& value : values)
		{
			if (value.GetType() == Element::kType_Form && value.Form())
				out.insert(value.Form()->refID);
			else if (value.GetType() == Element::kType_Numeric)
				out.insert(static_cast<UInt32>(value.Number()));
		}
	}

	void ParseNumericMap(Array* arr, std::unordered_map<UInt32, double>& out)
	{
		if (!g_arrInterface || !arr)
			return;

		const UInt32 size = g_arrInterface->GetArraySize(arr);
		if (!size)
			return;

		std::vector<Element> values(size);
		std::vector<Element> keys(size);
		g_arrInterface->GetElements(arr, values.data(), keys.data());
		for (UInt32 i = 0; i < size; i++)
		{
			if (values[i].GetType() != Element::kType_Numeric)
				continue;

			UInt32 key = 0;
			if (keys[i].GetType() == Element::kType_Numeric)
				key = static_cast<UInt32>(keys[i].Number());
			else if (keys[i].GetType() == Element::kType_String && keys[i].String())
				key = strtoul(keys[i].String(), nullptr, 0);

			if (key)
				out[key] = values[i].Number();
		}
	}

	bool GetMapElement(Array* arr, const char* key, Element& out)
	{
		if (!g_arrInterface || !arr)
			return false;

		Element keyElem(key);
		return g_arrInterface->GetElement(arr, keyElem, out);
	}

	StateDelta ParseStateDelta(Array* arr)
	{
		StateDelta delta;
		if (!g_arrInterface || !arr)
			return delta;

		Element value;
		if (GetMapElement(arr, "level", value) && value.GetType() == Element::kType_Numeric)
		{
			delta.hasLevel = true;
			delta.level = value.Number();
		}

		if (GetMapElement(arr, "av", value) && value.GetType() == Element::kType_Array)
			ParseNumericMap(value.Array(), delta.av);
		if (GetMapElement(arr, "avDelta", value) && value.GetType() == Element::kType_Array)
			ParseNumericMap(value.Array(), delta.avDelta);
		if (GetMapElement(arr, "itemsAdded", value) && value.GetType() == Element::kType_Array)
			ParseNumericMap(value.Array(), delta.itemsAdded);
		if (GetMapElement(arr, "itemsRemoved", value) && value.GetType() == Element::kType_Array)
			ParseNumericMap(value.Array(), delta.itemsRemoved);
		if (GetMapElement(arr, "perksAdded", value) && value.GetType() == Element::kType_Array)
			ParseFormIDSet(value.Array(), delta.perksAdded);
		if (GetMapElement(arr, "perksRemoved", value) && value.GetType() == Element::kType_Array)
			ParseFormIDSet(value.Array(), delta.perksRemoved);

		return delta;
	}

	bool HasStateDelta(const StateDelta& delta)
	{
		return delta.hasLevel
		    || !delta.av.empty()
		    || !delta.avDelta.empty()
		    || !delta.itemsAdded.empty()
		    || !delta.itemsRemoved.empty()
		    || !delta.perksAdded.empty()
		    || !delta.perksRemoved.empty();
	}

	double RequiredValue(const ConditionDescriptor& descriptor)
	{
		if (descriptor.usesGlobalComparison && descriptor.comparisonGlobal)
			return descriptor.comparisonGlobal->data;
		return descriptor.comparisonValue;
	}

	bool Compare(double current, UInt8 comparison, double required)
	{
		const double epsilon = 0.0001;
		switch (comparison)
		{
			case 0: return std::fabs(current - required) <= epsilon;
			case 1: return std::fabs(current - required) > epsilon;
			case 2: return current > required;
			case 3: return current >= required;
			case 4: return current < required;
			case 5: return current <= required;
			default: return false;
		}
	}

	double MissingValue(double current, UInt8 comparison, double required)
	{
		switch (comparison)
		{
			case 2:
			case 3:
				return current < required ? required - current : 0.0;
			case 0:
				return current < required ? required - current : 0.0;
			default:
				return 0.0;
		}
	}

	void* ActorValueOwner(PlayerCharacter* player)
	{
		return player ? reinterpret_cast<UInt8*>(player) + 0xA4 : nullptr;
	}

	double ReadActorValue(PlayerCharacter* player, UInt32 avCode)
	{
		auto* owner = ActorValueOwner(player);
		if (!owner)
			return 0.0;

		using GetActorValue_t = float(__thiscall*)(void*, UInt32);
		const auto GetActorValue = reinterpret_cast<GetActorValue_t>(0x66EF50);
		return static_cast<double>(GetActorValue(owner, avCode));
	}

	double ReadBaseActorValue(PlayerCharacter* player, UInt32 avCode)
	{
		auto* owner = ActorValueOwner(player);
		if (!owner)
			return 0.0;

		auto** vtbl = *reinterpret_cast<void***>(owner);
		if (!vtbl)
			return 0.0;

		auto fn = reinterpret_cast<UInt32(__thiscall*)(void*, UInt32)>(vtbl[0]);
		return fn ? static_cast<double>(fn(owner, avCode)) : 0.0;
	}

	double ReadPermanentActorValue(PlayerCharacter* player, UInt32 avCode)
	{
		auto* owner = ActorValueOwner(player);
		if (!owner)
			return 0.0;

		auto** vtbl = *reinterpret_cast<void***>(owner);
		if (!vtbl)
			return 0.0;

		auto fn = reinterpret_cast<float(__thiscall*)(void*, UInt32)>(vtbl[8]);
		return fn ? static_cast<double>(fn(owner, avCode)) : 0.0;
	}

	double ApplyActorValueDelta(const EvalContext& ctx, UInt32 avCode, double currentValue)
	{
		const auto exact = ctx.delta.av.find(avCode);
		if (exact != ctx.delta.av.end())
			return exact->second;

		const auto delta = ctx.delta.avDelta.find(avCode);
		return delta != ctx.delta.avDelta.end() ? currentValue + delta->second : currentValue;
	}

	double ReadLevel(PlayerCharacter* player)
	{
		if (!player)
			return 0.0;

		using GetLevel_t = UInt16(__thiscall*)(Actor*);
		const auto GetLevel = reinterpret_cast<GetLevel_t>(0x87F9F0);
		return static_cast<double>(GetLevel(player));
	}

	double ReadItemCount(PlayerCharacter* player, TESForm* item)
	{
		if (!player || !item)
			return 0.0;

		using GetItemCount_t = SInt32(__thiscall*)(TESObjectREFR*, TESForm*);
		const auto GetItemCount = reinterpret_cast<GetItemCount_t>(0x575610);
		return static_cast<double>(GetItemCount(player, item));
	}

	UInt32 NormalizeOwnedRank(UInt32 rank, UInt32 maxRank)
	{
		if (rank == 0xFF || rank > maxRank)
			return 0;
		return rank;
	}

	UInt32 GetOwnedRank(PlayerCharacter* player, BGSPerk* perk, UInt32 maxRank, const StateDelta& delta)
	{
		if (!perk)
			return 0;

		if (delta.perksRemoved.count(perk->refID))
			return 0;
		if (delta.perksAdded.count(perk->refID))
			return std::max<UInt32>(1, NormalizeOwnedRank(player ? player->GetPerkRank(perk, false) : 0, maxRank));

		return NormalizeOwnedRank(player ? player->GetPerkRank(perk, false) : 0, maxRank);
	}

	double GetDeltaAdjustedItemCount(const EvalContext& ctx, TESForm* item)
	{
		if (!item)
			return 0.0;

		double value = ReadItemCount(ctx.player, item);
		auto added = ctx.delta.itemsAdded.find(item->refID);
		if (added != ctx.delta.itemsAdded.end())
			value += added->second;
		auto removed = ctx.delta.itemsRemoved.find(item->refID);
		if (removed != ctx.delta.itemsRemoved.end())
			value -= removed->second;
		return value;
	}

	EvaluatedCondition EvaluateCondition(const ConditionDescriptor& descriptor, const EvalContext& ctx)
	{
		EvaluatedCondition out;
		if (descriptor.usesGlobalComparison && !descriptor.comparisonGlobal)
		{
			out.state = EvalState::Indeterminate;
			return out;
		}

		out.requiredValue = RequiredValue(descriptor);

		switch (descriptor.kind)
		{
			case Kind::ActorValue:
			{
				out.currentValue = ApplyActorValueDelta(ctx, descriptor.argValue, ReadActorValue(ctx.player, descriptor.argValue));
				break;
			}
			case Kind::BaseActorValue:
				out.currentValue = ApplyActorValueDelta(ctx, descriptor.argValue, ReadBaseActorValue(ctx.player, descriptor.argValue));
				break;
			case Kind::PermanentActorValue:
				out.currentValue = ApplyActorValueDelta(ctx, descriptor.argValue, ReadPermanentActorValue(ctx.player, descriptor.argValue));
				break;
			case Kind::Level:
				out.currentValue = ctx.delta.hasLevel ? ctx.delta.level : ReadLevel(ctx.player);
				break;
			case Kind::HasPerk:
			{
				auto* requiredPerk = static_cast<BGSPerk*>(descriptor.argForm);
				const UInt32 maxRank = requiredPerk ? std::max<UInt32>(requiredPerk->data.numRanks, 1) : 1;
				out.currentValue = GetOwnedRank(ctx.player, requiredPerk, maxRank, ctx.delta) > 0 ? 1.0 : 0.0;
				break;
			}
			case Kind::ItemCount:
				out.currentValue = GetDeltaAdjustedItemCount(ctx, descriptor.argForm);
				break;
			default:
				out.state = EvalState::Indeterminate;
				return out;
		}

		out.state = Compare(out.currentValue, descriptor.comparison, out.requiredValue) ? EvalState::Pass : EvalState::Fail;
		out.missingValue = MissingValue(out.currentValue, descriptor.comparison, out.requiredValue);
		return out;
	}

	EvaluatedGroup EvaluateGroup(const RequirementGroup& group, const EvalContext& ctx)
	{
		EvaluatedGroup out;
		if (group.descriptors.empty())
			return out;

		bool anyPass = false;
		bool anyUnknown = false;

		for (const auto& descriptor : group.descriptors)
		{
			auto evaluated = EvaluateCondition(descriptor, ctx);
			if (evaluated.state == EvalState::Pass)
			{
				anyPass = true;
			}
			else if (evaluated.state == EvalState::Fail)
			{
				out.failed.push_back({ descriptor, evaluated });
			}
			else
			{
				anyUnknown = true;
				out.indeterminate.push_back(descriptor);
			}

			if (!group.any && evaluated.state == EvalState::Fail)
				break;
		}

		if (group.any)
		{
			if (anyPass)
			{
				out.state = EvalState::Pass;
				out.failed.clear();
				out.indeterminate.clear();
			}
			else if (anyUnknown)
			{
				out.state = EvalState::Indeterminate;
				out.failed.clear();
			}
			else
			{
				out.state = EvalState::Fail;
			}
		}
		else
		{
			if (!out.failed.empty())
				out.state = EvalState::Fail;
			else if (anyUnknown)
				out.state = EvalState::Indeterminate;
			else
				out.state = EvalState::Pass;
		}

		return out;
	}

	const PerkDescriptor* FindPerkDescriptor(BGSPerk* perk)
	{
		if (!perk)
			return nullptr;

		auto it = g_perksByFormID.find(perk->refID);
		if (it == g_perksByFormID.end())
			return nullptr;

		return &g_perks[it->second];
	}

	bool HasRequirements(const PerkDescriptor& descriptor)
	{
		for (const auto& group : descriptor.requirements)
		{
			if (!group.descriptors.empty())
				return true;
		}

		return false;
	}

	bool IsLikelyScriptGranted(const PerkDescriptor& descriptor)
	{
		return !descriptor.trait && !HasRequirements(descriptor) && (descriptor.hidden || !descriptor.playable);
	}

	EligibilityResult EvaluateEligibility(const PerkDescriptor& descriptor, const EvalContext& ctx, UInt32 flags)
	{
		EligibilityResult result;
		result.perk = descriptor.perk;
		result.name = descriptor.name;
		result.maxRank = descriptor.maxRank;
		result.ownedRank = GetOwnedRank(ctx.player, descriptor.perk, descriptor.maxRank, ctx.delta);
		result.nextRank = result.ownedRank < result.maxRank ? result.ownedRank + 1 : result.maxRank;

		if (result.ownedRank >= result.maxRank && result.maxRank > 0)
		{
			AddBlocker(result,
			           MetadataRequirement(descriptor, Kind::OwnedMaxRank),
			           FailedRequirement(result.ownedRank, result.maxRank));
			result.status = "owned_max";
			return result;
		}

		if (IsLikelyScriptGranted(descriptor))
		{
			AddBlocker(result, MetadataRequirement(descriptor, Kind::Scripted), FailedRequirement(1.0, 0.0));
		}

		if (descriptor.trait && !(flags & kFlag_IncludeTraits))
		{
			AddBlocker(result, MetadataRequirement(descriptor, Kind::Trait), FailedRequirement(1.0, 0.0));
		}

		if (!descriptor.playable && !(flags & kFlag_IncludeNonPlayable))
		{
			AddBlocker(result, MetadataRequirement(descriptor, Kind::NonPlayable), FailedRequirement(0.0, 1.0));
		}

		if (descriptor.hidden && !(flags & kFlag_IncludeHidden))
		{
			AddBlocker(result, MetadataRequirement(descriptor, Kind::Hidden), FailedRequirement(1.0, 0.0));
		}

		const bool hasDelta = HasStateDelta(ctx.delta);
		if (descriptor.minLevel == 0 && !(flags & kFlag_IncludeNonLevelUp))
		{
			AddBlocker(result, MetadataRequirement(descriptor, Kind::NonLevelUp), FailedRequirement(0.0, 1.0));
		}
		else if (!hasDelta && !(flags & kFlag_IncludeNonLevelUp) && descriptor.playable && !descriptor.trait && !EngineLevelUpMenuBaseFilter(descriptor.perk, ctx.player))
		{
			AddBlocker(result, MetadataRequirement(descriptor, Kind::MenuFiltered), FailedRequirement(0.0, 1.0));
		}

		const double level = ctx.delta.hasLevel ? ctx.delta.level : ReadLevel(ctx.player);
		if (descriptor.minLevel > 0 && level < descriptor.minLevel)
		{
			auto blocker = MinLevelRequirement(descriptor);
			AddBlocker(result, blocker, FailedRequirement(level, descriptor.minLevel, descriptor.minLevel - level));
		}

		std::vector<std::pair<ConditionDescriptor, EvaluatedCondition>> parsedFailures;
		std::vector<ConditionDescriptor> parsedIndeterminate;
		for (const auto& group : descriptor.requirements)
		{
			auto evaluated = EvaluateGroup(group, ctx);
			if (evaluated.state == EvalState::Fail)
				parsedFailures.insert(parsedFailures.end(), evaluated.failed.begin(), evaluated.failed.end());
			else if (evaluated.state == EvalState::Indeterminate)
				parsedIndeterminate.insert(parsedIndeterminate.end(), evaluated.indeterminate.begin(), evaluated.indeterminate.end());
		}

		if (!hasDelta && HasRequirements(descriptor))
		{
			if (!EngineActorHasRequirements(descriptor.perk, ctx.player))
			{
				if (!parsedFailures.empty())
					result.blockers.insert(result.blockers.end(), parsedFailures.begin(), parsedFailures.end());
				else
					AddBlocker(result, MetadataRequirement(descriptor, Kind::EngineRequirements), FailedRequirement(0.0, 1.0));
			}
		}
		else
		{
			result.blockers.insert(result.blockers.end(), parsedFailures.begin(), parsedFailures.end());
			result.indeterminate.insert(result.indeterminate.end(), parsedIndeterminate.begin(), parsedIndeterminate.end());
		}

		if (result.blockers.empty() && result.indeterminate.empty())
		{
			result.eligible = true;
			result.status = "takeable";
		}
		else if (!result.blockers.empty())
		{
			result.status = IsLikelyScriptGranted(descriptor) ? "scripted" : "blocked";
		}
		else
		{
			result.status = "unknown";
		}

		return result;
	}

	std::string BlockerText(const ConditionDescriptor& descriptor, const EvaluatedCondition& evaluated)
	{
		char buf[256];
		switch (descriptor.kind)
		{
			case Kind::OwnedMaxRank:
				sprintf_s(buf, "Already owned at max rank %.0f/%.0f",
				          evaluated.currentValue,
				          evaluated.requiredValue);
				return buf;
			case Kind::Trait:
				return "Trait perk excluded";
			case Kind::Hidden:
				return "Hidden perk excluded";
			case Kind::NonPlayable:
				return "Non-playable perk excluded";
			case Kind::NonLevelUp:
				return "Not selectable in the level-up menu (min level is 0)";
			case Kind::MenuFiltered:
				return "Filtered by the level-up menu base checks";
			case Kind::EngineRequirements:
				return "Perk requirements not met";
			case Kind::Scripted:
				return "Likely script-granted perk";
			case Kind::ActorValue:
			case Kind::BaseActorValue:
			case Kind::PermanentActorValue:
				sprintf_s(buf, "%s %.0f/%.0f, missing %.0f",
				          FormatAVName(descriptor.argValue).c_str(),
				          evaluated.currentValue,
				          evaluated.requiredValue,
				          evaluated.missingValue);
				return buf;
			case Kind::Level:
				sprintf_s(buf, "Level %.0f/%.0f, missing %.0f",
				          evaluated.currentValue,
				          evaluated.requiredValue,
				          evaluated.missingValue);
				return buf;
			case Kind::HasPerk:
			{
				const std::string name = FormName(descriptor.argForm);
				sprintf_s(buf, "Requires %s", name.empty() ? "perk" : name.c_str());
				return buf;
			}
			case Kind::ItemCount:
			{
				const std::string name = FormName(descriptor.argForm);
				sprintf_s(buf, "%s %.0f/%.0f, missing %.0f",
				          name.empty() ? "Item" : name.c_str(),
				          evaluated.currentValue,
				          evaluated.requiredValue,
				          evaluated.missingValue);
				return buf;
			}
			default:
				return "Opaque requirement";
		}
	}

	Array* ConditionToMap(const ConditionDescriptor& descriptor, Script* scriptObj)
	{
		std::vector<const char*> keys;
		std::vector<Element> values;
		auto* perkForm = reinterpret_cast<TESForm*(__cdecl*)(UInt32)>(0x4839C0)(descriptor.perkFormID);

		AddStringMapValue(keys, values, "source", Element(SourceName(descriptor.source)));
		AddStringMapValue(keys, values, "perk", Element(perkForm));
		AddStringMapValue(keys, values, "rank", Element(static_cast<double>(descriptor.rank)));
		AddStringMapValue(keys, values, "entryPoint", Element(static_cast<double>(descriptor.entryPoint)));
		AddStringMapValue(keys, values, "entryFunction", Element(static_cast<double>(descriptor.entryFunction)));
		AddStringMapValue(keys, values, "tabIndex", Element(static_cast<double>(descriptor.tabIndex)));
		AddStringMapValue(keys, values, "rawFunctionId", Element(static_cast<double>(descriptor.rawFunctionId)));
		AddStringMapValue(keys, values, "commandOpcode", Element(static_cast<double>(descriptor.commandOpcode)));
		AddStringMapValue(keys, values, "function", Element(FunctionName(descriptor.rawFunctionId)));
		AddStringMapValue(keys, values, "runOn", Element(static_cast<double>(descriptor.runOn)));
		AddStringMapValue(keys, values, "runOnName", Element(RunOnName(descriptor.runOn)));
		AddStringMapValue(keys, values, "runOnReference", Element(static_cast<TESForm*>(descriptor.runOnReference)));
		AddStringMapValue(keys, values, "comparison", Element(ComparisonName(descriptor.comparison)));
		AddStringMapValue(keys, values, "comparisonValue", Element(static_cast<double>(RequiredValue(descriptor))));
		AddStringMapValue(keys, values, "isOr", Element(descriptor.isOr ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "usesGlobalComparison", Element(descriptor.usesGlobalComparison ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "kind", Element(KindName(descriptor.kind)));
		AddStringMapValue(keys, values, "argForm", Element(descriptor.argForm));
		AddStringMapValue(keys, values, "argValue", Element(static_cast<double>(descriptor.argValue)));

		return CreateStringMap(scriptObj, keys, values);
	}

	Array* BlockerToMap(const ConditionDescriptor& descriptor, const EvaluatedCondition& evaluated, Script* scriptObj)
	{
		std::vector<const char*> keys;
		std::vector<Element> values;

		AddStringMapValue(keys, values, "kind", Element(KindName(descriptor.kind)));
		AddStringMapValue(keys, values, "function", Element(FunctionName(descriptor.rawFunctionId)));
		AddStringMapValue(keys, values, "comparison", Element(ComparisonName(descriptor.comparison)));
		AddStringMapValue(keys, values, "currentValue", Element(evaluated.currentValue));
		AddStringMapValue(keys, values, "requiredValue", Element(evaluated.requiredValue));
		AddStringMapValue(keys, values, "missingValue", Element(evaluated.missingValue));
		AddStringMapValue(keys, values, "argForm", Element(descriptor.argForm));
		AddStringMapValue(keys, values, "argValue", Element(static_cast<double>(descriptor.argValue)));
		AddStringMapValue(keys, values, "text", Element(BlockerText(descriptor, evaluated).c_str()));

		return CreateStringMap(scriptObj, keys, values);
	}

	Array* ConditionsArray(const PerkDescriptor& descriptor, Script* scriptObj)
	{
		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return nullptr;

		if (descriptor.minLevel > 0)
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(MinLevelRequirement(descriptor), scriptObj)));
		else
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(MetadataRequirement(descriptor, Kind::NonLevelUp), scriptObj)));
		if (descriptor.trait)
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(MetadataRequirement(descriptor, Kind::Trait), scriptObj)));
		if (!descriptor.playable)
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(MetadataRequirement(descriptor, Kind::NonPlayable), scriptObj)));
		if (descriptor.hidden)
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(MetadataRequirement(descriptor, Kind::Hidden), scriptObj)));

		for (const auto& group : descriptor.requirements)
		{
			for (const auto& condition : group.descriptors)
				g_arrInterface->AppendElement(arr, Element(ConditionToMap(condition, scriptObj)));
		}

		for (const auto& entry : descriptor.entries)
		{
			for (const auto& condition : entry.filters)
				g_arrInterface->AppendElement(arr, Element(ConditionToMap(condition, scriptObj)));
		}

		return arr;
	}

	Array* BlockersArray(const EligibilityResult& eligibility, Script* scriptObj)
	{
		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return nullptr;

		for (const auto& [descriptor, evaluated] : eligibility.blockers)
			g_arrInterface->AppendElement(arr, Element(BlockerToMap(descriptor, evaluated, scriptObj)));

		return arr;
	}

	Array* IndeterminateArray(const EligibilityResult& eligibility, Script* scriptObj)
	{
		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return nullptr;

		for (const auto& descriptor : eligibility.indeterminate)
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(descriptor, scriptObj)));

		return arr;
	}

	Array* EligibilityToMap(const EligibilityResult& eligibility, Script* scriptObj)
	{
		std::vector<const char*> keys;
		std::vector<Element> values;

		AddStringMapValue(keys, values, "perk", Element(reinterpret_cast<TESForm*>(eligibility.perk)));
		AddStringMapValue(keys, values, "name", Element(eligibility.name.c_str()));
		AddStringMapValue(keys, values, "ownedRank", Element(static_cast<double>(eligibility.ownedRank)));
		AddStringMapValue(keys, values, "nextRank", Element(static_cast<double>(eligibility.nextRank)));
		AddStringMapValue(keys, values, "maxRank", Element(static_cast<double>(eligibility.maxRank)));
		AddStringMapValue(keys, values, "eligible", Element(eligibility.eligible ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "status", Element(eligibility.status.c_str()));
		AddStringMapValue(keys, values, "blockers", Element(BlockersArray(eligibility, scriptObj)));
		AddStringMapValue(keys, values, "indeterminate", Element(IndeterminateArray(eligibility, scriptObj)));

		return CreateStringMap(scriptObj, keys, values);
	}

	bool SameForm(TESForm* lhs, TESForm* rhs)
	{
		return lhs && rhs && lhs->refID == rhs->refID;
	}

	bool FormListContains(BGSListForm* list, TESForm* subject, std::unordered_set<UInt32>& visited)
	{
		if (!list || !subject || !visited.insert(list->refID).second)
			return false;

		for (auto iter = list->list.Begin(); !iter.End(); ++iter)
		{
			auto* form = iter.Get();
			if (!form)
				continue;

			if (SameForm(form, subject))
				return true;
			if (form->typeID == kFormType_ListForm && FormListContains(static_cast<BGSListForm*>(form), subject, visited))
				return true;
		}

		return false;
	}

	bool FormListContains(TESForm* listForm, TESForm* subject)
	{
		if (!listForm || listForm->typeID != kFormType_ListForm)
			return false;

		std::unordered_set<UInt32> visited;
		return FormListContains(static_cast<BGSListForm*>(listForm), subject, visited);
	}

	bool FormListHasSubjectType(BGSListForm* list, TESForm* subject, std::unordered_set<UInt32>& visited)
	{
		if (!list || !subject || !visited.insert(list->refID).second)
			return false;

		for (auto iter = list->list.Begin(); !iter.End(); ++iter)
		{
			auto* form = iter.Get();
			if (!form)
				continue;

			if (form->typeID == subject->typeID)
				return true;
			if (form->typeID == kFormType_ListForm && FormListHasSubjectType(static_cast<BGSListForm*>(form), subject, visited))
				return true;
		}

		return false;
	}

	bool EquippedArgMatches(TESForm* argForm, TESForm* subject)
	{
		if (!argForm || !subject)
			return false;

		if (argForm->typeID == kFormType_ListForm)
		{
			std::unordered_set<UInt32> visited;
			return FormListHasSubjectType(static_cast<BGSListForm*>(argForm), subject, visited);
		}

		return argForm->typeID == subject->typeID;
	}

	UInt32 WeaponAnimType(TESObjectWEAP* weapon)
	{
		static constexpr UInt32 kWeaponTypeToAnim[] = { 1, 2, 3, 4, 4, 5, 6, 5, 7, 8, 9, 10, 11, 9 };
		if (!weapon || weapon->eWeaponType >= sizeof(kWeaponTypeToAnim) / sizeof(kWeaponTypeToAnim[0]))
			return 1;

		return kWeaponTypeToAnim[weapon->eWeaponType];
	}

	EvalState EvalFormCondition(const ConditionDescriptor& descriptor, TESForm* subject)
	{
		if (descriptor.usesGlobalComparison && !descriptor.comparisonGlobal)
			return EvalState::Indeterminate;

		double currentValue = 0.0;
		switch (descriptor.kind)
		{
			case Kind::FormID:
				currentValue = SameForm(subject, descriptor.argForm) ? 1.0 : 0.0;
				break;
			case Kind::FormList:
				currentValue = FormListContains(descriptor.argForm, subject) ? 1.0 : 0.0;
				break;
			case Kind::WeaponList:
				currentValue = subject && subject->IsWeapon() && FormListContains(descriptor.argForm, subject) ? 1.0 : 0.0;
				break;
			case Kind::Equipped:
				if (!subject)
					return EvalState::Fail;
				if (!EquippedArgMatches(descriptor.argForm, subject))
					return EvalState::Fail;
				if (descriptor.argForm && descriptor.argForm->typeID == kFormType_ListForm)
					currentValue = FormListContains(descriptor.argForm, subject) ? 1.0 : 0.0;
				else
					currentValue = SameForm(subject, descriptor.argForm) ? 1.0 : 0.0;
				break;
			case Kind::WeaponAnimType:
				if (!subject || !subject->IsWeapon())
					return EvalState::Fail;
				currentValue = WeaponAnimType(static_cast<TESObjectWEAP*>(subject));
				break;
			case Kind::WeaponSkillType:
				if (!subject || !subject->IsWeapon())
					return EvalState::Fail;
				currentValue = static_cast<TESObjectWEAP*>(subject)->weaponSkill == descriptor.argValue ? 1.0 : 0.0;
				break;
			default:
				return EvalState::Indeterminate;
		}

		return Compare(currentValue, descriptor.comparison, RequiredValue(descriptor)) ? EvalState::Pass : EvalState::Fail;
	}

	std::vector<RequirementGroup> BuildFilterGroups(const EntryDescriptor& entry, SInt32 tabIndex)
	{
		std::vector<RequirementGroup> groups;
		RequirementGroup current;

		for (const auto& descriptor : entry.filters)
		{
			if (descriptor.tabIndex != tabIndex)
				continue;

			current.any = current.any || descriptor.isOr;
			current.descriptors.push_back(descriptor);
			if (!descriptor.isOr)
			{
				groups.push_back(current);
				current = RequirementGroup();
			}
		}

		if (!current.descriptors.empty())
			groups.push_back(current);

		return groups;
	}

	bool IsFormCondition(const ConditionDescriptor& descriptor)
	{
		switch (descriptor.kind)
		{
			case Kind::FormID:
			case Kind::FormList:
			case Kind::WeaponList:
			case Kind::Equipped:
			case Kind::WeaponAnimType:
			case Kind::WeaponSkillType:
				return true;
			default:
				return false;
		}
	}

	bool IsFormSelector(const ConditionDescriptor& descriptor)
	{
		switch (descriptor.kind)
		{
			case Kind::FormID:
			case Kind::FormList:
			case Kind::WeaponList:
			case Kind::Equipped:
				return true;
			default:
				return false;
		}
	}

	bool IsPositiveSelector(const ConditionDescriptor& descriptor)
	{
		if (descriptor.usesGlobalComparison && !descriptor.comparisonGlobal)
			return false;

		if (descriptor.kind == Kind::Equipped)
			return true;

		return Compare(1.0, descriptor.comparison, RequiredValue(descriptor));
	}

	bool EntryUsesAttackerWeapon(SInt32 entryPoint)
	{
		return entryPoint >= 0 && entryPoint != 36 && entryPoint != 56;
	}

	bool RunsOnSubject(const ConditionDescriptor& descriptor)
	{
		return descriptor.runOn == 0;
	}

	bool IsOwnerTab(const ConditionDescriptor& descriptor)
	{
		return descriptor.tabIndex <= 1;
	}

	bool CanUseEquippedFilter(const EntryDescriptor& entry, const ConditionDescriptor& descriptor)
	{
		if (IsOwnerTab(descriptor))
			return true;

		return entry.entryPoint == 56;
	}

	bool CanMatchForm(const ConditionDescriptor& descriptor, TESForm* subject)
	{
		if (!subject)
			return false;

		switch (descriptor.kind)
		{
			case Kind::FormID:
				return descriptor.argForm && descriptor.argForm->typeID == subject->typeID;
			case Kind::FormList:
			case Kind::WeaponList:
				return EquippedArgMatches(descriptor.argForm, subject);
			case Kind::Equipped:
				return EquippedArgMatches(descriptor.argForm, subject);
			default:
				return false;
		}
	}

	bool CanMatchSubject(const EntryDescriptor& entry, const ConditionDescriptor& descriptor, TESForm* subject)
	{
		if (!subject)
			return false;
		if (!RunsOnSubject(descriptor))
			return false;

		switch (descriptor.kind)
		{
			case Kind::Equipped:
				if (!CanUseEquippedFilter(entry, descriptor))
					return false;
				return CanMatchForm(descriptor, subject);
			case Kind::WeaponAnimType:
			case Kind::WeaponSkillType:
			case Kind::WeaponList:
				return subject->IsWeapon() && EntryUsesAttackerWeapon(entry.entryPoint);
			case Kind::FormID:
			case Kind::FormList:
				if (!CanMatchForm(descriptor, subject))
					return false;
				return !subject->IsWeapon() || EntryUsesAttackerWeapon(entry.entryPoint);
			default:
				return false;
		}
	}

	struct FilterEval
	{
		EvalState state = EvalState::Indeterminate;
		bool matched = false;
		bool hasFormSelector = false;
		bool hasPositiveFormSelector = false;
	};

	FilterEval EvalFilterGroup(const EntryDescriptor& entry, const RequirementGroup& group, TESForm* subject)
	{
		if (group.descriptors.empty())
			return {};

		bool anyPass = false;
		bool anyUnknown = false;
		bool anyFail = false;
		bool anyOpaque = false;
		bool matched = false;
		bool hasFormSelector = false;
		bool hasPositiveFormSelector = false;

		for (const auto& descriptor : group.descriptors)
		{
			if (!IsFormCondition(descriptor))
			{
				anyOpaque = true;
				continue;
			}
			if (!CanMatchSubject(entry, descriptor, subject))
				continue;

			matched = true;
			const auto state = EvalFormCondition(descriptor, subject);
			if (IsFormSelector(descriptor))
			{
				hasFormSelector = true;
				hasPositiveFormSelector = hasPositiveFormSelector || IsPositiveSelector(descriptor);
			}

			if (state == EvalState::Pass)
				anyPass = true;
			else if (state == EvalState::Indeterminate)
				anyUnknown = true;
			else
				anyFail = true;

			if (!group.any && state == EvalState::Fail)
				break;
		}

		if (!matched)
			return {};

		if (group.any)
		{
			if (anyPass)
				return { EvalState::Pass, true, hasFormSelector, hasPositiveFormSelector };
			return { anyUnknown || anyOpaque ? EvalState::Indeterminate : EvalState::Fail,
			         true,
			         hasFormSelector,
			         hasPositiveFormSelector };
		}

		if (anyFail)
			return { EvalState::Fail, true, hasFormSelector, hasPositiveFormSelector };
		return { anyUnknown ? EvalState::Indeterminate : EvalState::Pass,
		         true,
		         hasFormSelector,
		         hasPositiveFormSelector };
	}

	FilterEval EvalEntryFilters(const EntryDescriptor& entry, TESForm* subject)
	{
		bool matched = false;
		bool anyUnknown = false;
		bool hasFormSelector = false;
		bool hasPositiveFormSelector = false;
		const UInt8 count = std::min<UInt8>(entry.conditionTabs, kMaxEntryPointFilterTabs);
		for (UInt8 i = 0; i < count; i++)
		{
			const auto groups = BuildFilterGroups(entry, i + 1);
			for (const auto& group : groups)
			{
				const auto evaluated = EvalFilterGroup(entry, group, subject);
				if (!evaluated.matched)
					continue;

				matched = true;
				hasFormSelector = hasFormSelector || evaluated.hasFormSelector;
				hasPositiveFormSelector = hasPositiveFormSelector || evaluated.hasPositiveFormSelector;
				if (evaluated.state == EvalState::Fail)
				{
					return { EvalState::Fail,
					         true,
					         hasFormSelector,
					         hasPositiveFormSelector };
				}
				if (evaluated.state == EvalState::Indeterminate)
					anyUnknown = true;
			}
		}

		if (!matched)
			return {};

		if (hasFormSelector && !hasPositiveFormSelector)
			anyUnknown = true;

		return { anyUnknown ? EvalState::Indeterminate : EvalState::Pass,
		         true,
		         hasFormSelector,
		         hasPositiveFormSelector };
	}

	bool IsBroadEntryPointForForm(TESForm* subject, SInt32 entryPoint, UInt32 flags)
	{
		if (!subject || entryPoint < 0)
			return false;
		if (flags & kFlag_AllEntryPoints)
			return true;

		if (subject->IsWeapon())
		{
			switch (entryPoint)
			{
				case 0:
				case 1:
				case 2:
				case 3:
				case 5:
				case 7:
				case 8:
				case 13:
				case 34:
				case 37:
				case 38:
				case 40:
				case 52:
				case 53:
				case 54:
				case 57:
				case 58:
				case 59:
				case 68:
				case 72:
				case 73:
					return true;
				default:
					return false;
			}
		}

		if (subject->IsArmor())
		{
			switch (entryPoint)
			{
				case 13:
				case 38:
				case 55:
				case 56:
				case 68:
					return true;
				default:
					return false;
			}
		}

		switch (entryPoint)
		{
			case 13:
			case 38:
			case 55:
			case 68:
				return true;
			default:
				return false;
		}
	}

	EvalState EvalEntrySubject(const EntryDescriptor& entry, TESForm* subject)
	{
		if (!entry.raw || !subject || !entry.isEntryPoint || entry.conditionTabs == 0)
			return EvalState::Fail;

		const auto evaluated = EvalEntryFilters(entry, subject);
		if (evaluated.matched)
			return evaluated.state;

		return EvalState::Indeterminate;
	}

	std::vector<EntryMatch> GetEntryMatches(const PerkDescriptor& descriptor, TESForm* subject, UInt32 flags)
	{
		std::vector<EntryMatch> matches;
		if (!subject)
			return matches;

		for (const auto& entry : descriptor.entries)
		{
			if (!entry.isEntryPoint)
				continue;

			if (entry.conditionTabs == 0)
			{
				if (IsBroadEntryPointForForm(subject, entry.entryPoint, flags))
					matches.push_back({ &entry, "broad", false, false });
			}
			else
			{
				const auto state = EvalEntrySubject(entry, subject);
				if (state == EvalState::Pass)
					matches.push_back({ &entry, "filter", false, false });
				else if (state == EvalState::Indeterminate && (flags & kFlag_IncludeUnknown) && (flags & kFlag_AllEntryPoints))
					matches.push_back({ &entry, "unknown", false, true });
			}
		}

		return matches;
	}

	bool IsFutureBlocker(const ConditionDescriptor& descriptor)
	{
		switch (descriptor.kind)
		{
			case Kind::ActorValue:
			case Kind::BaseActorValue:
			case Kind::PermanentActorValue:
			case Kind::Level:
			case Kind::HasPerk:
			case Kind::ItemCount:
				return true;
			default:
				return false;
		}
	}

	std::string ApplicableStatus(const ApplicablePerkResult& result)
	{
		bool exactMatch = false;
		for (const auto& match : result.matches)
		{
			if (!match.contextual && !match.unknown)
			{
				exactMatch = true;
				break;
			}
		}

		if (!exactMatch && result.contextual)
			return "contextual";
		if (!exactMatch && result.unknown)
			return "unknown";
		if (result.owned)
			return "owned_active";
		if (result.eligibility.eligible)
			return "takeable";
		if (!result.eligibility.indeterminate.empty())
			return "unknown";

		if (!result.eligibility.blockers.empty())
		{
			for (const auto& [descriptor, ignored] : result.eligibility.blockers)
			{
				if (!IsFutureBlocker(descriptor))
					return "blocked";
			}
			return "future";
		}

		return "blocked";
	}

	ApplicablePerkResult EvaluateApplicablePerk(const PerkDescriptor& descriptor, TESForm* subject, const EvalContext& ctx, UInt32 flags)
	{
		ApplicablePerkResult result;
		result.eligibility = EvaluateEligibility(descriptor, ctx, flags);
		result.matches = GetEntryMatches(descriptor, subject, flags);
		result.relevant = !result.matches.empty();
		result.owned = result.eligibility.ownedRank > 0;

		for (const auto& match : result.matches)
		{
			result.contextual = result.contextual || match.contextual;
			result.unknown = result.unknown || match.unknown;
		}
		result.unknown = result.unknown || !result.eligibility.indeterminate.empty();
		result.status = ApplicableStatus(result);
		return result;
	}

	bool IncludeApplicablePerk(const ApplicablePerkResult& result, const PerkDescriptor& descriptor, UInt32 flags)
	{
		if (!result.relevant)
			return false;
		if (result.owned)
			return true;
		if (descriptor.hidden && !(flags & kFlag_IncludeHidden))
			return false;
		if (!descriptor.playable && !(flags & kFlag_IncludeNonPlayable))
			return false;
		if (descriptor.minLevel == 0 && !(flags & kFlag_IncludeNonLevelUp))
			return false;
		if (result.contextual && !(flags & kFlag_IncludeContextual))
			return false;
		if (result.unknown && !(flags & kFlag_IncludeUnknown) && result.matches.size() == 1 && result.matches[0].unknown)
			return false;
		return true;
	}

	Array* EntryMatchToMap(const EntryMatch& match, Script* scriptObj)
	{
		std::vector<const char*> keys;
		std::vector<Element> values;
		const auto* entry = match.entry;

		AddStringMapValue(keys, values, "rank", Element(static_cast<double>(entry ? entry->rank : 0)));
		AddStringMapValue(keys, values, "entryPoint", Element(static_cast<double>(entry ? entry->entryPoint : -1)));
		AddStringMapValue(keys, values, "entryPointName", Element(EntryPointName(entry ? entry->entryPoint : -1)));
		AddStringMapValue(keys, values, "entryFunction", Element(static_cast<double>(entry ? entry->function : -1)));
		AddStringMapValue(keys, values, "conditionTabs", Element(static_cast<double>(entry ? entry->conditionTabs : 0)));
		AddStringMapValue(keys, values, "match", Element(match.match.c_str()));
		AddStringMapValue(keys, values, "contextual", Element(match.contextual ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "unknown", Element(match.unknown ? 1.0 : 0.0));

		return CreateStringMap(scriptObj, keys, values);
	}

	Array* EntryMatchesArray(const ApplicablePerkResult& result, Script* scriptObj)
	{
		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return nullptr;

		for (const auto& match : result.matches)
			g_arrInterface->AppendElement(arr, Element(EntryMatchToMap(match, scriptObj)));

		return arr;
	}

	Array* ApplicablePerkToMap(const ApplicablePerkResult& result, TESForm* subject, Script* scriptObj)
	{
		const auto& eligibility = result.eligibility;
		std::vector<const char*> keys;
		std::vector<Element> values;

		AddStringMapValue(keys, values, "perk", Element(reinterpret_cast<TESForm*>(eligibility.perk)));
		AddStringMapValue(keys, values, "name", Element(eligibility.name.c_str()));
		AddStringMapValue(keys, values, "subject", Element(subject));
		AddStringMapValue(keys, values, "ownedRank", Element(static_cast<double>(eligibility.ownedRank)));
		AddStringMapValue(keys, values, "nextRank", Element(static_cast<double>(eligibility.nextRank)));
		AddStringMapValue(keys, values, "maxRank", Element(static_cast<double>(eligibility.maxRank)));
		AddStringMapValue(keys, values, "relevant", Element(result.relevant ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "eligible", Element(eligibility.eligible ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "owned", Element(result.owned ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "contextual", Element(result.contextual ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "unknown", Element(result.unknown ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "status", Element(result.status.c_str()));
		AddStringMapValue(keys, values, "blockers", Element(BlockersArray(eligibility, scriptObj)));
		AddStringMapValue(keys, values, "indeterminate", Element(IndeterminateArray(eligibility, scriptObj)));
		AddStringMapValue(keys, values, "matchedEntryPoints", Element(EntryMatchesArray(result, scriptObj)));

		return CreateStringMap(scriptObj, keys, values);
	}

	const char* DisplayName(const std::string& name)
	{
		return name.empty() ? "<unnamed>" : name.c_str();
	}

	void ConsolePrintBlockers(const EligibilityResult& eligibility, const char* commandName)
	{
		if (!IsConsoleMode())
			return;

		const UInt32 formID = eligibility.perk ? eligibility.perk->refID : 0;
		Console_Print("%s >> %s (%08X): %u blocker(s)",
		              commandName,
		              DisplayName(eligibility.name),
		              formID,
		              static_cast<UInt32>(eligibility.blockers.size()));

		const UInt32 count = std::min<UInt32>(static_cast<UInt32>(eligibility.blockers.size()), kConsoleDumpLimit);
		for (UInt32 i = 0; i < count; i++)
		{
			const auto& [descriptor, evaluated] = eligibility.blockers[i];
			const std::string text = BlockerText(descriptor, evaluated);
			Console_Print("  [%u] %s", i, text.c_str());
		}

		if (eligibility.blockers.size() > kConsoleDumpLimit)
			Console_Print("  ... %u more", static_cast<UInt32>(eligibility.blockers.size() - kConsoleDumpLimit));
	}

	void ConsolePrintIndeterminate(const EligibilityResult& eligibility)
	{
		if (!IsConsoleMode() || eligibility.indeterminate.empty())
			return;

		Console_Print("  unknown requirement(s): %u", static_cast<UInt32>(eligibility.indeterminate.size()));
		const UInt32 count = std::min<UInt32>(static_cast<UInt32>(eligibility.indeterminate.size()), kConsoleDumpLimit);
		for (UInt32 i = 0; i < count; i++)
		{
			const auto& descriptor = eligibility.indeterminate[i];
			Console_Print("    [%u] %s kind=%s arg=%u",
			              i,
			              FunctionName(descriptor.rawFunctionId),
			              KindName(descriptor.kind),
			              descriptor.argValue);
		}
	}

	void ConsolePrintEligibility(const EligibilityResult& eligibility)
	{
		if (!IsConsoleMode())
			return;

		const UInt32 formID = eligibility.perk ? eligibility.perk->refID : 0;
		Console_Print("GetPerkEligibility >> %s (%08X): %s eligible=%u owned=%u next=%u max=%u",
		              DisplayName(eligibility.name),
		              formID,
		              eligibility.status.c_str(),
		              eligibility.eligible ? 1 : 0,
		              eligibility.ownedRank,
		              eligibility.nextRank,
		              eligibility.maxRank);

		if (!eligibility.blockers.empty())
			ConsolePrintBlockers(eligibility, "  blockers");
		ConsolePrintIndeterminate(eligibility);
	}

	std::string DescriptorArgText(const ConditionDescriptor& descriptor)
	{
		if (descriptor.argForm)
			return FormName(descriptor.argForm);

		switch (descriptor.kind)
		{
			case Kind::ActorValue:
			case Kind::BaseActorValue:
			case Kind::PermanentActorValue:
				return FormatAVName(descriptor.argValue);
			default:
			{
				char buf[32];
				sprintf_s(buf, "%u", descriptor.argValue);
				return buf;
			}
		}
	}

	void ConsolePrintConditions(const PerkDescriptor& descriptor)
	{
		if (!IsConsoleMode())
			return;

		UInt32 total = 1;
		total += descriptor.trait ? 1 : 0;
		total += descriptor.playable ? 0 : 1;
		total += descriptor.hidden ? 1 : 0;
		for (const auto& group : descriptor.requirements)
			total += static_cast<UInt32>(group.descriptors.size());
		for (const auto& entry : descriptor.entries)
			total += static_cast<UInt32>(entry.filters.size());

		Console_Print("GetPerkConditions >> %s (%08X): %u condition(s)",
		              DisplayName(descriptor.name),
		              descriptor.formID,
		              total);

		UInt32 printed = 0;
		if (descriptor.minLevel > 0 && printed < kConsoleDumpLimit)
		{
			Console_Print("  [%u] data GetLevel >= %u", printed, descriptor.minLevel);
			printed++;
		}
		else if (printed < kConsoleDumpLimit)
		{
			Console_Print("  [%u] data MinLevel = 0 (not level-up selectable)", printed);
			printed++;
		}
		if (descriptor.trait && printed < kConsoleDumpLimit)
		{
			Console_Print("  [%u] data Trait = true", printed);
			printed++;
		}
		if (!descriptor.playable && printed < kConsoleDumpLimit)
		{
			Console_Print("  [%u] data Playable = false", printed);
			printed++;
		}
		if (descriptor.hidden && printed < kConsoleDumpLimit)
		{
			Console_Print("  [%u] data Hidden = true", printed);
			printed++;
		}

		for (const auto& group : descriptor.requirements)
		{
			for (const auto& condition : group.descriptors)
			{
				if (printed >= kConsoleDumpLimit)
					break;

				const std::string argText = DescriptorArgText(condition);
				Console_Print("  [%u] req %s %s %.0f arg=%s",
				              printed,
				              FunctionName(condition.rawFunctionId),
				              ComparisonName(condition.comparison),
				              static_cast<double>(RequiredValue(condition)),
				              argText.c_str());
				printed++;
			}
		}

		for (const auto& entry : descriptor.entries)
		{
			for (const auto& condition : entry.filters)
			{
				if (printed >= kConsoleDumpLimit)
					break;

				const std::string argText = DescriptorArgText(condition);
				Console_Print("  [%u] entry ep=%d rank=%u tab=%d runOn=%s %s %s %.0f arg=%s",
				              printed,
				              condition.entryPoint,
				              condition.rank,
				              condition.tabIndex,
				              RunOnName(condition.runOn),
				              FunctionName(condition.rawFunctionId),
				              ComparisonName(condition.comparison),
				              static_cast<double>(RequiredValue(condition)),
				              argText.c_str());
				printed++;
			}
		}

		if (total > kConsoleDumpLimit)
			Console_Print("  ... %u more", total - kConsoleDumpLimit);
	}

	void ConsolePrintApplicablePerks(TESForm* subject, UInt32 total, const std::vector<ApplicablePerkResult>& rows)
	{
		const std::string subjectName = FormName(subject);
		Console_Print("GetPerksForForm >> %s (%08X): %u perk(s)",
		              subjectName.empty() ? "<unnamed>" : subjectName.c_str(),
		              subject ? subject->refID : 0,
		              total);

		for (UInt32 i = 0; i < rows.size(); i++)
		{
			const auto& row = rows[i];
			Console_Print("  [%u] %s (%08X) status=%s owned=%u next=%u/%u matches=%u",
			              i,
			              DisplayName(row.eligibility.name),
			              row.eligibility.perk ? row.eligibility.perk->refID : 0,
			              row.status.c_str(),
			              row.owned ? 1 : 0,
			              row.eligibility.nextRank,
			              row.eligibility.maxRank,
			              static_cast<UInt32>(row.matches.size()));
		}

		if (total > kConsoleDumpLimit)
			Console_Print("  ... %u more", total - kConsoleDumpLimit);
	}

	bool IncludeInEligibleList(const EligibilityResult& result, const PerkDescriptor& descriptor, UInt32 flags)
	{
		if (result.status == "owned_max")
			return (flags & kFlag_IncludeOwnedMaxRank) != 0;
		if (descriptor.hidden && !(flags & kFlag_IncludeHidden))
			return false;
		if (!descriptor.playable && !(flags & kFlag_IncludeNonPlayable))
			return false;
		if (descriptor.trait && !(flags & kFlag_IncludeTraits))
			return false;
		return result.eligible;
	}

	PerkDescriptor BuildPerkDescriptor(BGSPerk* perk)
	{
		PerkDescriptor descriptor;
		descriptor.perk = perk;
		descriptor.formID = perk ? perk->refID : 0;
		descriptor.name = perk && perk->fullName.name.m_data ? perk->fullName.name.m_data : "";
		descriptor.minLevel = perk ? perk->data.minLevel : 0;
		descriptor.maxRank = perk ? std::max<UInt32>(perk->data.numRanks, 1) : 0;
		descriptor.trait = perk && perk->data.isTrait != 0;
		descriptor.playable = perk && perk->data.isPlayable != 0;
		descriptor.hidden = perk && perk->data.isHidden != 0;

		if (perk)
		{
			descriptor.requirements = ParseRequirementGroups(perk->conditions, perk->refID);
			descriptor.entries = ParseEntries(perk);
		}

		return descriptor;
	}

	BGSPerk* ExtractPerk(TESForm* form)
	{
		return form && form->typeID == kFormType_Perk ? static_cast<BGSPerk*>(form) : nullptr;
	}

	EvalContext MakeEvalContext(Array* deltaArray)
	{
		EvalContext ctx;
		ctx.player = PlayerCharacter::GetSingleton();
		ctx.delta = ParseStateDelta(deltaArray);
		return ctx;
	}

	static ParamInfo kParams_PerkDeltaFlags[3] = {
		{ "perk", kParamType_AnyForm, 0 },
		{ "delta", kParamType_Array, 1 },
		{ "flags", kParamType_Integer, 1 },
	};

	static ParamInfo kParams_DeltaFlags[2] = {
		{ "delta", kParamType_Array, 1 },
		{ "flags", kParamType_Integer, 1 },
	};

	static ParamInfo kParams_PerkFlags[2] = {
		{ "perk", kParamType_AnyForm, 0 },
		{ "flags", kParamType_Integer, 1 },
	};

	static ParamInfo kParams_FormDeltaFlags[3] = {
		{ "subject", kParamType_AnyForm, 0 },
		{ "delta", kParamType_Array, 1 },
		{ "flags", kParamType_Integer, 1 },
	};

	DEFINE_COMMAND_PLUGIN(GetPerkEligibility, "Returns structured player eligibility for a perk", 0, 3, kParams_PerkDeltaFlags);
	DEFINE_COMMAND_PLUGIN(GetPerkBlockers, "Returns structured blockers for a perk", 0, 3, kParams_PerkDeltaFlags);
	DEFINE_COMMAND_PLUGIN(GetEligiblePerks, "Returns currently level-up selectable perks", 0, 2, kParams_DeltaFlags);
	DEFINE_COMMAND_PLUGIN(GetPerkConditions, "Returns parsed perk requirement and entry-point descriptors", 0, 2, kParams_PerkFlags);
	DEFINE_COMMAND_PLUGIN(GetPerksForForm, "Returns perks relevant to a weapon, armour, or item form", 0, 3, kParams_FormDeltaFlags);

	bool EnsurePerkIndex()
	{
		if (g_perks.empty())
			PerkRuntimeFramework::BuildIndex();
		return !g_perks.empty();
	}

	bool Cmd_GetPerkEligibility_Execute(COMMAND_ARGS)
	{
		*result = 0;
		TESForm* form = nullptr;
		Array* deltaArray = nullptr;
		UInt32 flags = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &form, &deltaArray, &flags) || !g_arrInterface)
			return true;
		if (!EnsurePerkIndex())
			return true;

		auto* perk = ExtractPerk(form);
		const auto* descriptor = FindPerkDescriptor(perk);
		if (!descriptor)
			return true;

		auto eligibility = EvaluateEligibility(*descriptor, MakeEvalContext(deltaArray), flags);
		ConsolePrintEligibility(eligibility);
		AssignArrayResult(EligibilityToMap(eligibility, scriptObj), result);
		return true;
	}

	bool Cmd_GetPerkBlockers_Execute(COMMAND_ARGS)
	{
		*result = 0;
		TESForm* form = nullptr;
		Array* deltaArray = nullptr;
		UInt32 flags = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &form, &deltaArray, &flags) || !g_arrInterface)
			return true;
		if (!EnsurePerkIndex())
			return true;

		auto* perk = ExtractPerk(form);
		const auto* descriptor = FindPerkDescriptor(perk);
		if (!descriptor)
			return true;

		auto eligibility = EvaluateEligibility(*descriptor, MakeEvalContext(deltaArray), flags);
		ConsolePrintBlockers(eligibility, "GetPerkBlockers");
		ConsolePrintIndeterminate(eligibility);
		AssignArrayResult(BlockersArray(eligibility, scriptObj), result);
		return true;
	}

	bool Cmd_GetEligiblePerks_Execute(COMMAND_ARGS)
	{
		*result = 0;
		Array* deltaArray = nullptr;
		UInt32 flags = 0;
		ExtractArgs(EXTRACT_ARGS, &deltaArray, &flags);

		if (!g_arrInterface)
			return true;
		if (!EnsurePerkIndex())
			return true;

		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return true;

		auto ctx = MakeEvalContext(deltaArray);
		std::vector<EligibilityResult> consoleRows;
		UInt32 consoleCount = 0;
		for (const auto& descriptor : g_perks)
		{
			auto eligibility = EvaluateEligibility(descriptor, ctx, flags);
			if (IncludeInEligibleList(eligibility, descriptor, flags))
			{
				if (IsConsoleMode())
				{
					consoleCount++;
					if (consoleRows.size() < kConsoleDumpLimit)
						consoleRows.push_back(eligibility);
				}
				g_arrInterface->AppendElement(arr, Element(EligibilityToMap(eligibility, scriptObj)));
			}
		}

		if (IsConsoleMode())
		{
			Console_Print("GetEligiblePerks >> %u perk(s)", consoleCount);
			for (UInt32 i = 0; i < consoleRows.size(); i++)
			{
				const auto& eligibility = consoleRows[i];
				Console_Print("  [%u] %s (%08X) next=%u/%u",
				              i,
				              DisplayName(eligibility.name),
				              eligibility.perk ? eligibility.perk->refID : 0,
				              eligibility.nextRank,
				              eligibility.maxRank);
			}
			if (consoleCount > kConsoleDumpLimit)
				Console_Print("  ... %u more", consoleCount - kConsoleDumpLimit);
		}

		AssignArrayResult(arr, result);
		return true;
	}

	bool Cmd_GetPerkConditions_Execute(COMMAND_ARGS)
	{
		*result = 0;
		TESForm* form = nullptr;
		UInt32 flags = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &form, &flags) || !g_arrInterface)
			return true;
		if (!EnsurePerkIndex())
			return true;

		auto* perk = ExtractPerk(form);
		const auto* descriptor = FindPerkDescriptor(perk);
		if (!descriptor)
			return true;

		ConsolePrintConditions(*descriptor);
		AssignArrayResult(ConditionsArray(*descriptor, scriptObj), result);
		return true;
	}

	bool Cmd_GetPerksForForm_Execute(COMMAND_ARGS)
	{
		*result = 0;
		TESForm* subject = nullptr;
		Array* deltaArray = nullptr;
		UInt32 flags = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &subject, &deltaArray, &flags) || !g_arrInterface || !subject)
			return true;
		if (!EnsurePerkIndex())
			return true;

		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return true;

		auto ctx = MakeEvalContext(deltaArray);
		std::vector<ApplicablePerkResult> consoleRows;
		UInt32 consoleCount = 0;
		for (const auto& descriptor : g_perks)
		{
			auto applicable = EvaluateApplicablePerk(descriptor, subject, ctx, flags);
			if (!IncludeApplicablePerk(applicable, descriptor, flags))
				continue;

			consoleCount++;
			if (consoleRows.size() < kConsoleDumpLimit)
				consoleRows.push_back(applicable);
			g_arrInterface->AppendElement(arr, Element(ApplicablePerkToMap(applicable, subject, scriptObj)));
		}

		ConsolePrintApplicablePerks(subject, consoleCount, consoleRows);
		AssignArrayResult(arr, result);
		return true;
	}
}

namespace PerkRuntimeFramework
{
	bool Init(void* nvse)
	{
		return nvse != nullptr;
	}

	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterTypedCommand(&kCommandInfo_GetPerkEligibility, kRetnType_Array);
		nvse->RegisterTypedCommand(&kCommandInfo_GetPerkBlockers, kRetnType_Array);
		nvse->RegisterTypedCommand(&kCommandInfo_GetEligiblePerks, kRetnType_Array);
		nvse->RegisterTypedCommand(&kCommandInfo_GetPerkConditions, kRetnType_Array);
		nvse->RegisterTypedCommand(&kCommandInfo_GetPerksForForm, kRetnType_Array);
	}

	void BuildIndex()
	{
		g_perks.clear();
		g_perksByFormID.clear();

		auto** dataHandlerPtr = reinterpret_cast<DataHandler**>(kDataHandler);
		if (!*dataHandlerPtr)
			return;

		auto* dataHandler = *dataHandlerPtr;
		for (auto iter = dataHandler->perkList.Begin(); !iter.End(); ++iter)
		{
			BGSPerk* perk = iter.Get();
			if (!perk)
				continue;

			const size_t index = g_perks.size();
			g_perks.push_back(BuildPerkDescriptor(perk));
			g_perksByFormID[perk->refID] = index;
		}

		Log("PerkRuntimeFramework indexed %u perks", static_cast<UInt32>(g_perks.size()));
	}
}
