//tests for internal/GestureMath.h

#include "test.h"
#include "../internal/GestureMath.h"

namespace
{
	void SetIdentity(float* m)
	{
		for (int i = 0; i < 9; i++)
			m[i] = 0.0f;
		m[0] = 1.0f;
		m[4] = 1.0f;
		m[8] = 1.0f;
	}

	bool MatsNear(const float* a, const float* b, float eps)
	{
		for (int i = 0; i < 9; i++)
		{
			if (std::fabs(a[i] - b[i]) > eps)
				return false;
		}
		return true;
	}
}

TEST(GestureMath_SmoothstepBounds)
{
	ASSERT_NEAR(GestureMath::Smoothstep(0.0f), 0.0f, 0.0001f);
	ASSERT_NEAR(GestureMath::Smoothstep(1.0f), 1.0f, 0.0001f);
	return true;
}

TEST(GestureMath_AngleZeroAtStartAndEnd)
{
	ASSERT_NEAR(GestureMath::ComputeAngleRadians(GestureMath::kGesture_Nod, 0, 3000, 0.5f, 0.4f), 0.0f, 0.0001f);
	ASSERT_NEAR(GestureMath::ComputeAngleRadians(GestureMath::kGesture_Nod, 3000, 3000, 0.5f, 0.4f), 0.0f, 0.0001f);
	return true;
}

TEST(GestureMath_NodUsesPitchAxis)
{
	float pose[9];
	SetIdentity(pose);
	GestureMath::ComposePoseFromBase(pose, GestureMath::kGesture_Nod, 0.5f, pose);

	ASSERT_NEAR(pose[0], 1.0f, 0.0001f);
	ASSERT_NEAR(std::fabs(pose[5]), std::sin(0.5f), 0.0001f);
	ASSERT_NEAR(std::fabs(pose[7]), std::sin(0.5f), 0.0001f);
	ASSERT_NEAR(pose[1], 0.0f, 0.0001f);
	ASSERT_NEAR(pose[3], 0.0f, 0.0001f);
	return true;
}

TEST(GestureMath_ShakeUsesYawAxis)
{
	float pose[9];
	SetIdentity(pose);
	GestureMath::ComposePoseFromBase(pose, GestureMath::kGesture_Shake, 0.5f, pose);

	ASSERT_NEAR(pose[8], 1.0f, 0.0001f);
	ASSERT_NEAR(std::fabs(pose[1]), std::sin(0.5f), 0.0001f);
	ASSERT_NEAR(std::fabs(pose[3]), std::sin(0.5f), 0.0001f);
	ASSERT_NEAR(pose[2], 0.0f, 0.0001f);
	ASSERT_NEAR(pose[6], 0.0f, 0.0001f);
	return true;
}

TEST(GestureMath_TiltUsesRollAxis)
{
	float pose[9];
	SetIdentity(pose);
	GestureMath::ComposePoseFromBase(pose, GestureMath::kGesture_Tilt, 0.5f, pose);

	ASSERT_NEAR(pose[4], 1.0f, 0.0001f);
	ASSERT_NEAR(std::fabs(pose[2]), std::sin(0.5f), 0.0001f);
	ASSERT_NEAR(std::fabs(pose[6]), std::sin(0.5f), 0.0001f);
	ASSERT_NEAR(pose[1], 0.0f, 0.0001f);
	ASSERT_NEAR(pose[7], 0.0f, 0.0001f);
	return true;
}

TEST(GestureMath_ComposeFromBaseRestoresBaselineAtZeroAngle)
{
	const float base[9] = {
		0.1f, 0.2f, 0.3f,
		0.4f, 0.5f, 0.6f,
		0.7f, 0.8f, 0.9f,
	};
	float pose[9];
	GestureMath::ComposePoseFromBase(base, GestureMath::kGesture_Shake, 0.0f, pose);
	ASSERT(MatsNear(base, pose, 0.0001f));
	return true;
}

TEST(GestureMath_ComposeFromBaseIsDeterministic)
{
	float base[9];
	SetIdentity(base);

	float direct[9];
	float accumulated[9];
	GestureMath::ComposePoseFromBase(base, GestureMath::kGesture_Nod, 0.35f, direct);
	GestureMath::CopyMat3(accumulated, direct);
	GestureMath::ApplyLocalPitch(accumulated, 0.35f);

	float recomposed[9];
	GestureMath::ComposePoseFromBase(base, GestureMath::kGesture_Nod, 0.35f, recomposed);

	ASSERT(MatsNear(direct, recomposed, 0.0001f));

	bool differs = false;
	for (int i = 0; i < 9; i++)
	{
		if (std::fabs(accumulated[i] - direct[i]) > 0.0001f)
		{
			differs = true;
			break;
		}
	}
	ASSERT(differs);
	return true;
}

TEST(GestureMath_TiltHoldsFullAmplitudeMidGesture)
{
	float angle = GestureMath::ComputeAngleRadians(GestureMath::kGesture_Tilt, 1500, 3000, 0.25f, 0.4f);
	ASSERT_NEAR(angle, 0.25f, 0.0001f);
	return true;
}

TEST(GestureMath_NodCycleTimeIsClamped)
{
	float angle = GestureMath::ComputeAngleRadians(GestureMath::kGesture_Nod, 225, 1000, 0.6f, 0.001f);
	ASSERT_NEAR(angle, 0.6f, 0.0001f);
	return true;
}
