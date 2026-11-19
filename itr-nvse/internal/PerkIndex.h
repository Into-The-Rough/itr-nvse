#pragma once

#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"

#include <string>
#include <vector>

namespace PerkIndex {

	constexpr UInt32 kFlag_IncludeHidden = 1 << 0;
	constexpr UInt32 kFlag_IncludeNonPlayable = 1 << 1;
	constexpr UInt32 kFlag_IncludeTraits = 1 << 2;
	constexpr UInt32 kFlag_IncludeOwnedMaxRank = 1 << 3;
	constexpr UInt32 kFlag_IncludeNonLevelUp = 1 << 4;
	constexpr UInt32 kFlag_IncludeContextual = 1 << 5;
	constexpr UInt32 kFlag_IncludeUnknown = 1 << 6;
	constexpr UInt32 kFlag_AllEntryPoints = 1 << 7;
	constexpr UInt32 kFlag_IncludeBlocked = 1 << 8;

	constexpr UInt8 kMaxEntryPointFilterTabs = 3; //EntryPointConditions is exactly tab1..tab3, conditionTabs above 3 is malformed data

	enum class Source : UInt8 {
		PerkDataRequirement,
		TopLevelRequirement,
		EntryPointFilter,
	};

	enum class Kind : UInt8 {
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

	struct ConditionDescriptor {
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

	struct RequirementGroup {
		bool any = false;
		std::vector<ConditionDescriptor> descriptors;
	};

	struct EntryDescriptor {
		BGSPerkEntry* raw = nullptr;
		bool isEntryPoint = false;
		UInt32 rank = 0;
		UInt16 type = 0;
		SInt32 entryPoint = -1;
		SInt32 function = -1;
		UInt8 conditionTabs = 0;
		std::vector<ConditionDescriptor> filters;
	};

	struct PerkDescriptor {
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

	//rebuilds static main-thread perk index, invalidating prior descriptor pointers/references
	void BuildIndex();
	bool EnsureIndex();
	const std::vector<PerkDescriptor>& GetPerks();
	const PerkDescriptor* FindPerk(BGSPerk* perk);

	ConditionDescriptor MinLevelRequirement(const PerkDescriptor& perk);
	ConditionDescriptor MetadataRequirement(const PerkDescriptor& perk, Kind kind);
	double RequiredValue(const ConditionDescriptor& descriptor);

	const char* SourceName(Source source);
	const char* KindName(Kind kind);
	const char* FunctionName(UInt32 functionId);
	const char* RunOnName(UInt32 runOn);
	const char* EntryPointName(SInt32 entryPoint);
	const char* ComparisonName(UInt8 comparison);
	std::string FormatAVName(UInt32 av);
	std::string FormName(TESForm* form);

}
