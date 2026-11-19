#pragma once

#include "internal/PerkIndex.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace PerkEvaluator {

	enum class EvalState : UInt8 {
		Pass,
		Fail,
		Indeterminate,
	};

	struct StateDelta {
		bool hasLevel = false;
		double level = 0.0;
		std::unordered_map<UInt32, double> av;
		std::unordered_map<UInt32, double> avDelta;
		std::unordered_map<UInt32, double> itemsAdded;
		std::unordered_map<UInt32, double> itemsRemoved;
		std::unordered_set<UInt32> perksAdded;
		std::unordered_set<UInt32> perksRemoved;
	};

	struct EvalContext {
		PlayerCharacter* player = nullptr;
		StateDelta delta;
	};

	struct EvaluatedCondition {
		EvalState state = EvalState::Indeterminate;
		double currentValue = 0.0;
		double requiredValue = 0.0;
		double missingValue = 0.0;
	};

	struct EligibilityResult {
		BGSPerk* perk = nullptr;
		std::string name;
		UInt32 ownedRank = 0;
		UInt32 nextRank = 0;
		UInt32 maxRank = 0;
		bool eligible = false;
		std::string status = "unknown";
		std::vector<std::pair<PerkIndex::ConditionDescriptor, EvaluatedCondition>> blockers;
		std::vector<PerkIndex::ConditionDescriptor> indeterminate;
	};

	struct EntryMatch {
		const PerkIndex::EntryDescriptor* entry = nullptr;
		std::string match;
		bool contextual = false;
		bool unknown = false;
	};

	struct ApplicablePerkResult {
		EligibilityResult eligibility;
		std::vector<EntryMatch> matches;
		bool relevant = false;
		bool owned = false;
		bool contextual = false;
		bool unknown = false;
		std::string status;
	};

	EligibilityResult EvaluateEligibility(const PerkIndex::PerkDescriptor& descriptor, const EvalContext& ctx, UInt32 flags);
	bool IncludeInEligibleList(const EligibilityResult& result, const PerkIndex::PerkDescriptor& descriptor, UInt32 flags);

	ApplicablePerkResult EvaluateApplicablePerk(const PerkIndex::PerkDescriptor& descriptor, TESForm* subject, const EvalContext& ctx, UInt32 flags);
	bool IncludeApplicablePerk(const ApplicablePerkResult& result, const PerkIndex::PerkDescriptor& descriptor, UInt32 flags);

	std::string BlockerText(const PerkIndex::ConditionDescriptor& descriptor, const EvaluatedCondition& evaluated);

}
