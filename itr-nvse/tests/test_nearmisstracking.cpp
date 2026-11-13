#include "test.h"
#include "../internal/NearMissTracking.h"

using NearMissTracking::FlightHistory;
using NearMissTracking::Point3;

TEST(NearMissTracksHavokMotionBetweenUpdates)
{
	FlightHistory history;
	const Point3 launch = {0.0f, 0.0f, 100.0f};
	const Point3 moved = {80.0f, 0.0f, 90.0f};
	history.Observe(1, 0.0f, 0.01f, launch, launch);
	const auto start = history.Observe(1, 0.01f, 0.02f, moved, moved);
	ASSERT_NEAR(start.x, launch.x, 0.0001f);
	ASSERT_NEAR(start.z, launch.z, 0.0001f);
	return true;
}

TEST(NearMissTracksMissileMotionDuringUpdate)
{
	FlightHistory history;
	const Point3 launch = {10.0f, 20.0f, 100.0f};
	const Point3 moved = {400.0f, 20.0f, 100.0f};
	const auto first = history.Observe(1, 0.0f, 0.01f, launch, moved);
	ASSERT_NEAR(first.x, launch.x, 0.0001f);
	const Point3 next = {800.0f, 20.0f, 95.0f};
	const auto second = history.Observe(1, 0.01f, 0.02f, moved, next);
	ASSERT_NEAR(second.x, moved.x, 0.0001f);
	return true;
}

TEST(NearMissStationaryProjectileHasNoInventedTravel)
{
	FlightHistory history;
	const Point3 pos = {40.0f, 20.0f, 0.0f};
	const auto first = history.Observe(1, 0.0f, 0.01f, pos, pos);
	const auto second = history.Observe(1, 0.01f, 0.02f, pos, pos);
	ASSERT_NEAR(first.x, pos.x, 0.0001f);
	ASSERT_NEAR(second.x, pos.x, 0.0001f);
	return true;
}

TEST(NearMissDiscardsHistoryAfterLoadOrLifetimeReset)
{
	FlightHistory history;
	const Point3 oldPos = {1000.0f, 0.0f, 0.0f};
	const Point3 newPos = {20.0f, 0.0f, 0.0f};
	history.Observe(1, 0.0f, 0.01f, oldPos, oldPos);
	const auto reset = history.Observe(1, 0.0f, 0.01f, newPos, newPos);
	ASSERT_NEAR(reset.x, newPos.x, 0.0001f);
	history.Clear();
	const auto loaded = history.Observe(1, 0.01f, 0.02f, oldPos, oldPos);
	ASSERT_NEAR(loaded.x, oldPos.x, 0.0001f);
	return true;
}

TEST(NearMissDiscardsHistoryAcrossMissedUpdates)
{
	FlightHistory history;
	const Point3 oldPos = {1000.0f, 0.0f, 0.0f};
	const Point3 newPos = {20.0f, 0.0f, 0.0f};
	history.Observe(1, 0.0f, 0.01f, oldPos, oldPos);
	const auto start = history.Observe(1, 1.0f, 1.01f, newPos, newPos);
	ASSERT_NEAR(start.x, newPos.x, 0.0001f);
	return true;
}

TEST(NearMissHistorySaturationNeverJoinsDifferentProjectiles)
{
	FlightHistory history;
	for (uint32_t id = 1; id < 5000; ++id) {
		const Point3 pos = {static_cast<float>(id), 0.0f, 0.0f};
		const auto start = history.Observe(id, 1.0f, 2.0f, pos, pos);
		ASSERT_NEAR(start.x, pos.x, 0.0001f);
	}
	for (uint32_t id = 1; id < 5000; ++id) {
		const Point3 pos = {static_cast<float>(id), 0.0f, 0.0f};
		const auto start = history.Observe(id, 2.0f, 3.0f, pos, pos);
		ASSERT_NEAR(start.x, pos.x, 0.0001f);
	}
	return true;
}
