#include "LoopingSoundLoadFix.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/GameLayout.h"

#include "internal/globals.h"

namespace LoopingSoundLoadFix {
	static Detours::JumpDetour s_set3DDetour;
	static UInt8* s_set3DOrig = nullptr;

	//push ebp / mov ebp,esp / push -1
	static constexpr UInt8 kSet3DPrologue[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};

	static bool FadeExtraLoop(void* apExtraList, UInt32 aReadFn, UInt32 aWriteFn)
	{
		BSSoundHandleView handle = {0xFFFFFFFF, false, 0};
		ThisCall<void>(aReadFn, apExtraList, &handle);
		bool playing = ThisCall<bool>(0xAD8930, &handle); //BSSoundHandle::IsPlaying
		if (playing)
			ThisCall<void>(0xAD8DA0, &handle, 500); //FadeOutAndRelease
		else
			ThisCall<void>(0xAD8D10, &handle); //Release
		ThisCall<void>(aWriteFn, apExtraList, &handle); //invalid handle removes the extra
		return playing;
	}

	//Set3D(0) stored-handle fade at 0x579AC0 skips TESNPC (0x2A), leaving NPC weapon loops orphaned on unload
	static void __fastcall Hook_Set3D(TESObjectREFR* apRef, void* edx, void* apNode, UInt32 aUnk)
	{
		if (!apNode && apRef->baseForm && apRef->baseForm->typeID == kFormType_NPC) {
			void* extraList = &apRef->extraDataList;
			bool a = FadeExtraLoop(extraList, 0x418A00, 0x41A540); //ExtraWeaponAttackSound 0x86
			bool b = FadeExtraLoop(extraList, 0x4189C0, 0x41A3E0); //second weapon sound 0x83
			if (a || b)
				Log("LoopingSoundLoadFix: faded live weapon loop on NPC 3D unload");
		}
		ThisCall<void>((UInt32)s_set3DOrig, apRef, apNode, aUnk);
	}

	void Init()
	{
		if (memcmp((void*)0x5702E0, kSet3DPrologue, sizeof(kSet3DPrologue)) != 0) {
			Log("LoopingSoundLoadFix: Set3D prologue bytes changed, unload fix disabled");
			return;
		}
		if (!s_set3DDetour.WriteRelJump(0x5702E0, Hook_Set3D, 5, &s_set3DOrig)) {
			Log("LoopingSoundLoadFix: Set3D prologue already patched, unload fix disabled");
			return;
		}
		Log("LoopingSoundLoadFix: installed");
	}

	void OnPostLoadGame()
	{
		//death-reload runs Character::Update after LoadGame's stop-all, purge anim-driven mask 0x40000000 again
		ThisCall<void>(0xAD84E0, nullptr, 0x40000000, 0);
		Log("LoopingSoundLoadFix: queued animation-driven sound purge post-load");
	}
}
