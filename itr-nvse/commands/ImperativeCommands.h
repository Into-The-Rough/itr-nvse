#pragma once

namespace ImperativeCommands {
	bool Init(void* nvse);
	void ClearState();
	void RegisterCommands(void* nvse); //0x4021-0x4023: GetRefsSortedByDistance, Duplicate, GetAvailableRecipes
	void RegisterTargetLocationCommands(void* nvse); //0x4025-0x4028: combat target location getters
	void RegisterDebugCommands(void* nvse); //0x4029: DumpCombatTarget, debug builds only
	void RegisterAidCommands(void* nvse); //0x4030: UseAidItem
	void RegisterCombatSkillCommands(void* nvse); //0x4035: SetCreatureCombatSkill
	void RegisterForceReloadCommands(void* nvse); //0x4037: ForceReload
	void RegisterSetRaceCommands(void* nvse); //0x403B: SetRaceAlt
	void RegisterContactWatchCommands(void* nvse); //0x405F: SetOnContactWatch, GetOnContactWatch
	void RegisterRefillAmmoCommands(void* nvse); //0x4066: RefillAmmo
}
