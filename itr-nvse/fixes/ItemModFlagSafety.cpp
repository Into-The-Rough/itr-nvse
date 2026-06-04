//supports ExtraWeaponModFlags on non-weapons for SetItemModFlags
//jip gates the inventory plus marker to weapons
//vanilla display paths also assume weapon itemMod slots
//not hot-reloadable

#include "ItemModFlagSafety.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/globals.h"

namespace ItemModFlagSafety
{
	//sub_4BD570 reads the weapon itemMod slot form
	typedef int(__thiscall* GetModSlotForm_t)(void* baseForm, int bit);
	//entry to base form helper used by PopulateItemStatsDisplay
	typedef void* (__thiscall* EntryGetForm_t)(void* entry);
	//sub_4BD820 returns nonzero when the entry has ExtraWeaponModFlags
	typedef char(__thiscall* EntryHasModFlag_t)(void* entry);

	static const EntryGetForm_t    EntryGetForm = (EntryGetForm_t)0x44DDC0;
	static const EntryHasModFlag_t EntryHasModFlag = (EntryHasModFlag_t)0x4BD820;

	static Detours::JumpDetour s_slotGuard;
	static GetModSlotForm_t s_originalGetSlot = nullptr;
	static Detours::CallDetour s_statCardGate;

	static bool IsWeaponForm(void* form)
	{
		return form && *(UInt8*)((char*)form + 0x04) == 0x28; //kFormType_TESObjectWEAP
	}

	//__thiscall baseForm in ecx and bit on stack
	int __fastcall Hook_GetModSlotForm(void* baseForm, void* edx, int bit)
	{
		if (!IsWeaponForm(baseForm) || !s_originalGetSlot) return 0;
		return s_originalGetSlot(baseForm, bit);
	}

	//replaces the stat card has mod gate call
	char __fastcall Hook_StatCardGate(void* entry)
	{
		if (entry && !IsWeaponForm(EntryGetForm(entry))) return 0;
		return EntryHasModFlag(entry);
	}

	//sub_775A00 builds the world activation prompt
	//jip only appends plus in the weapon branch
	//0x776F87 is after the prompt name write and before display branching
	typedef void* (__thiscall* GetByType_t)(void*, UInt32);
	static const GetByType_t GetByType = (GetByType_t)0x410220;

	static void __fastcall RolloverAppendPlus(void* refr)
	{
		if (!refr) return;
		void* base = *(void**)((char*)refr + 0x20); //base form
		if (!base || *(UInt8*)((char*)base + 0x04) == 0x28) return; //weapon handled by jip
		void* x = GetByType((void*)((char*)refr + 0x44), 0x8D); //ref BaseExtraList is embedded at +0x44
		if (!x || *(UInt8*)((char*)x + 0x0C) == 0) return;
		char* name = (char*)0x11D9C48;
		UInt32 len = 0;
		while (name[len]) ++len;
		if (len && len < 0x103) { name[len] = '+'; name[len + 1] = 0; }
	}

	static Detours::JumpDetour s_rolloverName;
	static UInt32 s_rolloverTramp = 0;

	__declspec(naked) void Hook_RolloverName()
	{
		__asm
		{
			pushfd
			pushad
			mov		ecx, [ebp+8]   //arg_0 = the rollover ref (same frame, ebp intact)
			call	RolloverAppendPlus
			popad
			popfd
			jmp		[s_rolloverTramp]   //replays mov ecx,pPlayer then jmps to 0x776F8D
		}
	}

	//sub_4BD570 prologue is 7 bytes
	void Init()
	{
		if (s_slotGuard.WriteRelJump(0x4BD570, Hook_GetModSlotForm, 7)) {
			s_originalGetSlot = s_slotGuard.GetTrampoline<GetModSlotForm_t>();
			if (!s_originalGetSlot) {
				Log("ItemModFlagSafety: slot guard trampoline not created");
				s_slotGuard.Remove();
			}
		}
		else {
			Log("ItemModFlagSafety: slot guard failed to install");
		}

		//0x707FBF = the v123 gate call in Interface::PopulateItemStatsDisplay
		if (!s_statCardGate.WriteRelCall(0x707FBF, Hook_StatCardGate))
			Log("ItemModFlagSafety: stat-card gate failed to install");

		//0x776F87 is mov ecx,pPlayer after the rollover name is built
		if (s_rolloverName.WriteRelJump(0x776F87, Hook_RolloverName, 6))
			s_rolloverTramp = (UInt32)s_rolloverName.GetTrampoline<void*>();
		else
			Log("ItemModFlagSafety: rollover name hook failed to install");
	}

	//jip installs GetEntryDataModFlagsHook at 0x4BD820 during PostLoad
	//neutralise its weapon gate so flagged non-weapons get the plus marker
	//must run after jip PostLoad
	void InitJIPModFlagGate()
	{
		UInt8* p = (UInt8*)0x4BD820;
		if (*p != 0xE9) {
			Log("ItemModFlagGate: 0x4BD820 is not a JIP jmp, skipping (no inventory '+' on non-weapons)");
			return;
		}
		UInt8* fn = p + 5 + *(UInt32*)(p + 1); //jip hook
		for (int i = 0; i < 64; ++i) {
			//8B 41 08      mov eax,[ecx+8]
			//80 78 04 28   cmp byte ptr [eax+4],kFormType_TESObjectWEAP
			//75 ??         jnz retn0
			if (fn[i] == 0x8B && fn[i+1] == 0x41 && fn[i+2] == 0x08 &&
				fn[i+3] == 0x80 && fn[i+4] == 0x78 && fn[i+5] == 0x04 && fn[i+6] == 0x28 &&
				fn[i+7] == 0x75) {
				const UInt8 nops[2] = { 0x90, 0x90 };
				SafeWrite::WriteBuf((UInt32)(fn + i + 7), nops, 2);
				return;
			}
		}
		Log("ItemModFlagGate: gate signature not found in GetEntryDataModFlagsHook");
	}
}
