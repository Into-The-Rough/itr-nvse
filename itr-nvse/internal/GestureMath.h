#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace GestureMath {

enum GestureType : uint8_t {
	kGesture_None = 0,
	kGesture_Nod = 1,
	kGesture_Shake = 2,
	kGesture_Tilt = 3,
};

inline constexpr float kPi = 3.14159265f;

inline void CopyMat3(float* dst, const float* src)
{
	std::memcpy(dst, src, sizeof(float) * 9);
}

inline float Smoothstep(float t)
{
	return t * t * (3.0f - 2.0f * t);
}

inline void ApplyLocalPitch(float* m, float rad)
{
	const float c = std::cos(rad);
	const float s = std::sin(rad);
	for (int i = 0; i < 3; i++)
	{
		const float y = m[i * 3 + 1];
		const float z = m[i * 3 + 2];
		m[i * 3 + 1] = y * c + z * s;
		m[i * 3 + 2] = -y * s + z * c;
	}
}

inline void ApplyLocalRoll(float* m, float rad)
{
	const float c = std::cos(rad);
	const float s = std::sin(rad);
	for (int i = 0; i < 3; i++)
	{
		const float x = m[i * 3 + 0];
		const float z = m[i * 3 + 2];
		m[i * 3 + 0] = x * c - z * s;
		m[i * 3 + 2] = x * s + z * c;
	}
}

inline void ApplyLocalYaw(float* m, float rad)
{
	const float c = std::cos(rad);
	const float s = std::sin(rad);
	for (int i = 0; i < 3; i++)
	{
		const float x = m[i * 3 + 0];
		const float y = m[i * 3 + 1];
		m[i * 3 + 0] = x * c + y * s;
		m[i * 3 + 1] = -x * s + y * c;
	}
}

inline float ComputeEnvelope(uint8_t type, uint32_t elapsedMs, uint32_t durationMs, float cycleTimeSec)
{
	if (!durationMs || elapsedMs >= durationMs)
		return 0.0f;

	float blendMs = (type == kGesture_Tilt) ? cycleTimeSec * 1000.0f : 200.0f;
	const float duration = static_cast<float>(durationMs);
	if (blendMs > duration * 0.4f)
		blendMs = duration * 0.4f;
	if (blendMs <= 0.0f)
		return 1.0f;

	const float elapsed = static_cast<float>(elapsedMs);
	float env = 1.0f;
	if (elapsed < blendMs)
		env = Smoothstep(elapsed / blendMs);

	const float remaining = duration - elapsed;
	if (remaining < blendMs)
		env = Smoothstep(remaining / blendMs);

	return env;
}

inline float ComputeAngleRadians(uint8_t type, uint32_t elapsedMs, uint32_t durationMs, float amplitudeRad, float cycleTimeSec)
{
	const float env = ComputeEnvelope(type, elapsedMs, durationMs, cycleTimeSec);
	if (env <= 0.0f || amplitudeRad == 0.0f)
		return 0.0f;

	if (type == kGesture_Tilt)
		return amplitudeRad * env;

	float cycleMs = cycleTimeSec * 1000.0f;
	if (cycleMs < 50.0f)
		cycleMs = 50.0f;

	float phase = std::fmod(static_cast<float>(elapsedMs), cycleMs) / cycleMs;
	if (phase < 0.0f)
		phase += 1.0f;

	if (type == kGesture_Shake)
		return amplitudeRad * env * std::sin(2.0f * kPi * phase);
	if (type == kGesture_Nod)
		return amplitudeRad * env * std::sin(kPi * phase);

	return 0.0f;
}

inline void ComposePoseFromBase(const float* base, uint8_t type, float angleRad, float* out)
{
	CopyMat3(out, base);

	// FNV's head-bone local basis makes semantic nod/shake line up opposite to the naive pitch/yaw labels here.
	if (type == kGesture_Nod)
		ApplyLocalYaw(out, angleRad);
	else if (type == kGesture_Shake)
		ApplyLocalPitch(out, angleRad);
	else if (type == kGesture_Tilt)
		ApplyLocalRoll(out, angleRad);
}

}  // namespace GestureMath
