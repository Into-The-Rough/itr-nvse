//clamps physics timestep to prevent ragdoll energy gain during extreme slowmo
//NOT hot-reloadable - requires game restart

#include "SlowMotionPhysicsFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/GameGlobals.h"

#include "internal/globals.h"

namespace SlowMotionPhysicsFix
{
	constexpr UInt32 kAddr_StepDeltaTimeCall = 0xC6AFF9;
	constexpr UInt32 kAddr_SetFrameTimeMarkerCall = 0xC6AFC5;
	constexpr float kMinStepTime = 0.001f;
	constexpr int kMaxStepsPerFrame = 16;
	constexpr float kSlowMotionThreshold = 0.999f;

	static Detours::CallDetour s_stepDeltaTimeCall;
	static Detours::CallDetour s_setFrameTimeMarkerCall;
	typedef void(__thiscall* HavokWorldFloatFn)(void*, float);

	static float GetEffectiveStepTime(float timeMult)
	{
		float stepTime = GetHavokMaxTime() * timeMult;
		if (stepTime < kMinStepTime)
			stepTime = kMinStepTime;
		return stepTime;
	}

	void __fastcall Hook_SetFrameTimeMarker(void* hkpWorld, void* edx, float delta) {
		if (!::IsVATSActive()) {
			float timeMult = GetGlobalTimeMultiplier();
			if (timeMult < kSlowMotionThreshold) {
				float maxDelta = kMaxStepsPerFrame * GetEffectiveStepTime(timeMult);
				if (delta > maxDelta)
					delta = maxDelta;
			}
		}
		auto original = reinterpret_cast<HavokWorldFloatFn>(s_setFrameTimeMarkerCall.GetOverwrittenAddr());
		original(hkpWorld, delta);
	}

	void __fastcall Hook_StepDeltaTime(void* hkpWorld, void* edx, float stepTime) {
		auto original = reinterpret_cast<HavokWorldFloatFn>(s_stepDeltaTimeCall.GetOverwrittenAddr());

		if (::IsVATSActive()) {
			original(hkpWorld, stepTime);
			return;
		}
		if (stepTime < kMinStepTime)
			stepTime = kMinStepTime;
		original(hkpWorld, stepTime);
	}

	void Init() {
		s_setFrameTimeMarkerCall.WriteRelCall(kAddr_SetFrameTimeMarkerCall, Hook_SetFrameTimeMarker);
		s_stepDeltaTimeCall.WriteRelCall(kAddr_StepDeltaTimeCall, Hook_StepDeltaTime);
	}
}
