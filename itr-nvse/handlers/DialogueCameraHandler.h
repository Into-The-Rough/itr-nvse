#pragma once

struct Mat3;

namespace DialogueCameraHandler {
	bool Init(void* nvse);
	void Update();
	bool InstallCameraHooks();
	void SetEnabled(bool enabled);
	void SetExternalRotation(const Mat3& rot);
	void ClearExternalRotation();
	void RegisterCommands(void* nvse);
	void RegisterCommands2(void* nvse);
}
