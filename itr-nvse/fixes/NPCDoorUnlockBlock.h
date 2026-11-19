#pragma once

//0=vanilla, 1=direct/faction owners only, 2=require key/lockpick (player/followers always exempt)
namespace NPCDoorUnlockBlock {
	void Init(int level);
	void SetLevel(int level);
}
