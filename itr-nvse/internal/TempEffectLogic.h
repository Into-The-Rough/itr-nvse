#pragma once

#include <cstddef>
#include <cmath>

namespace TempEffectLogic
{
	struct Point3
	{
		float x;
		float y;
		float z;
	};

	struct Matrix3
	{
		float data[3][3];
	};

	static_assert(sizeof(Point3) == 0x0C);
	static_assert(sizeof(Matrix3) == 0x24);

	inline bool IsFinite(const Point3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	inline bool Normalize(Point3& value)
	{
		const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
		if (!std::isfinite(lengthSquared) || lengthSquared < 0.0625f || lengthSquared > 4.0f)
			return false;

		const float inverseLength = 1.0f / std::sqrt(lengthSquared);
		value.x *= inverseLength;
		value.y *= inverseLength;
		value.z *= inverseLength;
		return true;
	}

	inline Point3 Cross(const Point3& left, const Point3& right)
	{
		return {
			left.y * right.z - left.z * right.y,
			left.z * right.x - left.x * right.z,
			left.x * right.y - left.y * right.x,
		};
	}

	inline bool IsValid(float duration, const Point3& position, const Point3& normal, float scale, float yawDegrees)
	{
		if (!std::isfinite(duration) || duration <= 0.0f || duration > 60.0f)
			return false;
		if (!std::isfinite(scale) || scale <= 0.0f || scale > 100.0f)
			return false;
		if (!std::isfinite(yawDegrees) || !IsFinite(position))
			return false;

		Point3 copy = normal;
		return Normalize(copy);
	}

	inline bool MakeSurfaceRotation(Point3 normal, float yawDegrees, Matrix3& rotation)
	{
		if (!Normalize(normal) || !std::isfinite(yawDegrees))
			return false;

		Point3 tangent = std::fabs(normal.z) < 0.70710678f
			? Cross({ 0.0f, 0.0f, 1.0f }, normal)
			: Cross({ 0.0f, 1.0f, 0.0f }, normal);
		if (!Normalize(tangent))
			return false;

		const Point3 bitangent = Cross(normal, tangent);
		const float yaw = std::fmod(yawDegrees, 360.0f) * 0.017453292519943295f;
		const float sine = std::sin(yaw);
		const float cosine = std::cos(yaw);
		const Point3 axisX = {
			tangent.x * cosine + bitangent.x * sine,
			tangent.y * cosine + bitangent.y * sine,
			tangent.z * cosine + bitangent.z * sine,
		};
		const Point3 axisY = Cross(normal, axisX);

		rotation = {};
		rotation.data[0][0] = axisX.x;
		rotation.data[1][0] = axisX.y;
		rotation.data[2][0] = axisX.z;
		rotation.data[0][1] = axisY.x;
		rotation.data[1][1] = axisY.y;
		rotation.data[2][1] = axisY.z;
		rotation.data[0][2] = normal.x;
		rotation.data[1][2] = normal.y;
		rotation.data[2][2] = normal.z;
		return true;
	}
}
