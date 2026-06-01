//makes the weapon-mod "+" actually show on non-weapons carrying ExtraWeaponModFlags (0x8D, set by
//SetItemModFlags) and keeps the weapon-only display code from crashing on them.
//
//display (the "+"): JIP LN owns the live path and gates it to weapons in two places.
// - InitJIPModFlagGate: neutralise the weapon-type gate inside JIP's GetEntryDataModFlagsHook so the
//   inventory/barter list builder (ConstructItemEntryNameHook) marks flagged non-weapons too.
// - Hook_RolloverName: JIP's rollover "+" is weapon-branch-only, so append it for flagged non-weapon
//   refs at the name-build convergence in sub_775A00 (world activation prompt).
//safety (once non-weapons carry the flag): two vanilla paths assume a weapon.
// - Hook_GetModSlotForm: sub_4BD570 reads TESObjectWEAP::itemMod[bit] with no type check -> OOB on
//   non-weapons; return 0 for them (universal net: stat card, 3D preview, repair menu).
// - Hook_StatCardGate: skip PopulateItemStatsDisplay's "WEAPON MODS" block for non-weapons.
//NOT hot-reloadable - requires game restart

#include "ItemModFlagSafety.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/globals.h"

namespace ItemModFlagSafety
{
	//sub_4BD570(TESForm *baseForm, int bit) -> weapon itemMod slot form; junk for non-weapons
	typedef int(__thiscall* GetModSlotForm_t)(void* baseForm, int bit);
	//entry -> its base form (0x44DDC0, the helper PopulateItemStatsDisplay uses for its type switch)
	typedef void* (__thiscall* EntryGetForm_t)(void* entry);
	//sub_4BD820(entry) -> nonzero if the entry carries ExtraWeaponModFlags with a set bit
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

	//__thiscall (baseForm in ecx, bit on stack) -> __fastcall absorbs edx, bit aligns to first stack arg
	int __fastcall Hook_GetModSlotForm(void* baseForm, void* edx, int bit)
	{
		if (!IsWeaponForm(baseForm) || !s_originalGetSlot) return 0;
		return s_originalGetSlot(baseForm, bit);
	}

	//replaces the stat-card's "has mod?" gate call; entry arrives in ecx (sub_4BD820 is __thiscall)
	char __fastcall Hook_StatCardGate(void* entry)
	{
		if (entry && !IsWeaponForm(EntryGetForm(entry))) return 0;
		return EntryHasModFlag(entry);
	}

	//the world activation/rollover prompt is built in sub_775A00; JIP's RolloverWeaponHook only
	//runs in the weapon branch (case 40), so non-weapons never get the "+". 0x776F87 is the
	//convergence right after the name is written to byte_11D9C48 (0x11D9C48) and before any
	//display branch - append "+" here for a non-weapon ref carrying ExtraWeaponModFlags.
	typedef void* (__thiscall* GetByType_t)(void*, UInt32);
	static const GetByType_t GetByType = (GetByType_t)0x410220;

	static void __fastcall RolloverAppendPlus(void* refr)
	{
		if (!refr) return;
		void* base = *(void**)((char*)refr + 0x20); //TESObjectREFR::baseForm
		if (!base || *(UInt8*)((char*)base + 0x04) == 0x28) return; //weapon -> JIP handles it
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

	//sub_4BD570 prologue: push ebp(1) mov ebp,esp(2) push ecx(1) mov [ebp-4],ecx(3) = 7
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

		//0x776F87 = mov ecx,pPlayer (6 bytes), right after the rollover name is built
		if (s_rolloverName.WriteRelJump(0x776F87, Hook_RolloverName, 6))
			s_rolloverTramp = (UInt32)s_rolloverName.GetTrampoline<void*>();
		else
			Log("ItemModFlagSafety: rollover name hook failed to install");
	}

	//JIP LN owns the live inventory "+" path: at PostLoad it does WriteRelJump(0x4BD820,
	//GetEntryDataModFlagsHook), whose first act is a hard weapon-type gate
	//  mov eax,[ecx+8]; cmp byte ptr [eax+4],kFormType_TESObjectWEAP; jnz retn0
	//so non-weapons carrying our ExtraWeaponModFlags never get the marker. neutralise that jnz
	//(both the list builder ConstructItemEntryNameHook and the 0x4BD820 callers enter through it).
	//must run after JIP's PostLoad -> called from kMessage_PostPostLoad.
	void InitJIPModFlagGate()
	{
		UInt8* p = (UInt8*)0x4BD820;
		if (*p != 0xE9) {
			Log("ItemModFlagGate: 0x4BD820 is not a JIP jmp, skipping (no inventory '+' on non-weapons)");
			return;
		}
		UInt8* fn = p + 5 + *(UInt32*)(p + 1); //GetEntryDataModFlagsHook
		for (int i = 0; i < 64; ++i) {
			//8B 41 08            mov eax,[ecx+8]
			//80 78 04 28         cmp byte ptr [eax+4],kFormType_TESObjectWEAP
			//75 ??               jnz retn0
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
