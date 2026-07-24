#pragma once

namespace CrouchCommands {
	void InstallHooks(); //prologue patches on 0x8B39F0/0x981520, call once at PostLoad, idempotent
	void ClearState();
	void RegisterCommands(void* nvse); //0x405B: ForceCrouch, DisableCrouching
}
