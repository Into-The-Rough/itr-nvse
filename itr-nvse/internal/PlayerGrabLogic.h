#pragma once

#include <cmath>

namespace PlayerGrabLogic {
	struct Point3 {
		float x;
		float y;
		float z;
	};

	constexpr float kDefaultHoldDistance = 64.0f;
	constexpr float kMinHoldDistance = 36.0f;
	constexpr float kMaxHoldDistance = 250.0f;
	constexpr float kWallClearance = 10.0f;
	constexpr float kMinCloseWallDistance = 16.0f;
	constexpr float kMaxInPlaceGrabDistance = 220.0f;
	//0x960520 drops the spring above 96u stretch from eyePos + eyeDir * grabDistance
	constexpr float kMaxInPlaceStretch = 64.0f;

	inline Point3 PointAlongRay(const Point3& origin, const Point3& dir, float distance)
	{
		return {
			origin.x + dir.x * distance,
			origin.y + dir.y * distance,
			origin.z + dir.z * distance,
		};
	}

	inline float ClampRequestedDistance(float requestedDistance)
	{
		float dist = requestedDistance > 0.0f ? requestedDistance : kDefaultHoldDistance;
		if (dist < kMinHoldDistance)
			dist = kMinHoldDistance;
		else if (dist > kMaxHoldDistance)
			dist = kMaxHoldDistance;
		return dist;
	}

	inline float ClampHoldDistanceForWall(float desiredDistance, float rayLength, float hitFraction)
	{
		if (!std::isfinite(hitFraction) || hitFraction < 0.0f || hitFraction >= 1.0f || rayLength <= 0.0f)
			return desiredDistance;

		const float hitDist = rayLength * hitFraction;
		float safeDist = hitDist - kWallClearance;
		if (safeDist >= desiredDistance)
			return desiredDistance;
		if (safeDist >= kMinHoldDistance)
			return safeDist;

		const float fallback = hitDist * 0.75f;
		return fallback > kMinCloseWallDistance ? fallback : kMinCloseWallDistance;
	}

	inline bool ShouldRepositionForGrab(const Point3& eyePos, const Point3& eyeDir,
		const Point3& refPos, bool forceReposition)
	{
		if (forceReposition)
			return true;

		const float dx = refPos.x - eyePos.x;
		const float dy = refPos.y - eyePos.y;
		const float dz = refPos.z - eyePos.z;
		const float distSq = dx * dx + dy * dy + dz * dz;
		if (!std::isfinite(distSq) || distSq > kMaxInPlaceGrabDistance * kMaxInPlaceGrabDistance)
			return true;

		const float proj = dx * eyeDir.x + dy * eyeDir.y + dz * eyeDir.z;
		if (proj < kMinHoldDistance)
			return true;

		const float dist = std::sqrt(distSq);
		const float targetX = eyePos.x + eyeDir.x * dist;
		const float targetY = eyePos.y + eyeDir.y * dist;
		const float targetZ = eyePos.z + eyeDir.z * dist;
		const float sx = refPos.x - targetX;
		const float sy = refPos.y - targetY;
		const float sz = refPos.z - targetZ;
		return (sx * sx + sy * sy + sz * sz) > (kMaxInPlaceStretch * kMaxInPlaceStretch);
	}
}
