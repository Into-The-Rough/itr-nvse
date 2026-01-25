//tests for internal/CooldownTracker.h

#include "test.h"
#include "../internal/CooldownTracker.h"

TEST(CooldownTracker_InitiallyEmpty)
{
	CooldownTracker<64> tracker;
	ASSERT_EQ(tracker.Count(), 0);
	ASSERT_EQ(tracker.Capacity(), 64);
	return true;
}

TEST(CooldownTracker_FirstVisitNoTrigger)
{
	CooldownTracker<64> tracker;
	//first visit should return 1 (don't trigger)
	int result = tracker.Check(0x12345, 1000, 60000);
	ASSERT_EQ(result, 1);
	ASSERT_EQ(tracker.Count(), 1);
	return true;
}

TEST(CooldownTracker_StillPresentNoTrigger)
{
	CooldownTracker<64> tracker;
	tracker.Check(0x12345, 1000, 60000);
	//still present, should return 1
	int result = tracker.Check(0x12345, 2000, 60000);
	ASSERT_EQ(result, 1);
	return true;
}

TEST(CooldownTracker_LeftAndReturned)
{
	CooldownTracker<64> tracker;
	uint32_t cooldown = 60000;
	uint32_t leaveThreshold = 3000;

	//first visit at t=1000
	tracker.Check(0x12345, 1000, cooldown);
	//update at t=5000 - been 4 seconds since last seen, marks as left
	tracker.UpdateCooldowns(5000, leaveThreshold);
	//return at t=6000 - should trigger (left and no cooldown)
	int result = tracker.Check(0x12345, 6000, cooldown);
	ASSERT_EQ(result, 0);
	return true;
}

TEST(CooldownTracker_MarkShownCooldown)
{
	CooldownTracker<64> tracker;
	uint32_t cooldown = 60000;
	uint32_t leaveThreshold = 3000;

	//first visit at t=1000
	tracker.Check(0x12345, 1000, cooldown);
	//update at t=5000 - marks as left
	tracker.UpdateCooldowns(5000, leaveThreshold);
	//return at t=6000 - triggers
	tracker.Check(0x12345, 6000, cooldown);
	//mark popup shown
	tracker.MarkShown(0x12345, 6000);
	//leave again
	tracker.UpdateCooldowns(10000, leaveThreshold);
	//return at t=20000 - still in cooldown (60s)
	int result = tracker.Check(0x12345, 20000, cooldown);
	ASSERT_EQ(result, 1);
	return true;
}

TEST(CooldownTracker_CooldownExpired)
{
	CooldownTracker<64> tracker;
	uint32_t cooldown = 60000;
	uint32_t leaveThreshold = 3000;

	tracker.Check(0x12345, 1000, cooldown);
	tracker.UpdateCooldowns(5000, leaveThreshold);
	tracker.Check(0x12345, 6000, cooldown);
	tracker.MarkShown(0x12345, 6000);
	//leave again
	tracker.UpdateCooldowns(10000, leaveThreshold);
	//return at t=70000 - cooldown expired
	int result = tracker.Check(0x12345, 70000, cooldown);
	ASSERT_EQ(result, 0);
	return true;
}

TEST(CooldownTracker_MultipleMarkers)
{
	CooldownTracker<64> tracker;
	uint32_t cooldown = 60000;
	uint32_t leaveThreshold = 3000;

	tracker.Check(0x11111, 1000, cooldown);
	tracker.Check(0x22222, 1000, cooldown);
	tracker.Check(0x33333, 1000, cooldown);
	ASSERT_EQ(tracker.Count(), 3);

	tracker.UpdateCooldowns(5000, leaveThreshold);
	//all should trigger now
	ASSERT_EQ(tracker.Check(0x11111, 6000, cooldown), 0);
	ASSERT_EQ(tracker.Check(0x22222, 6000, cooldown), 0);
	ASSERT_EQ(tracker.Check(0x33333, 6000, cooldown), 0);
	return true;
}

TEST(CooldownTracker_MaxCapacity)
{
	CooldownTracker<4> tracker;
	tracker.Check(1, 1000, 60000);
	tracker.Check(2, 1000, 60000);
	tracker.Check(3, 1000, 60000);
	tracker.Check(4, 1000, 60000);
	ASSERT_EQ(tracker.Count(), 4);
	//should not add more
	tracker.Check(5, 1000, 60000);
	ASSERT_EQ(tracker.Count(), 4);
	return true;
}

TEST(CooldownTracker_Clear)
{
	CooldownTracker<64> tracker;
	tracker.Check(0x11111, 1000, 60000);
	tracker.Check(0x22222, 1000, 60000);
	tracker.Clear();
	ASSERT_EQ(tracker.Count(), 0);
	//first visit again
	ASSERT_EQ(tracker.Check(0x11111, 2000, 60000), 1);
	return true;
}

TEST(CooldownTracker_NotLeftNoTrigger)
{
	CooldownTracker<64> tracker;
	uint32_t cooldown = 60000;
	uint32_t leaveThreshold = 3000;

	tracker.Check(0x12345, 1000, cooldown);
	//only 2 seconds since last seen - not left yet
	tracker.UpdateCooldowns(3000, leaveThreshold);
	//should not trigger
	int result = tracker.Check(0x12345, 4000, cooldown);
	ASSERT_EQ(result, 1);
	return true;
}
