//cancellable pre-pickup event. handlers SetFunctionValue 0 to veto.
//once any handler vetoes, later handlers can't un-veto.

#include "OnPrePickUpHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/GameLayout.h"
#include "internal/GameGlobals.h"

extern void Log(const char* fmt, ...);

struct ExtraDataList;

namespace OnPrePickUpHandler {

constexpr char kEventName[] = "ITR:OnPrePickUp";

constexpr UInt32 kAddr_PlayerPickUp           = 0x00953FF0;
constexpr UInt32 kAddr_ActorPickUp            = 0x00891E00;
constexpr UInt32 kAddr_AddObjecttoContainer   = 0x00574FA0;
constexpr UInt32 kAddr_ContainerTransferItem  = 0x0075DC80;

static Detours::JumpDetour s_playerPickUpDetour;
static Detours::JumpDetour s_actorPickUpDetour;
static Detours::JumpDetour s_addObjectToContainerDetour;
static Detours::JumpDetour s_containerTransferItemDetour;

typedef void(__thiscall* PlayerPickUp_t)(TESObjectREFR*, TESObjectREFR*, SInt32, UInt8);
typedef int(__thiscall* ActorPickUp_t)(TESObjectREFR*, TESObjectREFR*, SInt32, UInt8);
typedef int(__thiscall* AddObjecttoContainer_t)(TESObjectREFR*, TESForm*, ExtraDataList*, SInt32);
typedef void(__cdecl* ContainerTransferItem_t)(SInt32);
static PlayerPickUp_t s_playerPickUp = nullptr;
static ActorPickUp_t s_actorPickUp = nullptr;
static AddObjecttoContainer_t s_addObjectToContainer = nullptr;
static ContainerTransferItem_t s_containerTransferItem = nullptr;

typedef TESObjectREFR* (__stdcall *InvRefCreateEntry_t)(TESObjectREFR* container, TESForm* itemForm, SInt32 countDelta, ExtraDataList* xData);
static InvRefCreateEntry_t g_invRefCreateEntry = nullptr;

constexpr UInt32 kNVSEData_InventoryReferenceCreateEntry = 7;

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
	TESForm* baseForm = TESObjectREFRGetBaseForm(itemRef);
	return DispatchPrePickUp(picker, baseForm, itemRef, count) ? 1 : 0;
}

static void __fastcall PlayerPickUp_Hook(TESObjectREFR* picker, void*, TESObjectREFR* itemRef, SInt32 count, UInt8 playSounds)
{
	if (!CheckPickUpObject(picker, itemRef, count))
		return;

	if (s_playerPickUp)
		s_playerPickUp(picker, itemRef, count, playSounds);
}

static int __fastcall ActorPickUp_Hook(TESObjectREFR* picker, void*, TESObjectREFR* itemRef, SInt32 count, UInt8 playSounds)
{
	if (!CheckPickUpObject(picker, itemRef, count))
		return 0;

	return s_actorPickUp ? s_actorPickUp(picker, itemRef, count, playSounds) : 0;
}

//player-only filter keeps cell-load container fill and levelled-list npc init untouched
static int __cdecl CheckAddObjecttoContainer(TESObjectREFR* this_, TESForm* item, ExtraDataList* xData, SInt32 count)
{
	if (!this_ || this_ != *(TESObjectREFR**)g_thePlayerPtr) return 1;

	TESObjectREFR* invRef = nullptr;
	if (g_invRefCreateEntry && item)
		invRef = g_invRefCreateEntry(this_, item, count, xData);

	return DispatchPrePickUp(this_, item, invRef, count) ? 1 : 0;
}

static int __fastcall AddObjecttoContainer_Hook(TESObjectREFR* container, void*, TESForm* item, ExtraDataList* xData, SInt32 count)
{
	if (!CheckAddObjecttoContainer(container, item, xData, count))
		return 0;

	return s_addObjectToContainer ? s_addObjectToContainer(container, item, xData, count) : 0;
}

static int __cdecl CheckContainerTransferItem(SInt32 count)
{
	void* menu = GetContainerMenu();
	if (!menu) return 1;

	void* currentItems = ContainerMenuGetCurrentItems(menu);
	void* playerList = ContainerMenuGetLeftItems(menu);
	if (currentItems == playerList) return 1;     //PUT direction

	auto* entry = static_cast<ExtraContainerChanges::EntryData*>(GetContainerMenuSelection());
	if (!entry) return 1;
	TESForm* item = entry->type;
	if (!item) return 1;

	TESObjectREFR* playerRef = *(TESObjectREFR**)g_thePlayerPtr;
	if (!playerRef) return 1;

	auto* container = ContainerMenuGetContainerRef(menu);

	TESObjectREFR* invRef = nullptr;
	if (g_invRefCreateEntry && container)
		invRef = g_invRefCreateEntry(container, item, count, nullptr);

	return DispatchPrePickUp(playerRef, item, invRef, count) ? 1 : 0;
}

static void __cdecl ContainerTransferItem_Hook(SInt32 count)
{
	if (!CheckContainerTransferItem(count))
		return;

	if (s_containerTransferItem)
		s_containerTransferItem(count);
}

template <typename T>
static bool InstallEntryDetour(const char* name, Detours::JumpDetour& detour, UInt32 addr, void* hook, UInt32 size, T& original)
{
	if (!detour.WriteRelJump(addr, hook, size))
	{
		Log("OnPrePickUp: %s prologue at 0x%X could not be detoured", name, addr);
		return false;
	}

	original = detour.GetTrampoline<T>();
	return original != nullptr;
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

	int installed = 0;
	installed += InstallEntryDetour("PlayerCharacter::PickUpObject", s_playerPickUpDetour, kAddr_PlayerPickUp, (void*)PlayerPickUp_Hook, 6, s_playerPickUp);
	installed += InstallEntryDetour("Actor::PickUpObject", s_actorPickUpDetour, kAddr_ActorPickUp, (void*)ActorPickUp_Hook, 6, s_actorPickUp);
	installed += InstallEntryDetour("TESObjectREFR::AddObjecttoContainer", s_addObjectToContainerDetour, kAddr_AddObjecttoContainer, (void*)AddObjecttoContainer_Hook, 6, s_addObjectToContainer);
	installed += InstallEntryDetour("ContainerMenu::TransferItem", s_containerTransferItemDetour, kAddr_ContainerTransferItem, (void*)ContainerTransferItem_Hook, 9, s_containerTransferItem);

	Log("OnPrePickUp: %d/4 hooks installed", installed);
	return installed > 0;
}

}
