#include "test.h"

#include "../internal/FootContactLogic.h"
#include "../internal/TempEffectLogic.h"

TEST(FootContactSideMapping)
{
	ASSERT_EQ(FootContactLogic::ResolveSide(0), FootContactLogic::kSide_Right);
	ASSERT_EQ(FootContactLogic::ResolveSide(1), FootContactLogic::kSide_Left);
	ASSERT_EQ(FootContactLogic::ResolveSide(2), FootContactLogic::kSide_Right);
	ASSERT_EQ(FootContactLogic::ResolveSide(3), FootContactLogic::kSide_Left);
	ASSERT_EQ(FootContactLogic::ResolveSide(4), FootContactLogic::kSide_Unknown);
	ASSERT_EQ(FootContactLogic::ResolveSide(-1), FootContactLogic::kSide_Unknown);
	return true;
}

TEST(FootContactInterpolationClamps)
{
	const FootContactLogic::Point3 from = { 10.0f, 20.0f, 30.0f };
	const FootContactLogic::Point3 to = { 20.0f, 40.0f, 10.0f };

	auto middle = FootContactLogic::Interpolate(from, to, 0.25f);
	ASSERT_NEAR(middle.x, 12.5f, 0.0001f);
	ASSERT_NEAR(middle.y, 25.0f, 0.0001f);
	ASSERT_NEAR(middle.z, 25.0f, 0.0001f);

	auto below = FootContactLogic::Interpolate(from, to, -1.0f);
	ASSERT_NEAR(below.z, 30.0f, 0.0001f);
	auto above = FootContactLogic::Interpolate(from, to, 2.0f);
	ASSERT_NEAR(above.z, 10.0f, 0.0001f);
	return true;
}

TEST(TempEffectValidation)
{
	const TempEffectLogic::Point3 position = { 1.0f, 2.0f, 3.0f };
	const TempEffectLogic::Point3 normal = { 0.0f, 0.0f, 1.0f };
	ASSERT(TempEffectLogic::IsValid(0.2f, position, normal, 1.0f, 180.0f));
	ASSERT(!TempEffectLogic::IsValid(0.0f, position, normal, 1.0f, 0.0f));
	ASSERT(!TempEffectLogic::IsValid(0.2f, position, { 0.0f, 0.0f, 0.0f }, 1.0f, 0.0f));
	ASSERT(!TempEffectLogic::IsValid(0.2f, position, normal, 0.0f, 0.0f));
	return true;
}

TEST(TempEffectRotationUsesSurfaceNormal)
{
	TempEffectLogic::Matrix3 rotation;
	ASSERT(TempEffectLogic::MakeSurfaceRotation({ 0.0f, 1.0f, 1.0f }, 90.0f, rotation));
	const float invSqrt2 = 0.70710678f;
	ASSERT_NEAR(rotation.data[0][2], 0.0f, 0.0001f);
	ASSERT_NEAR(rotation.data[1][2], invSqrt2, 0.0001f);
	ASSERT_NEAR(rotation.data[2][2], invSqrt2, 0.0001f);
	return true;
}

TEST(TempEffectRotationAcceptsShallowSlopes)
{
	TempEffectLogic::Matrix3 rotation;
	ASSERT(TempEffectLogic::MakeSurfaceRotation(
		{ -0.0622573f, 0.0622573f, 0.996117f }, 0.0f, rotation));
	ASSERT(TempEffectLogic::MakeSurfaceRotation(
		{ 0.0f, -0.0623782f, 0.998053f }, 180.0f, rotation));
	ASSERT_NEAR(rotation.data[0][2], 0.0f, 0.0001f);
	ASSERT_NEAR(rotation.data[1][2], -0.0623782f, 0.0001f);
	ASSERT_NEAR(rotation.data[2][2], 0.998053f, 0.0001f);
	return true;
}
