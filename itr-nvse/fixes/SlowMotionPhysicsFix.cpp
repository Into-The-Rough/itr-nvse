//clamps physics timestep to prevent ragdoll energy gain during extreme slowmo
//NOT hot-reloadable - requires game restart

#include "SlowMotionPhysicsFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/CallTemplates.h"
#include "internal/SafeWrite.h"

#include "internal/globals.h"

namespace SlowMotionPhysicsFix
{
	constexpr UInt32 kAddr_StepDeltaTimeCall = 0xC6AFF9;
	constexpr UInt32 kAddr_SetFrameTimeMarkerCall = 0xC6AFC5;
	constexpr float kMinStepTime = 0.001f;
	constexpr int kMaxStepsPerFrame = 16;
	constexpr float kSlowMotionThreshold = 0.999f;

	struct Setting { void* vtbl; float f; const char* name; };

	static UInt32* g_VATSMode = (UInt32*)0x11F2258;
	static float* g_globalTimeMultiplier = (float*)0x11AC3A0;
	static Setting* g_havokMaxTime = (Setting*)0x1267B34;
	static UInt32 originalStepDeltaTime = 0;
	static UInt32 originalSetFrameTimeMarker = 0;

	static bool IsVATSActive() { return *g_VATSMode != 0; }
	static float GetGlobalTimeMultiplier() { return *g_globalTimeMultiplier; }

	static float GetEffectiveStepTime(float timeMult)
	{
		float stepTime = g_havokMaxTime->f * timeMult;
		if (stepTime < kMinStepTime)
			stepTime = kMinStepTime;
		return stepTime;
	}

	void __fastcall Hook_SetFrameTimeMarker(void* hkpWorld, void* edx, float delta) {
		if (!IsVATSActive()) {
			float timeMult = GetGlobalTimeMultiplier();
			if (timeMult < kSlowMotionThreshold) {
				float maxDelta = kMaxStepsPerFrame * GetEffectiveStepTime(timeMult);
				if (delta > maxDelta)
					delta = maxDelta;
			}
		}
		ThisCall<void>(originalSetFrameTimeMarker, hkpWorld, delta);
	}

	void __fastcall Hook_StepDeltaTime(void* hkpWorld, void* edx, float stepTime) {
		if (IsVATSActive()) {
			ThisCall<void>(originalStepDeltaTime, hkpWorld, stepTime);
			return;
		}
		if (stepTime < kMinStepTime)
			stepTime = kMinStepTime;
		ThisCall<void>(originalStepDeltaTime, hkpWorld, stepTime);
	}

	void Init() {
		originalSetFrameTimeMarker = SafeWrite::GetRelJumpTarget(kAddr_SetFrameTimeMarkerCall);
		SafeWrite::WriteRelCall(kAddr_SetFrameTimeMarkerCall, (UInt32)Hook_SetFrameTimeMarker);
		originalStepDeltaTime = SafeWrite::GetRelJumpTarget(kAddr_StepDeltaTimeCall);
		SafeWrite::WriteRelCall(kAddr_StepDeltaTimeCall, (UInt32)Hook_StepDeltaTime);
	}
}

