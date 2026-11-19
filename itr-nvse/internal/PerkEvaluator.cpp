#include "internal/PerkEvaluator.h"
#include "internal/GameLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace PerkEvaluator {

	using PerkIndex::ConditionDescriptor;
	using PerkIndex::EntryDescriptor;
	using PerkIndex::FormatAVName;
	using PerkIndex::FormName;
	using PerkIndex::kFlag_AllEntryPoints;
	using PerkIndex::kFlag_IncludeBlocked;
	using PerkIndex::kFlag_IncludeContextual;
	using PerkIndex::kFlag_IncludeHidden;
	using PerkIndex::kFlag_IncludeNonLevelUp;
	using PerkIndex::kFlag_IncludeNonPlayable;
	using PerkIndex::kFlag_IncludeOwnedMaxRank;
	using PerkIndex::kFlag_IncludeTraits;
	using PerkIndex::kFlag_IncludeUnknown;
	using PerkIndex::Kind;
	using PerkIndex::kMaxEntryPointFilterTabs;
	using PerkIndex::MetadataRequirement;
	using PerkIndex::MinLevelRequirement;
	using PerkIndex::PerkDescriptor;
	using PerkIndex::RequiredValue;
	using PerkIndex::RequirementGroup;

	namespace {

		struct EvaluatedGroup {
			EvalState state = EvalState::Pass;
			std::vector<std::pair<ConditionDescriptor, EvaluatedCondition>> failed;
			std::vector<ConditionDescriptor> indeterminate;
		};

		struct FilterEval {
			EvalState state = EvalState::Indeterminate;
			bool matched = false;
			bool hasFormSelector = false;
			bool hasPositiveFormSelector = false;
		};

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
			result.blockers.push_back({descriptor, evaluated});
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

		bool Compare(double current, UInt8 comparison, double required)
		{
			const double epsilon = 0.0001;
			switch (comparison) {
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
			switch (comparison) {
				case 0:
				case 2:
				case 3:
					return current < required ? required - current : 0.0;
				default:
					return 0.0;
			}
		}

		void* GetActorValueOwner(PlayerCharacter* player)
		{
			return ActorGetActorValueOwner(player);
		}

		double ReadActorValue(PlayerCharacter* player, UInt32 avCode)
		{
			auto* owner = GetActorValueOwner(player);
			if (!owner)
				return 0.0;

			using GetActorValue_t = float(__thiscall*)(void*, UInt32);
			const auto GetActorValue = reinterpret_cast<GetActorValue_t>(0x66EF50);
			return static_cast<double>(GetActorValue(owner, avCode));
		}

		double ReadBaseActorValue(PlayerCharacter* player, UInt32 avCode)
		{
			auto* owner = GetActorValueOwner(player);
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
			auto* owner = GetActorValueOwner(player);
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
			if (descriptor.usesGlobalComparison && !descriptor.comparisonGlobal) {
				out.state = EvalState::Indeterminate;
				return out;
			}

			out.requiredValue = RequiredValue(descriptor);

			switch (descriptor.kind) {
				case Kind::ActorValue:
					out.currentValue = ApplyActorValueDelta(ctx, descriptor.argValue, ReadActorValue(ctx.player, descriptor.argValue));
					break;
				case Kind::BaseActorValue:
					out.currentValue = ApplyActorValueDelta(ctx, descriptor.argValue, ReadBaseActorValue(ctx.player, descriptor.argValue));
					break;
				case Kind::PermanentActorValue:
					out.currentValue = ApplyActorValueDelta(ctx, descriptor.argValue, ReadPermanentActorValue(ctx.player, descriptor.argValue));
					break;
				case Kind::Level:
					out.currentValue = ctx.delta.hasLevel ? ctx.delta.level : ReadLevel(ctx.player);
					break;
				case Kind::HasPerk: {
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

			for (const auto& descriptor : group.descriptors) {
				auto evaluated = EvaluateCondition(descriptor, ctx);
				if (evaluated.state == EvalState::Pass) {
					anyPass = true;
				} else if (evaluated.state == EvalState::Fail) {
					out.failed.push_back({descriptor, evaluated});
				} else {
					anyUnknown = true;
					out.indeterminate.push_back(descriptor);
				}

				if (!group.any && evaluated.state == EvalState::Fail)
					break;
			}

			if (group.any) {
				if (anyPass) {
					out.state = EvalState::Pass;
					out.failed.clear();
					out.indeterminate.clear();
				} else if (anyUnknown) {
					out.state = EvalState::Indeterminate;
					out.failed.clear();
				} else {
					out.state = EvalState::Fail;
				}
			} else {
				if (!out.failed.empty())
					out.state = EvalState::Fail;
				else if (anyUnknown)
					out.state = EvalState::Indeterminate;
				else
					out.state = EvalState::Pass;
			}

			return out;
		}

		bool HasRequirements(const PerkDescriptor& descriptor)
		{
			for (const auto& group : descriptor.requirements) {
				if (!group.descriptors.empty())
					return true;
			}
			return false;
		}

		bool IsLikelyScriptGranted(const PerkDescriptor& descriptor)
		{
			return !descriptor.trait && !HasRequirements(descriptor) && (descriptor.hidden || !descriptor.playable);
		}

		bool SameForm(TESForm* lhs, TESForm* rhs)
		{
			return lhs && rhs && lhs->refID == rhs->refID;
		}

		bool FormListContains(BGSListForm* list, TESForm* subject, std::unordered_set<UInt32>& visited)
		{
			if (!list || !subject || !visited.insert(list->refID).second)
				return false;

			for (auto iter = list->list.Begin(); !iter.End(); ++iter) {
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

			for (auto iter = list->list.Begin(); !iter.End(); ++iter) {
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

			if (argForm->typeID == kFormType_ListForm) {
				std::unordered_set<UInt32> visited;
				return FormListHasSubjectType(static_cast<BGSListForm*>(argForm), subject, visited);
			}

			return argForm->typeID == subject->typeID;
		}

		UInt32 WeaponAnimType(TESObjectWEAP* weapon)
		{
			static constexpr UInt32 kWeaponTypeToAnim[] = {1, 2, 3, 4, 4, 5, 6, 5, 7, 8, 9, 10, 11, 9};
			if (!weapon || weapon->eWeaponType >= sizeof(kWeaponTypeToAnim) / sizeof(kWeaponTypeToAnim[0]))
				return 1;

			return kWeaponTypeToAnim[weapon->eWeaponType];
		}

		EvalState EvalFormCondition(const ConditionDescriptor& descriptor, TESForm* subject)
		{
			if (descriptor.usesGlobalComparison && !descriptor.comparisonGlobal)
				return EvalState::Indeterminate;

			double currentValue = 0.0;
			switch (descriptor.kind) {
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
					if (!subject || !EquippedArgMatches(descriptor.argForm, subject))
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

			for (const auto& descriptor : entry.filters) {
				if (descriptor.tabIndex != tabIndex)
					continue;

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

		bool IsFormCondition(const ConditionDescriptor& descriptor)
		{
			switch (descriptor.kind) {
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
			switch (descriptor.kind) {
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

			switch (descriptor.kind) {
				case Kind::FormID:
					return descriptor.argForm && descriptor.argForm->typeID == subject->typeID;
				case Kind::FormList:
				case Kind::WeaponList:
				case Kind::Equipped:
					return EquippedArgMatches(descriptor.argForm, subject);
				default:
					return false;
			}
		}

		bool CanMatchSubject(const EntryDescriptor& entry, const ConditionDescriptor& descriptor, TESForm* subject)
		{
			if (!subject || !RunsOnSubject(descriptor))
				return false;

			switch (descriptor.kind) {
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

			for (const auto& descriptor : group.descriptors) {
				if (!IsFormCondition(descriptor)) {
					anyOpaque = true;
					continue;
				}
				if (!CanMatchSubject(entry, descriptor, subject))
					continue;

				matched = true;
				const auto state = EvalFormCondition(descriptor, subject);
				if (IsFormSelector(descriptor)) {
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

			if (group.any) {
				if (anyPass)
					return {EvalState::Pass, true, hasFormSelector, hasPositiveFormSelector};
				return {anyUnknown || anyOpaque ? EvalState::Indeterminate : EvalState::Fail, true, hasFormSelector, hasPositiveFormSelector};
			}

			if (anyFail)
				return {EvalState::Fail, true, hasFormSelector, hasPositiveFormSelector};
			return {anyUnknown ? EvalState::Indeterminate : EvalState::Pass, true, hasFormSelector, hasPositiveFormSelector};
		}

		FilterEval EvalEntryFilters(const EntryDescriptor& entry, TESForm* subject)
		{
			bool matched = false;
			bool anyUnknown = false;
			bool hasFormSelector = false;
			bool hasPositiveFormSelector = false;
			const UInt8 count = std::min<UInt8>(entry.conditionTabs, kMaxEntryPointFilterTabs);
			for (UInt8 i = 0; i < count; i++) {
				const auto groups = BuildFilterGroups(entry, i + 1);
				for (const auto& group : groups) {
					const auto evaluated = EvalFilterGroup(entry, group, subject);
					if (!evaluated.matched)
						continue;

					matched = true;
					hasFormSelector = hasFormSelector || evaluated.hasFormSelector;
					hasPositiveFormSelector = hasPositiveFormSelector || evaluated.hasPositiveFormSelector;
					if (evaluated.state == EvalState::Fail)
						return {EvalState::Fail, true, hasFormSelector, hasPositiveFormSelector};
					if (evaluated.state == EvalState::Indeterminate)
						anyUnknown = true;
				}
			}

			if (!matched)
				return {};
			if (hasFormSelector && !hasPositiveFormSelector)
				anyUnknown = true;

			return {anyUnknown ? EvalState::Indeterminate : EvalState::Pass, true, hasFormSelector, hasPositiveFormSelector};
		}

		bool IsBroadEntryPointForForm(TESForm* subject, SInt32 entryPoint, UInt32 flags)
		{
			if (!subject || entryPoint < 0)
				return false;
			if (flags & kFlag_AllEntryPoints)
				return true;

			if (subject->IsWeapon()) {
				switch (entryPoint) {
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

			if (subject->IsArmor()) {
				switch (entryPoint) {
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

			switch (entryPoint) {
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

			for (const auto& entry : descriptor.entries) {
				if (!entry.isEntryPoint)
					continue;

				if (entry.conditionTabs == 0) {
					if (IsBroadEntryPointForForm(subject, entry.entryPoint, flags))
						matches.push_back({&entry, "broad", false, false});
				} else {
					const auto state = EvalEntrySubject(entry, subject);
					if (state == EvalState::Pass)
						matches.push_back({&entry, "filter", false, false});
					else if (state == EvalState::Indeterminate && (flags & kFlag_IncludeUnknown) && (flags & kFlag_AllEntryPoints))
						matches.push_back({&entry, "unknown", false, true});
				}
			}

			return matches;
		}

		bool IsFutureBlocker(const ConditionDescriptor& descriptor)
		{
			switch (descriptor.kind) {
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
			for (const auto& match : result.matches) {
				if (!match.contextual && !match.unknown) {
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

			if (!result.eligibility.blockers.empty()) {
				for (const auto& [descriptor, ignored] : result.eligibility.blockers) {
					if (!IsFutureBlocker(descriptor))
						return "blocked";
				}
				return "future";
			}

			return "blocked";
		}

	}

	EligibilityResult EvaluateEligibility(const PerkDescriptor& descriptor, const EvalContext& ctx, UInt32 flags)
	{
		EligibilityResult result;
		result.perk = descriptor.perk;
		result.name = descriptor.name;
		result.maxRank = descriptor.maxRank;
		result.ownedRank = GetOwnedRank(ctx.player, descriptor.perk, descriptor.maxRank, ctx.delta);
		result.nextRank = result.ownedRank < result.maxRank ? result.ownedRank + 1 : result.maxRank;

		if (result.ownedRank >= result.maxRank && result.maxRank > 0) {
			AddBlocker(result, MetadataRequirement(descriptor, Kind::OwnedMaxRank), FailedRequirement(result.ownedRank, result.maxRank));
			result.status = "owned_max";
			return result;
		}

		if (IsLikelyScriptGranted(descriptor))
			AddBlocker(result, MetadataRequirement(descriptor, Kind::Scripted), FailedRequirement(1.0, 0.0));
		if (descriptor.trait && !(flags & kFlag_IncludeTraits))
			AddBlocker(result, MetadataRequirement(descriptor, Kind::Trait), FailedRequirement(1.0, 0.0));
		if (!descriptor.playable && !(flags & kFlag_IncludeNonPlayable))
			AddBlocker(result, MetadataRequirement(descriptor, Kind::NonPlayable), FailedRequirement(0.0, 1.0));
		if (descriptor.hidden && !(flags & kFlag_IncludeHidden))
			AddBlocker(result, MetadataRequirement(descriptor, Kind::Hidden), FailedRequirement(1.0, 0.0));

		const bool hasDelta = HasStateDelta(ctx.delta);
		if (descriptor.minLevel == 0 && !(flags & kFlag_IncludeNonLevelUp)) {
			AddBlocker(result, MetadataRequirement(descriptor, Kind::NonLevelUp), FailedRequirement(0.0, 1.0));
		} else if (!hasDelta && !(flags & kFlag_IncludeNonLevelUp) && descriptor.playable && !descriptor.trait && !EngineLevelUpMenuBaseFilter(descriptor.perk, ctx.player)) {
			AddBlocker(result, MetadataRequirement(descriptor, Kind::MenuFiltered), FailedRequirement(0.0, 1.0));
		}

		const double level = ctx.delta.hasLevel ? ctx.delta.level : ReadLevel(ctx.player);
		if (descriptor.minLevel > 0 && level < descriptor.minLevel) {
			auto blocker = MinLevelRequirement(descriptor);
			AddBlocker(result, blocker, FailedRequirement(level, descriptor.minLevel, descriptor.minLevel - level));
		}

		std::vector<std::pair<ConditionDescriptor, EvaluatedCondition>> parsedFailures;
		std::vector<ConditionDescriptor> parsedIndeterminate;
		for (const auto& group : descriptor.requirements) {
			auto evaluated = EvaluateGroup(group, ctx);
			if (evaluated.state == EvalState::Fail)
				parsedFailures.insert(parsedFailures.end(), evaluated.failed.begin(), evaluated.failed.end());
			else if (evaluated.state == EvalState::Indeterminate)
				parsedIndeterminate.insert(parsedIndeterminate.end(), evaluated.indeterminate.begin(), evaluated.indeterminate.end());
		}

		if (!hasDelta && HasRequirements(descriptor)) {
			if (!EngineActorHasRequirements(descriptor.perk, ctx.player)) {
				if (!parsedFailures.empty())
					result.blockers.insert(result.blockers.end(), parsedFailures.begin(), parsedFailures.end());
				else
					AddBlocker(result, MetadataRequirement(descriptor, Kind::EngineRequirements), FailedRequirement(0.0, 1.0));
			}
		} else {
			result.blockers.insert(result.blockers.end(), parsedFailures.begin(), parsedFailures.end());
			result.indeterminate.insert(result.indeterminate.end(), parsedIndeterminate.begin(), parsedIndeterminate.end());
		}

		if (result.blockers.empty() && result.indeterminate.empty()) {
			result.eligible = true;
			result.status = "takeable";
		} else if (!result.blockers.empty()) {
			result.status = IsLikelyScriptGranted(descriptor) ? "scripted" : "blocked";
		} else {
			result.status = "unknown";
		}

		return result;
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
		if (result.eligible)
			return true;
		if ((flags & kFlag_IncludeBlocked) == 0)
			return false;
		//flag 256: include only greyed level-up menu entries, not NonLevelUp/MenuFiltered/Scripted perks
		for (const auto& blocker : result.blockers) {
			const Kind kind = blocker.first.kind;
			if (kind == Kind::NonLevelUp || kind == Kind::MenuFiltered || kind == Kind::Scripted)
				return false;
		}
		return true;
	}

	ApplicablePerkResult EvaluateApplicablePerk(const PerkDescriptor& descriptor, TESForm* subject, const EvalContext& ctx, UInt32 flags)
	{
		ApplicablePerkResult result;
		result.eligibility = EvaluateEligibility(descriptor, ctx, flags);
		result.matches = GetEntryMatches(descriptor, subject, flags);
		result.relevant = !result.matches.empty();
		result.owned = result.eligibility.ownedRank > 0;

		for (const auto& match : result.matches) {
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

	std::string BlockerText(const ConditionDescriptor& descriptor, const EvaluatedCondition& evaluated)
	{
		char buf[256];
		switch (descriptor.kind) {
			case Kind::OwnedMaxRank:
				snprintf(buf, sizeof(buf), "Already owned at max rank %.0f/%.0f", evaluated.currentValue, evaluated.requiredValue);
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
				snprintf(buf, sizeof(buf), "%s %.0f/%.0f, missing %.0f",
					FormatAVName(descriptor.argValue).c_str(), evaluated.currentValue, evaluated.requiredValue, evaluated.missingValue);
				return buf;
			case Kind::Level:
				snprintf(buf, sizeof(buf), "Level %.0f/%.0f, missing %.0f",
					evaluated.currentValue, evaluated.requiredValue, evaluated.missingValue);
				return buf;
			case Kind::HasPerk: {
				const std::string name = FormName(descriptor.argForm);
				snprintf(buf, sizeof(buf), "Requires %s", name.empty() ? "perk" : name.c_str());
				return buf;
			}
			case Kind::ItemCount: {
				const std::string name = FormName(descriptor.argForm);
				snprintf(buf, sizeof(buf), "%s %.0f/%.0f, missing %.0f",
					name.empty() ? "Item" : name.c_str(), evaluated.currentValue, evaluated.requiredValue, evaluated.missingValue);
				return buf;
			}
			default:
				return "Opaque requirement";
		}
	}

}
