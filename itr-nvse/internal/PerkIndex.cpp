#include "internal/PerkIndex.h"
#include "internal/GameGlobals.h"
#include "internal/GameLayout.h"
#include "internal/globals.h"
#include "nvse/GameData.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace PerkIndex {

	namespace {

		constexpr UInt32 kConditionFlag_OR = 0x01;
		constexpr UInt32 kConditionFlag_UseGlobal = 0x04;

		constexpr UInt32 kCond_GetActorValue = 14;
		constexpr UInt32 kCond_GetItemCount = 47;
		constexpr UInt32 kCond_GetIsID = 72;
		constexpr UInt32 kCond_GetLevel = 80;
		constexpr UInt32 kCond_GetWeaponAnimType = 108;
		constexpr UInt32 kCond_IsWeaponSkillType = 109;
		constexpr UInt32 kCond_GetEquipped = 182;
		constexpr UInt32 kCond_GetBaseActorValue = 277;
		constexpr UInt32 kCond_IsInList = 372;
		constexpr UInt32 kCond_IsWeaponInList = 399;
		constexpr UInt32 kCond_HasPerk = 449;
		constexpr UInt32 kCond_GetPermanentActorValue = 495;

		constexpr UInt16 kPerkEntryType_Quest = 0x0C24;
		constexpr UInt16 kPerkEntryType_Ability = 0x0B27;
		constexpr UInt32 kVtbl_BGSEntryPointPerkEntry = 0x1046D0C;

		std::vector<PerkDescriptor> g_perks;
		std::unordered_map<UInt32, size_t> g_perksByFormID;

		const char* ActorValueName(UInt32 av)
		{
			switch (av) {
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
			descriptor.isOr = (condition->type & kConditionFlag_OR) != 0;
			descriptor.usesGlobalComparison = (condition->type & kConditionFlag_UseGlobal) != 0;
			descriptor.comparisonValue = descriptor.usesGlobalComparison ? 0.0f : condition->comparisonValue.value;
			descriptor.comparisonGlobal = descriptor.usesGlobalComparison ? reinterpret_cast<TESGlobal*>(condition->comparisonValue.global) : nullptr;

			switch (descriptor.rawFunctionId) {
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

			for (auto iter = conditions.Begin(); !iter.End(); ++iter) {
				Condition* condition = iter.Get();
				if (!condition)
					continue;

				auto descriptor = ParseCondition(condition, Source::TopLevelRequirement, perkFormID, 0, -1, -1, -1);
				current.any = current.any || descriptor.isOr;
				current.descriptors.push_back(descriptor);

				if (!descriptor.isOr) {
					groups.push_back(current);
					current = RequirementGroup();
				}
			}

			if (!current.descriptors.empty())
				groups.push_back(current);

			return groups;
		}

		void AppendEntryConditions(std::vector<ConditionDescriptor>& out, tList<Condition>& conditions, UInt32 perkFormID, UInt32 rank, SInt32 entryPoint, SInt32 function, SInt32 tabIndex)
		{
			for (auto iter = conditions.Begin(); !iter.End(); ++iter) {
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

			for (auto iter = perk->entries.Begin(); !iter.End(); ++iter) {
				BGSPerkEntry* entry = iter.Get();
				if (!entry)
					continue;

				EntryDescriptor descriptor;
				descriptor.raw = entry;
				descriptor.isEntryPoint = IsEntryPointPerkEntry(entry);
				descriptor.rank = static_cast<UInt32>(entry->rank) + 1;
				descriptor.type = entry->type;

				if (descriptor.isEntryPoint) {
					auto* ep = static_cast<BGSEntryPointPerkEntry*>(entry);
					descriptor.entryPoint = ep->entryPoint;
					descriptor.function = ep->function;
					descriptor.conditionTabs = ep->conditionTabs;

					if (ep->conditions) {
						auto* tabs = reinterpret_cast<tList<Condition>*>(ep->conditions);
						const UInt8 count = std::min<UInt8>(descriptor.conditionTabs, kMaxEntryPointFilterTabs);
						for (UInt8 i = 0; i < count; i++)
							AppendEntryConditions(descriptor.filters, tabs[i], perk->refID, descriptor.rank, descriptor.entryPoint, descriptor.function, i + 1);
					}
				} else if (entry->type == kPerkEntryType_Quest || entry->type == kPerkEntryType_Ability) {
					descriptor.entryPoint = -1;
					descriptor.function = -1;
				}

				entries.push_back(descriptor);
			}

			return entries;
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

			if (perk) {
				descriptor.requirements = ParseRequirementGroups(perk->conditions, perk->refID);
				descriptor.entries = ParseEntries(perk);
			}

			return descriptor;
		}

	}

	const char* SourceName(Source source)
	{
		switch (source) {
			case Source::PerkDataRequirement: return "PerkDataRequirement";
			case Source::TopLevelRequirement: return "TopLevelRequirement";
			default: return "EntryPointFilter";
		}
	}

	const char* KindName(Kind kind)
	{
		switch (kind) {
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
		switch (functionId) {
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
		switch (runOn) {
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
		switch (entryPoint) {
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
		switch (comparison) {
			case 0: return "==";
			case 1: return "!=";
			case 2: return ">";
			case 3: return ">=";
			case 4: return "<";
			case 5: return "<=";
			default: return "?";
		}
	}

	std::string FormatAVName(UInt32 av)
	{
		const char* name = ActorValueName(av);
		if (name)
			return name;

		char buf[32];
		snprintf(buf, sizeof(buf), "AV%u", av);
		return buf;
	}

	std::string FormName(TESForm* form)
	{
		if (!form)
			return "";

		if (form->typeID == kFormType_Perk) {
			auto* perk = static_cast<BGSPerk*>(form);
			if (perk->fullName.name.m_data)
				return perk->fullName.name.m_data;
		}

		const char* name = form->GetName();
		return name ? name : "";
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

	double RequiredValue(const ConditionDescriptor& descriptor)
	{
		if (descriptor.usesGlobalComparison && descriptor.comparisonGlobal)
			return descriptor.comparisonGlobal->data;
		return descriptor.comparisonValue;
	}

	void BuildIndex()
	{
		g_perks.clear();
		g_perksByFormID.clear();

		auto** dataHandlerPtr = reinterpret_cast<DataHandler**>(g_dataHandlerPtr);
		if (!*dataHandlerPtr)
			return;

		auto* dataHandler = *dataHandlerPtr;
		for (auto iter = dataHandler->perkList.Begin(); !iter.End(); ++iter) {
			BGSPerk* perk = iter.Get();
			if (!perk)
				continue;

			const size_t index = g_perks.size();
			g_perks.push_back(BuildPerkDescriptor(perk));
			g_perksByFormID[perk->refID] = index;
		}

		Log("PerkIndex indexed %u perks", static_cast<UInt32>(g_perks.size()));
	}

	bool EnsureIndex()
	{
		if (g_perks.empty())
			BuildIndex();
		return !g_perks.empty();
	}

	const std::vector<PerkDescriptor>& GetPerks()
	{
		return g_perks;
	}

	const PerkDescriptor* FindPerk(BGSPerk* perk)
	{
		if (!perk)
			return nullptr;

		auto it = g_perksByFormID.find(perk->refID);
		if (it == g_perksByFormID.end())
			return nullptr;

		return &g_perks[it->second];
	}

}
