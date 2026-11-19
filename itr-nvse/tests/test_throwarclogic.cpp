#include "test.h"

#include "../internal/ThrowArcLogic.h"

TEST(ThrowArc_ChargeRatioAndPowerCurve)
{
	//within tap grace returns 0
	ASSERT_NEAR(ThrowArcLogic::ComputeChargeRatio(0.05f, 1.2f, 0.1f), 0.0f, 0.0001f);
	ASSERT_NEAR(ThrowArcLogic::ComputeChargeRatio(0.10f, 1.2f, 0.1f), 0.0f, 0.0001f);

	//midpoint and full charge match OnThrowKeyUp formula
	ASSERT_NEAR(ThrowArcLogic::ComputeChargeRatio(0.65f, 1.2f, 0.1f), 0.5f, 0.0001f);
	ASSERT_NEAR(ThrowArcLogic::ComputeChargeRatio(1.20f, 1.2f, 0.1f), 1.0f, 0.0001f);
	ASSERT_NEAR(ThrowArcLogic::ComputeChargeRatio(2.50f, 1.2f, 0.1f), 1.0f, 0.0001f);

	//ease-in curve r * (0.4 + 0.6 * r)
	ASSERT_NEAR(ThrowArcLogic::ComputeThrowPower(0.0f), 0.0f, 0.0001f);
	ASSERT_NEAR(ThrowArcLogic::ComputeThrowPower(0.5f), 0.35f, 0.0001f);
	ASSERT_NEAR(ThrowArcLogic::ComputeThrowPower(1.0f), 1.0f, 0.0001f);
	return true;
}

TEST(ThrowArc_InitialVelocityMatchesLaunchContract)
{
	//facing +Y (yaw=0, pitch=0) at tap (ratio=0)
	const ThrowArcLogic::Vec3 tap = ThrowArcLogic::ComputeInitialVelocity(
		0.0f, 0.0f, 200.0f, 1400.0f, 65.0f, 230.0f, 0.0f);
	ASSERT_NEAR(tap.x, 0.0f, 0.001f);
	ASSERT_NEAR(tap.y, 200.0f, 0.001f);
	ASSERT_NEAR(tap.z, 65.0f, 0.001f);

	//facing +X (yaw=pi/2), aiming up 30 deg (pitch=-pi/6) at full charge
	const float pi = 3.14159265f;
	const ThrowArcLogic::Vec3 full = ThrowArcLogic::ComputeInitialVelocity(
		pi * 0.5f, -pi / 6.0f, 200.0f, 1400.0f, 65.0f, 230.0f, 1.0f);
	ASSERT_NEAR(full.x, 1400.0f * std::cos(-pi / 6.0f), 0.05f);
	ASSERT_NEAR(full.y, 0.0f, 0.05f);
	ASSERT_NEAR(full.z, 1400.0f * 0.5f + 230.0f, 0.05f);
	return true;
}

TEST(ThrowArc_BallisticStepAndDamping)
{
	ThrowArcLogic::Vec3 pos = {0.0f, 0.0f, 100.0f};
	ThrowArcLogic::Vec3 vel = {0.0f, 500.0f, 0.0f};
	const ThrowArcLogic::Vec3 grav = {0.0f, 0.0f, -686.6f};
	ThrowArcLogic::StepBallistic(pos, vel, grav, 0.05f, 0.04f);
	ASSERT(vel.y < 500.0f && vel.y > 498.0f);
	ASSERT(vel.z < -27.0f && vel.z > -28.0f);
	ASSERT(pos.y > 19.5f && pos.y < 20.1f);
	ASSERT(pos.z < 100.0f);
	return true;
}

TEST(ThrowArc_HUDColorUnpackAndFallback)
{
	UInt8 r = 0, g = 0, b = 0;
	//SystemColorManager packs 0xRRGGBBAA
	ThrowArcLogic::UnpackHUDColor(0x1AFF80FFu, r, g, b);
	ASSERT_EQ(r, 0x1A);
	ASSERT_EQ(g, 0xFF);
	ASSERT_EQ(b, 0x80);
	ASSERT_EQ(ThrowArcLogic::MakeD3DColor(0xC0, r, g, b), 0xC01AFF80u);

	//zero color falls back to New Vegas amber
	ThrowArcLogic::UnpackHUDColor(0, r, g, b);
	ASSERT_EQ(r, 255);
	ASSERT_EQ(g, 182);
	ASSERT_EQ(b, 66);
	return true;
}

TEST(ThrowArc_ProjectionDepthAndClipNear)
{
	//normal depth matrix (zf/(zf-zn) > 0, -zn*zf/(zf-zn) < 0, m[11]=1, m[15]=0)
	float normalProj[16] = {};
	normalProj[10] = 1.001f;
	normalProj[11] = 1.0f;
	normalProj[14] = -5.0f;
	ASSERT(!ThrowArcLogic::ReversedDepth(normalProj));

	//reversed depth matrix (m[10] < 0, m[14] > 0)
	float revProj[16] = {};
	revProj[10] = -0.001f;
	revProj[11] = 1.0f;
	revProj[14] = 5.0f;
	ASSERT(ThrowArcLogic::ReversedDepth(revProj));

	ThrowArcLogic::Vec3 a = {0.0f, 0.0f, -10.0f};
	ThrowArcLogic::Vec3 b = {0.0f, 0.0f, 10.0f};
	ASSERT(ThrowArcLogic::ClipNear(a, b, 2.0f));
	ASSERT_NEAR(a.z, 2.0f, 0.0001f);
	ASSERT_NEAR(b.z, 10.0f, 0.0001f);

	ThrowArcLogic::Vec3 c = {0.0f, 0.0f, -5.0f};
	ThrowArcLogic::Vec3 d = {0.0f, 0.0f, 1.0f};
	ASSERT(!ThrowArcLogic::ClipNear(c, d, 2.0f));
	return true;
}

TEST(ThrowArc_DashedArcAndSurfaceBasis)
{
	ThrowArcLogic::Vec3 pts[3] = {
		{0.0f, 0.0f, 0.0f},
		{0.0f, 50.0f, 0.0f},
		{0.0f, 100.0f, 0.0f}};
	ThrowArcLogic::DashSegment segs[32] = {};
	const UInt32 count = ThrowArcLogic::BuildDashedArc(pts, 3, true, 12.0f, 8.0f, 4.0f, segs, 32);
	ASSERT(count >= 4 && count <= 7);
	ASSERT(segs[0].alphaA < segs[count - 1].alphaB);

	ThrowArcLogic::Vec3 right = {}, fwd = {};
	const ThrowArcLogic::Vec3 n = {0.0f, 0.0f, 1.0f};
	ThrowArcLogic::BuildSurfaceBasis(n, right, fwd);
	ASSERT_NEAR(ThrowArcLogic::Dot(n, right), 0.0f, 0.001f);
	ASSERT_NEAR(ThrowArcLogic::Dot(n, fwd), 0.0f, 0.001f);
	ASSERT_NEAR(ThrowArcLogic::Dot(right, fwd), 0.0f, 0.001f);
	ASSERT_NEAR(ThrowArcLogic::Length(right), 1.0f, 0.001f);
	ASSERT_NEAR(ThrowArcLogic::Length(fwd), 1.0f, 0.001f);
	return true;
}
