#pragma once

#include <cmath>
#include <cstddef>

#include "common/ITypes.h"

namespace ThrowArcLogic {
	struct Vec3 {
		float x;
		float y;
		float z;
	};

	struct Vec4 {
		float x;
		float y;
		float z;
		float w;
	};

	struct DashSegment {
		Vec3 a;
		Vec3 b;
		float alphaA;
		float alphaB;
	};

	constexpr float kDefaultGravityZ = -686.6f;
	constexpr float kSurfaceLift = 1.5f;
	constexpr UInt32 kMaxArcPoints = 52;
	constexpr UInt32 kMaxDashSegments = 160;
	constexpr UInt32 kRingSegments = 16;

	inline float Clamp(float v, float lo, float hi)
	{
		return v < lo ? lo : (v > hi ? hi : v);
	}

	inline Vec3 Add(const Vec3& a, const Vec3& b)
	{
		return {a.x + b.x, a.y + b.y, a.z + b.z};
	}

	inline Vec3 Sub(const Vec3& a, const Vec3& b)
	{
		return {a.x - b.x, a.y - b.y, a.z - b.z};
	}

	inline Vec3 Scale(const Vec3& v, float s)
	{
		return {v.x * s, v.y * s, v.z * s};
	}

	inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t)
	{
		return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
	}

	inline float Dot(const Vec3& a, const Vec3& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	inline Vec3 Cross(const Vec3& a, const Vec3& b)
	{
		return {
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x};
	}

	inline float Length(const Vec3& v)
	{
		return std::sqrt(Dot(v, v));
	}

	inline bool IsFiniteVec3(const Vec3& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

	inline bool Normalize(Vec3& v)
	{
		const float len = Length(v);
		if (!std::isfinite(len) || len <= 1e-5f)
			return false;
		const float inv = 1.0f / len;
		v.x *= inv;
		v.y *= inv;
		v.z *= inv;
		return true;
	}

	inline float ComputeChargeRatio(float elapsedSec, float chargeTime, float tapGrace)
	{
		if (!std::isfinite(elapsedSec) || !std::isfinite(chargeTime) || !std::isfinite(tapGrace))
			return 0.0f;
		const float grace = tapGrace > 0.0f ? tapGrace : 0.0f;
		const float span = chargeTime - grace;
		if (span <= 0.01f)
			return elapsedSec >= grace ? 1.0f : 0.0f;
		return Clamp((elapsedSec - grace) / span, 0.0f, 1.0f);
	}

	inline float ComputeThrowPower(float chargeRatio)
	{
		const float r = Clamp(chargeRatio, 0.0f, 1.0f);
		return r * (0.4f + 0.6f * r);
	}

	inline Vec3 ComputeInitialVelocity(float yawRad, float pitchRad, float minForce, float maxForce,
		float upBoostMin, float upBoostMax, float chargeRatio)
	{
		const float clampedMax = maxForce < minForce ? minForce : maxForce;
		const float power = ComputeThrowPower(chargeRatio);
		const float force = minForce + (clampedMax - minForce) * power;
		const float upBoost = upBoostMin + (upBoostMax - upBoostMin) * power;
		const float horiz = std::cos(pitchRad);
		return {
			horiz * std::sin(yawRad) * force,
			horiz * std::cos(yawRad) * force,
			std::sin(-pitchRad) * force + upBoost};
	}

	inline void StepBallistic(Vec3& pos, Vec3& vel, const Vec3& gravity, float linearDamping, float dt)
	{
		const float damp = Clamp(1.0f - (linearDamping > 0.0f ? linearDamping : 0.0f) * dt, 0.0f, 1.0f);
		vel.x = (vel.x + gravity.x * dt) * damp;
		vel.y = (vel.y + gravity.y * dt) * damp;
		vel.z = (vel.z + gravity.z * dt) * damp;
		pos.x += vel.x * dt;
		pos.y += vel.y * dt;
		pos.z += vel.z * dt;
	}

	inline void UnpackHUDColor(UInt32 rgba, UInt8& r, UInt8& g, UInt8& b)
	{
		if ((rgba & 0xFFFFFF00u) == 0) {
			r = 255;
			g = 182;
			b = 66;
			return;
		}
		r = static_cast<UInt8>((rgba >> 24) & 0xFFu);
		g = static_cast<UInt8>((rgba >> 16) & 0xFFu);
		b = static_cast<UInt8>((rgba >> 8) & 0xFFu);
	}

	inline UInt32 MakeD3DColor(UInt8 a, UInt8 r, UInt8 g, UInt8 b)
	{
		return (static_cast<UInt32>(a) << 24) | (static_cast<UInt32>(r) << 16) | (static_cast<UInt32>(g) << 8) | static_cast<UInt32>(b);
	}

	inline Vec4 Transform(const Vec3& p, const float* m)
	{
		return {
			p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12],
			p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
			p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14],
			p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15]};
	}

	//depth is (a*z+b)/(c*z+d) so derivative follows a*d-b*c
	inline bool ReversedDepth(const float* projection)
	{
		return projection[10] * projection[15] < projection[14] * projection[11];
	}

	inline bool ClipNear(Vec3& a, Vec3& b, float nearZ)
	{
		if (!IsFiniteVec3(a) || !IsFiniteVec3(b) || (a.z < nearZ && b.z < nearZ))
			return false;
		if (a.z < nearZ)
			a = Lerp(a, b, (nearZ - a.z) / (b.z - a.z));
		if (b.z < nearZ)
			b = Lerp(b, a, (nearZ - b.z) / (a.z - b.z));
		return true;
	}

	inline void BuildSurfaceBasis(const Vec3& normal, Vec3& outRight, Vec3& outForward)
	{
		Vec3 n = normal;
		if (!Normalize(n))
			n = {0.0f, 0.0f, 1.0f};
		const Vec3 refAxis = std::fabs(n.z) > 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
		outRight = Cross(refAxis, n);
		if (!Normalize(outRight))
			outRight = {1.0f, 0.0f, 0.0f};
		outForward = Cross(n, outRight);
		Normalize(outForward);
	}

	inline float ComputeArcFade(float distAlongArc, float totalArcLen, bool hasHit)
	{
		const float startFade = Clamp(distAlongArc / 28.0f, 0.0f, 1.0f);
		if (hasHit || totalArcLen <= 1.0f)
			return startFade;
		const float remaining = totalArcLen - distAlongArc;
		const float endFade = Clamp(remaining / 120.0f, 0.0f, 1.0f);
		return startFade * endFade;
	}

	inline UInt32 BuildDashedArc(const Vec3* points, UInt32 pointCount, bool hasHit,
		float dashLen, float gapLen, float phaseOffset,
		DashSegment* outSegments, UInt32 maxSegments)
	{
		if (!points || pointCount < 2 || !outSegments || !maxSegments)
			return 0;
		const float cycle = dashLen + gapLen;
		if (cycle <= 0.1f || dashLen <= 0.05f)
			return 0;

		float cumDist[kMaxArcPoints] = {};
		const UInt32 count = pointCount < kMaxArcPoints ? pointCount : kMaxArcPoints;
		for (UInt32 i = 1; i < count; ++i)
			cumDist[i] = cumDist[i - 1] + Length(Sub(points[i], points[i - 1]));

		const float totalLen = cumDist[count - 1];
		if (totalLen <= 2.0f)
			return 0;

		auto sampleAt = [&](float s) -> Vec3 {
			if (s <= 0.0f)
				return points[0];
			if (s >= totalLen)
				return points[count - 1];
			for (UInt32 i = 1; i < count; ++i) {
				if (s <= cumDist[i]) {
					const float segLen = cumDist[i] - cumDist[i - 1];
					const float t = segLen > 1e-5f ? (s - cumDist[i - 1]) / segLen : 0.0f;
					return Lerp(points[i - 1], points[i], t);
				}
			}
			return points[count - 1];
		};

		float phase = std::fmod(phaseOffset, cycle);
		if (phase < 0.0f)
			phase += cycle;

		UInt32 written = 0;
		for (float start = phase - cycle; start < totalLen && written < maxSegments; start += cycle) {
			const float a = start < 0.0f ? 0.0f : start;
			const float b = (start + dashLen) > totalLen ? totalLen : (start + dashLen);
			if (b - a <= 0.5f)
				continue;
			outSegments[written].a = sampleAt(a);
			outSegments[written].b = sampleAt(b);
			outSegments[written].alphaA = ComputeArcFade(a, totalLen, hasHit);
			outSegments[written].alphaB = ComputeArcFade(b, totalLen, hasHit);
			++written;
		}
		return written;
	}
}
