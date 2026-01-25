//tests for internal/RefIdSet.h

#include "test.h"

typedef uint32_t UInt32;
#include "../internal/RefIdSet.h"

TEST(RefIdSet_InitiallyEmpty)
{
	RefIdSet<64> set;
	ASSERT_EQ(set.Count(), 0);
	ASSERT(!set.Contains(0x12345));
	return true;
}

TEST(RefIdSet_Add)
{
	RefIdSet<64> set;
	ASSERT(set.Add(0x12345));
	ASSERT(set.Contains(0x12345));
	ASSERT_EQ(set.Count(), 1);
	return true;
}

TEST(RefIdSet_AddMultiple)
{
	RefIdSet<64> set;
	set.Add(0x11111);
	set.Add(0x22222);
	set.Add(0x33333);
	ASSERT(set.Contains(0x11111));
	ASSERT(set.Contains(0x22222));
	ASSERT(set.Contains(0x33333));
	ASSERT(!set.Contains(0x44444));
	ASSERT_EQ(set.Count(), 3);
	return true;
}

TEST(RefIdSet_Remove)
{
	RefIdSet<64> set;
	set.Add(0x12345);
	ASSERT(set.Remove(0x12345));
	ASSERT(!set.Contains(0x12345));
	ASSERT_EQ(set.Count(), 0);
	return true;
}

TEST(RefIdSet_RemoveMiddle)
{
	RefIdSet<64> set;
	set.Add(0x11111);
	set.Add(0x22222);
	set.Add(0x33333);
	set.Remove(0x22222);
	ASSERT(set.Contains(0x11111));
	ASSERT(!set.Contains(0x22222));
	ASSERT(set.Contains(0x33333));
	ASSERT_EQ(set.Count(), 2);
	return true;
}

TEST(RefIdSet_RemoveNonexistent)
{
	RefIdSet<64> set;
	set.Add(0x11111);
	ASSERT(!set.Remove(0x99999));
	ASSERT_EQ(set.Count(), 1);
	return true;
}

TEST(RefIdSet_DuplicateAdd)
{
	RefIdSet<64> set;
	ASSERT(set.Add(0x12345));
	ASSERT(!set.Add(0x12345));
	ASSERT(!set.Add(0x12345));
	ASSERT_EQ(set.Count(), 1);
	return true;
}

TEST(RefIdSet_AddZero)
{
	RefIdSet<64> set;
	ASSERT(!set.Add(0));
	ASSERT_EQ(set.Count(), 0);
	return true;
}

TEST(RefIdSet_MaxCapacity)
{
	RefIdSet<8> set;
	for (int i = 1; i <= 8; i++)
		ASSERT(set.Add(i));
	ASSERT(!set.Add(9));
	ASSERT_EQ(set.Count(), 8);
	return true;
}

TEST(RefIdSet_Clear)
{
	RefIdSet<64> set;
	set.Add(0x11111);
	set.Add(0x22222);
	set.Add(0x33333);
	set.Clear();
	ASSERT_EQ(set.Count(), 0);
	ASSERT(!set.Contains(0x11111));
	return true;
}

TEST(RefIdSet_Capacity)
{
	RefIdSet<32> set;
	ASSERT_EQ(set.Capacity(), 32);
	return true;
}
