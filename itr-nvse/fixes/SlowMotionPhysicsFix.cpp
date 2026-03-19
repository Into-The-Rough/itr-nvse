//clamps physics timestep to prevent ragdoll energy gain during extreme slowmo
//NOT hot-reloadable - requires game restart

#include "SlowMotionPhysicsFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/CallTemplates.h"

#include "internal/globals.h"

namespace SlowMotionPhysicsFix
{
	constexpr UInt32 kAddr_StepDeltaTimeCall = 0xC6AFF9;
	constexpr UInt32 kAddr_SetFrameTimeMarkerCall = 0xC6AFC5;
	constexpr float kMinStepTime = 0.001f;

	static UInt32* g_VATSMode = (UInt32*)0x11F2258;
	static UInt32 originalStepDeltaTime = 0;
	static UInt32 originalSetFrameTimeMarker = 0;

	static bool IsVATSActive() { return *g_VATSMode != 0; }

	void __fastcall Hook_SetFrameTimeMarker(void* hkpWorld, void* edx, float delta) {
		if (!IsVATSActive()) {
			float maxDelta = 16 * kMinStepTime;
			if (delta > maxDelta)
				delta = maxDelta;
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
		SafeWrite::Write32(kAddr_SetFrameTimeMarkerCall + 1, (UInt32)Hook_SetFrameTimeMarker - kAddr_SetFrameTimeMarkerCall - 5);
		originalStepDeltaTime = SafeWrite::GetRelJumpTarget(kAddr_StepDeltaTimeCall);
		SafeWrite::Write32(kAddr_StepDeltaTimeCall + 1, (UInt32)Hook_StepDeltaTime - kAddr_StepDeltaTimeCall - 5);
		Log("SlowMotionPhysicsFix installed");
	}
}

void SlowMotionPhysicsFix_Init()
{
	SlowMotionPhysicsFix::Init();
}
