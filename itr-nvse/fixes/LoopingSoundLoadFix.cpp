//fixes two engine holes that orphan looping weapon sounds (minigun, gatling, flamer, ripper)
//
//1) death reload: BGSSaveLoadGame::LoadGame runs synchronously mid-frame inside
//PlayerCharacter::Update (timer at 0x93FEDF, load at 0x93FF79), then the rest of the
//frame still runs Character::Update twice (0x9400BF/0x940113), letting Actor::Update
//recreate the looping shot sound after the load's own stop-all (sub_850C20) already
//ran. the handle (ExtraWeaponAttackSound type 0x86, no-op dtor) is then lost in
//post-load reinit and the loop plays at its last position forever - the manager sweep
//retriggers loop-family sounds so they never expire on their own.
//covered by reposting the engine's stop-all at PostLoadGame with the animation-driven
//mask (0x40000000, set on every weapon loop at creation 0x8894A0, AND-matched by the
//dispatcher, music/radio exempt). legitimate attack loops are recreated next frame by
//the owning actor's state machine, so only orphans die.
//
//2) cell transition: when a human NPC's 3D unloads mid-fire, nothing stops the loop.
//Set3D(0) (0x5702E0) has three stop actions and all miss it: the flag sweep 0x578250
//only stops sounds following 0x20000-flagged nodes but the attack loop follows the
//weapon fire node (0x8BE0A0, plain NiNode, never flagged), the root backstop only
//stops root-followers, and the stored-handle fade 0x579AC0 switches on base form type
//and skips TESNPC (0x2A) while handling TESCreature (0x2B, jump table 0x57A2C4).
//covered by fading the stored weapon-sound extras on NPC Set3D(0), mirroring the
//sequence Actor::Kill uses at 0x89D9A0 (fade 500ms if playing, write-back of the
//invalidated handle removes the extra). anim-text-key loops (ripper idle) need no
//help here - PlaySounds flags both attach node and actor root (0x4EF512/0x4EF52B) so
//the sweep catches them.

#include "LoopingSoundLoadFix.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"

#include "internal/globals.h"

namespace LoopingSoundLoadFix
{
	static Detours::JumpDetour s_set3DDetour;
	static UInt8* s_set3DOrig = nullptr;

	constexpr UInt8 kFormType_TESNPC = 0x2A;

	//push ebp / mov ebp,esp / push -1
	static constexpr UInt8 kSet3DPrologue[5] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };

	struct SoundHandle
	{
		UInt32 id;
		UInt8 assumeSuccess;
		UInt32 state;
	};

	static bool FadeExtraLoop(void* apExtraList, UInt32 aReadFn, UInt32 aWriteFn)
	{
		SoundHandle handle = { 0xFFFFFFFF, 0, 0 };
		ThisCall<void>(aReadFn, apExtraList, &handle);
		bool playing = ThisCall<bool>(0xAD8930, &handle); //BSSoundHandle::IsPlaying
		if (playing)
			ThisCall<void>(0xAD8DA0, &handle, 500); //FadeOutAndRelease
		else
			ThisCall<void>(0xAD8D10, &handle); //Release
		ThisCall<void>(aWriteFn, apExtraList, &handle); //invalid handle removes the extra
		return playing;
	}

	static void __fastcall Hook_Set3D(void* apRef, void*, void* apNode, UInt32 aUnk)
	{
		if (!apNode)
		{
			void* base = *(void**)((UInt8*)apRef + 0x20);
			if (base && *((UInt8*)base + 0x4) == kFormType_TESNPC)
			{
				void* extraList = (UInt8*)apRef + 0x44;
				bool a = FadeExtraLoop(extraList, 0x418A00, 0x41A540); //ExtraWeaponAttackSound 0x86
				bool b = FadeExtraLoop(extraList, 0x4189C0, 0x41A3E0); //second weapon sound 0x83
				if (a || b)
					Log("LoopingSoundLoadFix: faded live weapon loop on NPC 3D unload");
			}
		}
		ThisCall<void>((UInt32)s_set3DOrig, apRef, apNode, aUnk);
	}

	void Init()
	{
		if (memcmp((void*)0x5702E0, kSet3DPrologue, sizeof(kSet3DPrologue)) != 0)
		{
			Log("LoopingSoundLoadFix: Set3D prologue bytes changed, unload fix disabled");
			return;
		}
		if (!s_set3DDetour.WriteRelJump(0x5702E0, Hook_Set3D, 5, &s_set3DOrig))
		{
			Log("LoopingSoundLoadFix: Set3D prologue already patched, unload fix disabled");
			return;
		}
		Log("LoopingSoundLoadFix: installed");
	}

	void OnPostLoadGame()
	{
		//BSAudioManager stop-all shim, thiscall with unused ecx, fetches its own singleton
		ThisCall<void>(0xAD84E0, nullptr, 0x40000000, 0);
		Log("LoopingSoundLoadFix: queued animation-driven sound purge post-load");
	}
}
