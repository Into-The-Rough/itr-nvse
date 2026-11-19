#pragma once

#include <algorithm>
#include <vector>

#include "common/ITypes.h"

namespace DetectionSoundLogic {
	constexpr UInt32 kNpcReactionCooldownMs = 12000;
	constexpr UInt32 kInvestigatorRedirectCooldownMs = 3500;
	constexpr UInt32 kGlobalBarkCooldownMs = 5000;

	enum CandidateKind : UInt8 {
		kCandidate_None = 0,
		kCandidate_DetectedCombatHostile = 1,
		kCandidate_ActiveCautionHostile = 2,
		kCandidate_IdleUndetectedHostile = 3,
		kCandidate_Neutral = 4,
	};

	struct ActorCooldown {
		UInt32 refID;
		UInt32 lastTickMs;
	};

	struct ClassifiedCandidate {
		UInt32 refID;
		float distSq;
		CandidateKind kind;
		bool canSpeak;
	};

	struct DistractionPlan {
		UInt32 newScoutRefID = 0;
		UInt32 redirectScoutRefID = 0;
		UInt32 speakerRefID = 0;
		bool scoutLocked = false;
		std::vector<UInt32> turnRefIDs;
		std::vector<UInt32> pushCombatSearchRefIDs;
	};

	inline bool IsCooldownElapsed(UInt32 nowMs, UInt32 stampMs, UInt32 cooldownMs)
	{
		return static_cast<SInt32>(nowMs - stampMs) >= static_cast<SInt32>(cooldownMs);
	}

	inline bool IsActorCooldownReady(const std::vector<ActorCooldown>& entries, UInt32 refID, UInt32 nowMs, UInt32 cooldownMs)
	{
		for (const auto& entry : entries) {
			if (entry.refID == refID)
				return IsCooldownElapsed(nowMs, entry.lastTickMs, cooldownMs);
		}
		return true;
	}

	inline void StampActorCooldown(std::vector<ActorCooldown>& entries, UInt32 refID, UInt32 nowMs, UInt32 pruneMs)
	{
		bool updated = false;
		for (size_t i = 0; i < entries.size();) {
			if (entries[i].refID == refID) {
				entries[i].lastTickMs = nowMs;
				updated = true;
				++i;
				continue;
			}
			if (IsCooldownElapsed(nowMs, entries[i].lastTickMs, pruneMs)) {
				entries[i] = entries.back();
				entries.pop_back();
				continue;
			}
			++i;
		}
		if (!updated && refID)
			entries.push_back({refID, nowMs});
	}

	inline DistractionPlan BuildDistractionPlan(
		std::vector<ClassifiedCandidate> candidates,
		bool allowStartCaution,
		bool allowBark,
		const std::vector<ActorCooldown>& reactionCooldowns,
		const std::vector<ActorCooldown>& investigatorCooldowns,
		bool hasPendingBark,
		bool hasLastBark,
		UInt32 lastBarkTickMs,
		UInt32 nowMs)
	{
		DistractionPlan plan;
		if (candidates.empty())
			return plan;

		std::sort(candidates.begin(), candidates.end(), [](const ClassifiedCandidate& a, const ClassifiedCandidate& b) {
			return a.distSq < b.distSq;
		});

		plan.turnRefIDs.reserve(candidates.size());

		//an enemy already searching in [CAUTION] locks out peeling a second campmate
		for (const auto& cand : candidates) {
			if (cand.kind == kCandidate_ActiveCautionHostile) {
				plan.scoutLocked = true;
				if (IsActorCooldownReady(investigatorCooldowns, cand.refID, nowMs, kInvestigatorRedirectCooldownMs))
					plan.redirectScoutRefID = cand.refID;
				break;
			}
		}

		UInt32 neutralSpeakerRefID = 0;
		bool scoutCanSpeak = false;

		for (const auto& cand : candidates) {
			switch (cand.kind) {
				case kCandidate_DetectedCombatHostile:
					plan.pushCombatSearchRefIDs.push_back(cand.refID);
					break;

				case kCandidate_ActiveCautionHostile:
					if (cand.refID == plan.redirectScoutRefID)
						scoutCanSpeak = cand.canSpeak;
					break;

				case kCandidate_IdleUndetectedHostile:
					if (allowStartCaution && !plan.scoutLocked) {
						plan.scoutLocked = true;
						plan.newScoutRefID = cand.refID;
						scoutCanSpeak = cand.canSpeak;
					} else if (IsActorCooldownReady(reactionCooldowns, cand.refID, nowMs, kNpcReactionCooldownMs)) {
						plan.turnRefIDs.push_back(cand.refID);
					}
					break;

				case kCandidate_Neutral:
					if (IsActorCooldownReady(reactionCooldowns, cand.refID, nowMs, kNpcReactionCooldownMs)) {
						plan.turnRefIDs.push_back(cand.refID);
						if (!neutralSpeakerRefID && cand.canSpeak)
							neutralSpeakerRefID = cand.refID;
					}
					break;

				default:
					break;
			}
		}

		if (allowBark && !hasPendingBark && (!hasLastBark || IsCooldownElapsed(nowMs, lastBarkTickMs, kGlobalBarkCooldownMs))) {
			if (plan.newScoutRefID && scoutCanSpeak)
				plan.speakerRefID = plan.newScoutRefID;
			else if (plan.redirectScoutRefID && scoutCanSpeak)
				plan.speakerRefID = plan.redirectScoutRefID;
			else if (!plan.newScoutRefID && !plan.redirectScoutRefID)
				plan.speakerRefID = neutralSpeakerRefID;
		}

		return plan;
	}
}
