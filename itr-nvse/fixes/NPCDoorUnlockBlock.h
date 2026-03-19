#pragma once

//iNPCDoorUnlockBlock levels:
//0 = vanilla (guards, cell owners, followers can bypass locks)
//1 = only direct door owners/faction can bypass (no cell ownership, no guard bypass)
//2 = nobody bypasses locks, must use key or lockpicks

namespace NPCDoorUnlockBlock {
	void Init(int level);
	void SetLevel(int level);
}
