#include "test.h"

#include "../internal/PlayerGrabLogic.h"

TEST(PlayerGrab_DefaultAndClampedDistances)
{
	ASSERT_NEAR(PlayerGrabLogic::ClampRequestedDistance(0.0f), PlayerGrabLogic::kDefaultHoldDistance, 0.0001f);
	ASSERT_NEAR(PlayerGrabLogic::ClampRequestedDistance(-12.0f), PlayerGrabLogic::kDefaultHoldDistance, 0.0001f);
	ASSERT_NEAR(PlayerGrabLogic::ClampRequestedDistance(10.0f), PlayerGrabLogic::kMinHoldDistance, 0.0001f);
	ASSERT_NEAR(PlayerGrabLogic::ClampRequestedDistance(80.0f), 80.0f, 0.0001f);
	ASSERT_NEAR(PlayerGrabLogic::ClampRequestedDistance(999.0f), PlayerGrabLogic::kMaxHoldDistance, 0.0001f);
	return true;
}

TEST(PlayerGrab_WallClampPullsBackBeforeSurface)
{
	const float desired = 64.0f;
	const float rayLen = desired + PlayerGrabLogic::kWallClearance;
	//no wall hit keeps desired distance
	ASSERT_NEAR(PlayerGrabLogic::ClampHoldDistanceForWall(desired, rayLen, 1.0f), desired, 0.0001f);

	//wall at 52u pulls hold distance to 42u (10u clearance, above 36u min)
	const float frac52 = 52.0f / rayLen;
	ASSERT_NEAR(PlayerGrabLogic::ClampHoldDistanceForWall(desired, rayLen, frac52), 42.0f, 0.001f);

	//wall flush at 28u falls back inside wall distance instead of spawning behind the wall
	const float frac28 = 28.0f / rayLen;
	const float clampedClose = PlayerGrabLogic::ClampHoldDistanceForWall(desired, rayLen, frac28);
	ASSERT(clampedClose < 28.0f);
	ASSERT(clampedClose >= PlayerGrabLogic::kMinCloseWallDistance);
	return true;
}

TEST(PlayerGrab_RepositionContractMatchesUpdateMouseSpringStretchLimit)
{
	const PlayerGrabLogic::Point3 eyePos = {0.0f, 0.0f, 120.0f};
	const PlayerGrabLogic::Point3 eyeDir = {0.0f, 1.0f, 0.0f};

	//item sitting on a table ahead within reach and <64u stretch grabs in-place
	const PlayerGrabLogic::Point3 tableItem = {8.0f, 95.0f, 100.0f};
	ASSERT(!PlayerGrabLogic::ShouldRepositionForGrab(eyePos, eyeDir, tableItem, false));

	//explicit distance forces reposition even for nearby items
	ASSERT(PlayerGrabLogic::ShouldRepositionForGrab(eyePos, eyeDir, tableItem, true));

	//PlaceAtMe spawn at player's feet (proj == 0, stretch ~99u > 96u UpdateMouseSpring auto-drop) repositions
	const PlayerGrabLogic::Point3 feetSpawn = {0.0f, 0.0f, 50.0f};
	ASSERT(PlayerGrabLogic::ShouldRepositionForGrab(eyePos, eyeDir, feetSpawn, false));

	//item behind camera repositions
	const PlayerGrabLogic::Point3 behind = {0.0f, -60.0f, 120.0f};
	ASSERT(PlayerGrabLogic::ShouldRepositionForGrab(eyePos, eyeDir, behind, false));

	//item far beyond reach repositions
	const PlayerGrabLogic::Point3 farItem = {0.0f, 400.0f, 120.0f};
	ASSERT(PlayerGrabLogic::ShouldRepositionForGrab(eyePos, eyeDir, farItem, false));
	return true;
}
