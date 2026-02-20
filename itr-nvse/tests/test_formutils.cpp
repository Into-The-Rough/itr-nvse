//tests for internal/FormUtils.h

#include "test.h"
#include "../internal/FormUtils.h"

using namespace FormUtils;

//mock ref for distance tests
struct MockRef {
	float posX, posY, posZ;
};

TEST(FormUtils_IsInventoryItem_Weapon)
{
	ASSERT(IsInventoryItemType(0x28));
	return true;
}

TEST(FormUtils_IsInventoryItem_Armor)
{
	ASSERT(IsInventoryItemType(0x18));
	return true;
}

TEST(FormUtils_IsInventoryItem_Ammo)
{
	ASSERT(IsInventoryItemType(0x29));
	return true;
}

TEST(FormUtils_IsInventoryItem_Key)
{
	ASSERT(IsInventoryItemType(0x2E));
	return true;
}

TEST(FormUtils_IsInventoryItem_Book)
{
	ASSERT(IsInventoryItemType(0x19));
	return true;
}

TEST(FormUtils_IsInventoryItem_AlchemyItem)
{
	ASSERT(IsInventoryItemType(0x2F));
	return true;
}

TEST(FormUtils_IsInventoryItem_Note)
{
	ASSERT(IsInventoryItemType(0x31));
	return true;
}

TEST(FormUtils_NotInventoryItem_Creature)
{
	ASSERT(!IsInventoryItemType(0x30));
	return true;
}

TEST(FormUtils_NotInventoryItem_NPC)
{
	ASSERT(!IsInventoryItemType(0x36));
	return true;
}

TEST(FormUtils_NotInventoryItem_Zero)
{
	ASSERT(!IsInventoryItemType(0));
	return true;
}

TEST(FormUtils_Distance_SamePosition)
{
	MockRef a = {100, 200, 300};
	MockRef b = {100, 200, 300};
	ASSERT_NEAR(CalcDistanceSquared(&a, &b), 0.0f, 0.001f);
	return true;
}

TEST(FormUtils_Distance_345Triangle)
{
	MockRef a = {0, 0, 0};
	MockRef b = {3, 4, 0};
	//3-4-5 triangle, distance = 5, squared = 25
	ASSERT_NEAR(CalcDistanceSquared(&a, &b), 25.0f, 0.001f);
	return true;
}

TEST(FormUtils_Distance_3D)
{
	MockRef a = {0, 0, 0};
	MockRef b = {1, 2, 2};
	//sqrt(1 + 4 + 4) = 3, squared = 9
	ASSERT_NEAR(CalcDistanceSquared(&a, &b), 9.0f, 0.001f);
	return true;
}

TEST(FormUtils_Distance_Negative)
{
	MockRef a = {10, 10, 10};
	MockRef b = {7, 6, 10};
	//dx=-3, dy=-4, dz=0 -> 9+16+0 = 25
	ASSERT_NEAR(CalcDistanceSquared(&a, &b), 25.0f, 0.001f);
	return true;
}

TEST(FormUtils_Distance_LargeValues)
{
	MockRef a = {10000, 20000, 5000};
	MockRef b = {10003, 20004, 5000};
	ASSERT_NEAR(CalcDistanceSquared(&a, &b), 25.0f, 0.001f);
	return true;
}
