//tests for internal/FormUtils.h

#include "test.h"
#define FORMUTILS_TEST_MIRROR_ENGINE
#include "../internal/FormUtils.h"

using namespace FormUtils;

//mock ref for distance tests
struct MockRef {
	float posX, posY, posZ;
};

//ContainerCanHoldType (0x481F30) output for form types 0x00-0xFF, dumped from the
//runtime on 2026-07-02 - the 1.4.0.525 binary is frozen so this table is permanent
static const unsigned char kEngineInventoryTable[256] = {
	/*0x00*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/*0x10*/ 0,0,0,0,0,0,0,0,1,1,1,0,0,1,1,1,
	/*0x20*/ 0,0,0,0,0,0,0,0,1,1,0,0,0,0,1,1,
	/*0x30*/ 0,1,1,0,1,0,0,0,0,0,0,0,0,0,0,0,
	/*0x40*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/*0x50*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/*0x60*/ 0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,
	/*0x70*/ 0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,
	/*0x80*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/*0x90*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/*0xA0*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/*0xB0*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/*0xC0*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/*0xD0*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/*0xE0*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/*0xF0*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

TEST(FormUtils_InventoryTable_MatchesEngineDump)
{
	for (int type = 0; type < 256; ++type)
		ASSERT(IsInventoryItemType((uint8_t)type) == (kEngineInventoryTable[type] != 0));
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
