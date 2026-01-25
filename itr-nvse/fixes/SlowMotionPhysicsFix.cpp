//clamps physics timestep to prevent ragdoll energy gain during extreme slowmo

#include "SlowMotionPhysicsFix.h"
#include <Windows.h>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace SlowMotionPhysicsFix
{
	constexpr uint32_t kAddr_StepDeltaTimeCall = 0xC6AFF9;
	constexpr uint32_t kAddr_SetFrameTimeMarkerCall = 0xC6AF85;
	constexpr float kMinStepTime = 0.001f;
	constexpr int kMaxStepsPerFrame = 16;

	static uint32_t* g_VATSMode = (uint32_t*)0x11F2258;
	static uint32_t originalStepDeltaTime = 0;
	static uint32_t originalSetFrameTimeMarker = 0;

	static bool IsVATSActive() { return *g_VATSMode != 0; }

	void PatchWrite32(uint32_t addr, uint32_t data) {
		DWORD oldProtect;
		VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint32_t*)addr = data;
		VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
	}

	void PatchCall(uint32_t jumpSrc, uint32_t jumpTgt) {
		PatchWrite32(jumpSrc + 1, jumpTgt - jumpSrc - 5);
	}

	uint32_t ReadCallTarget(uint32_t jumpSrc) {
		return *(uint32_t*)(jumpSrc + 1) + jumpSrc + 5;
	}

	void __fastcall Hook_SetFrameTimeMarker(void* hkpWorld, void* edx, float delta) {
		if (!IsVATSActive()) {
			float maxDelta = kMaxStepsPerFrame * kMinStepTime;
			if (delta > maxDelta)
				delta = maxDelta;
		}
		((void(__thiscall*)(void*, float))originalSetFrameTimeMarker)(hkpWorld, delta);
	}

	void __fastcall Hook_StepDeltaTime(void* hkpWorld, void* edx, float stepTime) {
		if (IsVATSActive()) {
			((void(__thiscall*)(void*, float))originalStepDeltaTime)(hkpWorld, stepTime);
			return;
		}
		if (stepTime < kMinStepTime)
			stepTime = kMinStepTime;
		((void(__thiscall*)(void*, float))originalStepDeltaTime)(hkpWorld, stepTime);
	}

	void Init() {
		originalSetFrameTimeMarker = ReadCallTarget(kAddr_SetFrameTimeMarkerCall);
		PatchCall(kAddr_SetFrameTimeMarkerCall, (uint32_t)Hook_SetFrameTimeMarker);
		originalStepDeltaTime = ReadCallTarget(kAddr_StepDeltaTimeCall);
		PatchCall(kAddr_StepDeltaTimeCall, (uint32_t)Hook_StepDeltaTime);
		Log("SlowMotionPhysicsFix installed");
	}
}

void SlowMotionPhysicsFix_Init()
{
	SlowMotionPhysicsFix::Init();
}
