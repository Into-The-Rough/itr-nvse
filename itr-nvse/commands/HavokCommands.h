#pragma once

namespace HavokCommands {
	void RegisterCommands(void* nvse);
	void RegisterRagdollCommands(void* nvse);
	void RegisterRagdollLimbCommands(void* nvse);
	void RegisterLimbVelocityCommands(void* nvse);
	void RegisterCenterOfMassCommands(void* nvse);
	void ClearState();
	void Update();
}
