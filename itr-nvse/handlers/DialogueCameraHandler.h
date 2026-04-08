#pragma once

struct Mat3;

namespace DialogueCameraHandler {
	struct DebugState {
		bool enabled;
		int angleMode;
		int fixedAngle;
		int currentAngle;
		float dollySpeed;
		float dollyMaxDist;
		int dollyRunOnce;
		float shakeAmplitude;
	};

	bool Init(void* nvse);
	void Update();
	bool InstallCameraHooks();
	void SetEnabled(bool enabled);
	bool IsEnabled();
	bool SetAngleMode(int mode);
	bool SetFixedAngle(int angle);
	int SetCurrentAngle(int angle);
	void SetDolly(float speed, float maxDist, int runOnce);
	void SetShakeAmplitude(float amplitude);
	DebugState GetDebugState();
	void RestoreDebugState(const DebugState& state);
	void SetExternalRotation(const Mat3& rot);
	void ClearExternalRotation();
	void RegisterCommands(void* nvse);
	void RegisterCommands2(void* nvse);
}
