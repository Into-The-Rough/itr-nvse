#pragma once

#include <cmath>

namespace FootContactLogic
{
	enum Side
	{
		kSide_Unknown = -1,
		kSide_Left = 0,
		kSide_Right = 1,
	};

	struct Point3
	{
		float x;
		float y;
		float z;
	};

	inline int ResolveSide(int soundID)
	{
		if (soundID > 1)
			soundID -= 2;

		if (soundID == 0)
			return kSide_Right;
		if (soundID == 1)
			return kSide_Left;
		return kSide_Unknown;
	}

	inline Point3 Interpolate(const Point3& from, const Point3& to, float fraction)
	{
		if (fraction < 0.0f)
			fraction = 0.0f;
		else if (fraction > 1.0f)
			fraction = 1.0f;

		return {
			from.x + (to.x - from.x) * fraction,
			from.y + (to.y - from.y) * fraction,
			from.z + (to.z - from.z) * fraction,
		};
	}

	inline bool Normalize(Point3& value)
	{
		const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
		if (!std::isfinite(lengthSquared) || lengthSquared < 0.000001f)
			return false;

		const float inverseLength = 1.0f / std::sqrt(lengthSquared);
		value.x *= inverseLength;
		value.y *= inverseLength;
		value.z *= inverseLength;
		return true;
	}

	inline bool IsFinite(const Point3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}
}
