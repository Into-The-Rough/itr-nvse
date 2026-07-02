#pragma once

//iNPCDoorUnlockBlock levels (the player and followers are always exempt so companions don't get stranded):
//0 = vanilla (guards, cell owners, followers can bypass locks)
//1 = only direct door owners/faction can bypass (no cell ownership, no guard bypass)
//2 = nobody else bypasses, all other NPCs must use a key or lockpicks

namespace NPCDoorUnlockBlock {
	void Init(int level);
	void SetLevel(int level);
}
