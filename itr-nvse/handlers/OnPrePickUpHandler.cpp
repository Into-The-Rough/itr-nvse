//cancellable pre-pickup event. handlers SetFunctionValue 0 to veto.
//once any handler vetoes, later handlers can't un-veto.

#include "OnPrePickUpHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"
#include "internal/SafeWrite.h"

extern void Log(const char* fmt, ...);

class ExtraDataList;

namespace OnPrePickUpHandler {

constexpr char kEventName[] = "ITR:OnPrePickUp";

constexpr UInt32 kAddr_PlayerPickUp           = 0x00953FF0;
constexpr UInt32 kAddr_ActorPickUp            = 0x00891E00;
constexpr UInt32 kAddr_AddObjecttoContainer   = 0x00574FA0;
constexpr UInt32 kAddr_ContainerTransferItem  = 0x0075DC80;

//resume after push ebp; mov ebp,esp; sub esp,N
constexpr UInt32 kRet_PlayerPickUp            = 0x00953FF6;
constexpr UInt32 kRet_ActorPickUp             = 0x00891E06;
constexpr UInt32 kRet_AddObjecttoContainer    = 0x00574FA6;
constexpr UInt32 kRet_ContainerTransferItem   = 0x0075DC89;

constexpr UInt32 kTESObjectREFR_BaseFormOffset = 0x20;
static TESObjectREFR** g_thePlayer = (TESObjectREFR**)0x011DEA3C;
static void**    g_containerMenuPtrAddr = (void**)0x011D93F8;
static UInt32*   g_containerMenuSelAddr = (UInt32*)0x011D93FC;
constexpr UInt32 kContainerMenu_ContainerRefOffset = 0x74;
constexpr UInt32 kContainerMenu_PlayerListOffset   = 0x98;
constexpr UInt32 kContainerMenu_CurrentItemsOffset = 0xF8;

typedef TESObjectREFR* (__stdcall *InvRefCreateEntry_t)(TESObjectREFR* container, TESForm* itemForm, SInt32 countDelta, ExtraDataList* xData);
static InvRefCreateEntry_t g_invRefCreateEntry = nullptr;

constexpr UInt32 kNVSEData_InventoryReferenceCreateEntry = 7;

struct ContChangesEntry {
	void*    extendData;
	SInt32   countDelta;
	TESForm* type;
};

static bool DispatchResultCb(NVSEArrayVarInterface::Element& result, void* shouldPickAddr)
{
	UInt32& shouldPick = *static_cast<UInt32*>(shouldPickAddr);
	if (shouldPick && result.IsValid())
	{
		if (result.type == NVSEArrayVarInterface::Element::kType_Numeric)
			shouldPick = (result.num != 0.0) ? 1 : 0;
	}
	return true;
}

static bool DispatchPrePickUp(TESObjectREFR* picker, TESForm* baseForm, TESObjectREFR* itemRef, SInt32 count)
{
	if (!g_eventManagerInterface || !picker || !baseForm)
		return true;

	UInt32 shouldPick = 1;
	g_eventManagerInterface->DispatchEventAlt(kEventName, DispatchResultCb, &shouldPick,
		picker, baseForm, itemRef, &shouldPick);
	return shouldPick != 0;
}

static int __cdecl CheckPickUpObject(TESObjectREFR* picker, TESObjectREFR* itemRef, SInt32 count)
{
	if (!itemRef) return 1;
	TESForm* baseForm = *(TESForm**)((UInt8*)itemRef + kTESObjectREFR_BaseFormOffset);
	return DispatchPrePickUp(picker, baseForm, itemRef, count) ? 1 : 0;
}

__declspec(naked) static void PlayerPickUp_Hook()
{
	__asm {
		//ecx=this, [esp+4]=apObject, [+8]=count, [+0xC]=playSounds
		push    ecx
		mov     eax, [esp + 0x0C]
		push    eax
		mov     eax, [esp + 0x0C]
		push    eax
		mov     eax, [esp + 0x08]
		push    eax
		call    CheckPickUpObject
		add     esp, 0x0C
		pop     ecx
		test    eax, eax
		jz      veto

		push    ebp
		mov     ebp, esp
		sub     esp, 0x44
		mov     eax, kRet_PlayerPickUp
		jmp     eax

	veto:
		ret     0x0C
	}
}

__declspec(naked) static void ActorPickUp_Hook()
{
	__asm {
		push    ecx
		mov     eax, [esp + 0x0C]
		push    eax
		mov     eax, [esp + 0x0C]
		push    eax
		mov     eax, [esp + 0x08]
		push    eax
		call    CheckPickUpObject
		add     esp, 0x0C
		pop     ecx
		test    eax, eax
		jz      veto

		push    ebp
		mov     ebp, esp
		sub     esp, 0x54
		mov     eax, kRet_ActorPickUp
		jmp     eax

	veto:
		ret     0x0C
	}
}

//player-only filter keeps cell-load container fill and levelled-list npc init untouched
static int __cdecl CheckAddObjecttoContainer(TESObjectREFR* this_, TESForm* item, ExtraDataList* xData, SInt32 count)
{
	if (!this_ || this_ != *g_thePlayer) return 1;

	TESObjectREFR* invRef = nullptr;
	if (g_invRefCreateEntry && item)
		invRef = g_invRefCreateEntry(this_, item, count, xData);

	return DispatchPrePickUp(this_, item, invRef, count) ? 1 : 0;
}

__declspec(naked) static void AddObjecttoContainer_Hook()
{
	__asm {
		//ecx=this, [esp+4]=item, [+8]=xData, [+0xC]=count
		push    ecx
		mov     eax, [esp + 0x10]
		push    eax
		mov     eax, [esp + 0x10]
		push    eax
		mov     eax, [esp + 0x10]
		push    eax
		mov     eax, [esp + 0x0C]
		push    eax
		call    CheckAddObjecttoContainer
		add     esp, 0x10
		pop     ecx
		test    eax, eax
		jz      veto

		push    ebp
		mov     ebp, esp
		sub     esp, 0x0C
		mov     eax, kRet_AddObjecttoContainer
		jmp     eax

	veto:
		ret     0x0C
	}
}

static int __cdecl CheckContainerTransferItem(SInt32 count)
{
	void* menu = *g_containerMenuPtrAddr;
	if (!menu) return 1;

	void* currentItems = *(void**)((UInt8*)menu + kContainerMenu_CurrentItemsOffset);
	void* playerList = (UInt8*)menu + kContainerMenu_PlayerListOffset;
	if (currentItems == playerList) return 1;     //PUT direction

	UInt32 selRaw = *g_containerMenuSelAddr;
	if (!selRaw) return 1;
	auto* entry = reinterpret_cast<ContChangesEntry*>(selRaw);
	TESForm* item = entry->type;
	if (!item) return 1;

	TESObjectREFR* playerRef = *g_thePlayer;
	if (!playerRef) return 1;

	auto* container = *reinterpret_cast<TESObjectREFR**>((UInt8*)menu + kContainerMenu_ContainerRefOffset);

	TESObjectREFR* invRef = nullptr;
	if (g_invRefCreateEntry && container)
		invRef = g_invRefCreateEntry(container, item, count, nullptr);

	return DispatchPrePickUp(playerRef, item, invRef, count) ? 1 : 0;
}

__declspec(naked) static void ContainerTransferItem_Hook()
{
	__asm {
		mov     eax, [esp + 4]
		push    eax
		call    CheckContainerTransferItem
		add     esp, 4
		test    eax, eax
		jz      veto

		push    ebp
		mov     ebp, esp
		sub     esp, 0x154
		mov     eax, kRet_ContainerTransferItem
		jmp     eax

	veto:
		ret
	}
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	if (!g_eventManagerInterface)
	{
		Log("OnPrePickUpHandler: g_eventManagerInterface not ready, aborting Init");
		return false;
	}

	using P = NVSEEventManagerInterface::ParamType;
	using F = NVSEEventManagerInterface::EventFlags;
	static P params[] = {
		P::eParamType_AnyForm,
		P::eParamType_AnyForm,
		P::eParamType_IntPtr,
	};
	g_eventManagerInterface->RegisterEvent(kEventName, 3, params,
		(F)(F::kFlag_FlushOnLoad | F::kFlag_AllowScriptDispatch));

	auto* dataInterface = reinterpret_cast<NVSEDataInterface*>(nvse->QueryInterface(kInterface_Data));
	if (dataInterface)
		g_invRefCreateEntry = reinterpret_cast<InvRefCreateEntry_t>(
			dataInterface->GetFunc(kNVSEData_InventoryReferenceCreateEntry));

	//if another plugin already patched the prologue, skip - don't corrupt their trampoline
	auto TryInstall = [](const char* name, UInt32 addr, const UInt8* expected, UInt32 size, void* hook, UInt32 nopCount) -> bool {
		if (memcmp((void*)addr, expected, size) != 0)
		{
			Log("OnPrePickUp: %s prologue at 0x%X differs from expected, skipping (another plugin hooked here?)", name, addr);
			return false;
		}
		SafeWrite::WriteRelJump(addr, (UInt32)hook);
		if (nopCount > 0) SafeWrite::WriteNop(addr + 5, nopCount);
		return true;
	};

	//sub esp,154h is imm32 form, so 9 prologue bytes (5-byte jmp + 4 nops); others use imm8 (6 bytes)
	static const UInt8 kPrologue_PlayerPickUp[6]    = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x44 };
	static const UInt8 kPrologue_ActorPickUp[6]     = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x54 };
	static const UInt8 kPrologue_AddObjectTo[6]     = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };
	static const UInt8 kPrologue_ContainerXfer[9]   = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x54, 0x01, 0x00, 0x00 };

	int installed = 0;
	installed += TryInstall("PlayerCharacter::PickUpObject", kAddr_PlayerPickUp,          kPrologue_PlayerPickUp,  6, (void*)PlayerPickUp_Hook,          1);
	installed += TryInstall("Actor::PickUpObject",           kAddr_ActorPickUp,           kPrologue_ActorPickUp,   6, (void*)ActorPickUp_Hook,           1);
	installed += TryInstall("TESObjectREFR::AddObjecttoContainer", kAddr_AddObjecttoContainer, kPrologue_AddObjectTo, 6, (void*)AddObjecttoContainer_Hook, 1);
	installed += TryInstall("ContainerMenu::TransferItem",   kAddr_ContainerTransferItem, kPrologue_ContainerXfer, 9, (void*)ContainerTransferItem_Hook, 4);

	Log("OnPrePickUp: %d/4 hooks installed", installed);
	return installed > 0;
}

}
