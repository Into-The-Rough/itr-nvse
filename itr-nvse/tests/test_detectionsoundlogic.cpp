#include "test.h"

#include "../internal/DetectionSoundLogic.h"

using namespace DetectionSoundLogic;

TEST(DetectionSound_OneScoutOutOfCampAndBystandersTurn)
{
	std::vector<ClassifiedCandidate> cands = {
		{0x00100003, 800.0f * 800.0f, kCandidate_IdleUndetectedHostile, true},
		{0x00100001, 250.0f * 250.0f, kCandidate_IdleUndetectedHostile, true},
		{0x00100002, 500.0f * 500.0f, kCandidate_IdleUndetectedHostile, true},
	};
	std::vector<ActorCooldown> reactCd;
	std::vector<ActorCooldown> investCd;

	const DistractionPlan plan = BuildDistractionPlan(
		cands, true, true, reactCd, investCd, false, false, 0, 10000);

	ASSERT_EQ(plan.newScoutRefID, 0x00100001u);
	ASSERT_EQ(plan.redirectScoutRefID, 0u);
	ASSERT_EQ(plan.speakerRefID, 0x00100001u);
	ASSERT_EQ(plan.turnRefIDs.size(), (size_t)2);
	ASSERT_EQ(plan.turnRefIDs[0], 0x00100002u);
	ASSERT_EQ(plan.turnRefIDs[1], 0x00100003u);
	return true;
}

TEST(DetectionSound_ActiveScoutBlocksSecondScoutAndRespectsRedirectCooldown)
{
	//Guard 2 (0x100002) is already in [CAUTION] search, Guard 1 (0x100001) is closer and idle
	std::vector<ClassifiedCandidate> cands = {
		{0x00100001, 200.0f * 200.0f, kCandidate_IdleUndetectedHostile, true},
		{0x00100002, 600.0f * 600.0f, kCandidate_ActiveCautionHostile, true},
	};
	std::vector<ActorCooldown> reactCd;
	std::vector<ActorCooldown> investCd;
	StampActorCooldown(reactCd, 0x00100001, 10000, kNpcReactionCooldownMs);
	StampActorCooldown(investCd, 0x00100002, 10000, kInvestigatorRedirectCooldownMs);

	//1.5s later: neither redirect (3.5s) nor bystander turn (12s) nor second scout may fire
	const DistractionPlan early = BuildDistractionPlan(
		cands, true, true, reactCd, investCd, false, true, 10000, 11500);
	ASSERT(early.scoutLocked);
	ASSERT_EQ(early.newScoutRefID, 0u);
	ASSERT_EQ(early.redirectScoutRefID, 0u);
	ASSERT_EQ(early.speakerRefID, 0u);
	ASSERT(early.turnRefIDs.empty());

	//4.0s later: active scout redirects, still no second scout peeled, global 5s bark still cooling
	const DistractionPlan mid = BuildDistractionPlan(
		cands, true, true, reactCd, investCd, false, true, 10000, 14000);
	ASSERT_EQ(mid.newScoutRefID, 0u);
	ASSERT_EQ(mid.redirectScoutRefID, 0x00100002u);
	ASSERT_EQ(mid.speakerRefID, 0u);

	//5.5s later: redirect + bark both allowed
	const DistractionPlan ready = BuildDistractionPlan(
		cands, true, true, reactCd, investCd, false, true, 10000, 15500);
	ASSERT_EQ(ready.newScoutRefID, 0u);
	ASSERT_EQ(ready.redirectScoutRefID, 0x00100002u);
	ASSERT_EQ(ready.speakerRefID, 0x00100002u);
	return true;
}

TEST(DetectionSound_NeutralPerActorAndGlobalBarkCooldowns)
{
	std::vector<ClassifiedCandidate> cands = {
		{0x00200002, 700.0f * 700.0f, kCandidate_Neutral, true},
		{0x00200001, 300.0f * 300.0f, kCandidate_Neutral, true},
	};
	std::vector<ActorCooldown> reactCd;
	std::vector<ActorCooldown> investCd;

	const DistractionPlan first = BuildDistractionPlan(
		cands, true, true, reactCd, investCd, false, false, 0, 50000);
	ASSERT_EQ(first.newScoutRefID, 0u);
	ASSERT_EQ(first.turnRefIDs.size(), (size_t)2);
	ASSERT_EQ(first.speakerRefID, 0x00200001u);

	for (UInt32 refID : first.turnRefIDs)
		StampActorCooldown(reactCd, refID, 50000, kNpcReactionCooldownMs);

	//spam throw 2s later: both neutrals ignore completely
	const DistractionPlan spam = BuildDistractionPlan(
		cands, true, true, reactCd, investCd, false, true, 50000, 52000);
	ASSERT(spam.turnRefIDs.empty());
	ASSERT_EQ(spam.speakerRefID, 0u);

	//after 12.5s: neutrals react again and closest speaks
	const DistractionPlan afterCd = BuildDistractionPlan(
		cands, true, true, reactCd, investCd, false, true, 50000, 62500);
	ASSERT_EQ(afterCd.turnRefIDs.size(), (size_t)2);
	ASSERT_EQ(afterCd.speakerRefID, 0x00200001u);
	return true;
}

TEST(DetectionSound_TickWraparoundSafety)
{
	const UInt32 beforeWrap = 0xFFFFF800u; //2048ms before wrap
	const UInt32 afterWrapShort = beforeWrap + 3000u;
	const UInt32 afterWrapReady = beforeWrap + 12500u;

	ASSERT(!IsCooldownElapsed(afterWrapShort, beforeWrap, kNpcReactionCooldownMs));
	ASSERT(IsCooldownElapsed(afterWrapReady, beforeWrap, kNpcReactionCooldownMs));
	return true;
}
