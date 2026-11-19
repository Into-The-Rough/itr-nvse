//world and container-take pre-pickup veto (0x574FA0 is crafting/reward delivery, excluded)

#include <Windows.h>
#include <cstring>

#include "OnPrePickUpHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/GameLayout.h"
#include "internal/GameGlobals.h"
#include "internal/globals.h"

struct ExtraDataList;

namespace OnPrePickUpHandler {

	constexpr char kEventName[] = "ITR:OnPrePickUp";

	constexpr UInt32 kAddr_PlayerPickUp = 0x00953FF0;
	constexpr UInt32 kAddr_ActorPickUp = 0x00891E00;
	constexpr UInt32 kAddr_ContainerTransferItem = 0x0075DC80;

	constexpr UInt8 kPrologue_PlayerPickUp[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x44};
	constexpr UInt8 kPrologue_ActorPickUp[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x54};
	constexpr UInt8 kPrologue_ContainerTransferItem[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x54, 0x01, 0x00, 0x00};

	static Detours::JumpDetour s_playerPickUpDetour;
	static Detours::JumpDetour s_actorPickUpDetour;
	static Detours::JumpDetour s_containerTransferItemDetour;

	using PlayerPickUp_t = void(__thiscall*)(TESObjectREFR*, TESObjectREFR*, SInt32, UInt8);
	using ActorPickUp_t = int(__thiscall*)(TESObjectREFR*, TESObjectREFR*, SInt32, UInt8);
	using ContainerTransferItem_t = void(__cdecl*)(SInt32);
	static PlayerPickUp_t s_playerPickUp = nullptr;
	static ActorPickUp_t s_actorPickUp = nullptr;
	static ContainerTransferItem_t s_containerTransferItem = nullptr;
	static DWORD s_mainThreadId = 0;

	using InvRefCreateEntry_t = TESObjectREFR*(__stdcall*)(TESObjectREFR * container, TESForm* itemForm, SInt32 countDelta, ExtraDataList* xData);
	static InvRefCreateEntry_t g_invRefCreateEntry = nullptr;

	constexpr UInt32 kNVSEData_InventoryReferenceCreateEntry = 7;

	static bool DispatchResultCb(NVSEArrayVarInterface::Element& result, void* shouldPickAddr)
	{
		UInt32& shouldPick = *static_cast<UInt32*>(shouldPickAddr);
		if (shouldPick && result.IsValid()) {
			if (result.type == NVSEArrayVarInterface::Element::kType_Numeric)
				shouldPick = (result.num != 0.0) ? 1 : 0;
		}
		return true;
	}

	static bool DispatchPrePickUp(TESObjectREFR* picker, TESForm* baseForm, TESObjectREFR* itemRef, SInt32 count)
	{
		if (!g_eventManagerInterface || !picker || !baseForm)
			return true;

		if (GetCurrentThreadId() != s_mainThreadId) {
			static volatile LONG loggedOffThread = 0;
			if (InterlockedCompareExchange(&loggedOffThread, 1, 0) == 0)
				Log("OnPrePickUp: pickup ran off main thread, declining to dispatch");
			return true;
		}

		//a handler adding items retriggers a pickup site synchronously, cap the depth
		//and let pickups past it proceed, breaking the cycle
		static UInt32 s_depth = 0;
		if (s_depth >= 16) {
			static volatile LONG loggedDepth = 0;
			if (InterlockedCompareExchange(&loggedDepth, 1, 0) == 0)
				Log("OnPrePickUp: recursion cap hit, letting pickup proceed");
			return true;
		}

		UInt32 shouldPick = 1;
		s_depth++;
		g_eventManagerInterface->DispatchEventAlt(kEventName, DispatchResultCb, &shouldPick,
			picker, baseForm, itemRef, &shouldPick);
		s_depth--;
		return shouldPick != 0;
	}

	static int __cdecl CheckPickUpObject(TESObjectREFR* picker, TESObjectREFR* itemRef, SInt32 count)
	{
		if (!itemRef) return 1;
		TESForm* baseForm = TESObjectREFRGetBaseForm(itemRef);
		return DispatchPrePickUp(picker, baseForm, itemRef, count) ? 1 : 0;
	}

	static void __fastcall Hook_PlayerPickUp(TESObjectREFR* picker, void* edx, TESObjectREFR* itemRef, SInt32 count, UInt8 playSounds)
	{
		if (!CheckPickUpObject(picker, itemRef, count))
			return;

		s_playerPickUp(picker, itemRef, count, playSounds);
	}

	static int __fastcall Hook_ActorPickUp(TESObjectREFR* picker, void* edx, TESObjectREFR* itemRef, SInt32 count, UInt8 playSounds)
	{
		if (!CheckPickUpObject(picker, itemRef, count))
			return 0;

		return s_actorPickUp(picker, itemRef, count, playSounds);
	}

	static int __cdecl CheckContainerTransferItem(SInt32 count)
	{
		void* menu = GetContainerMenu();
		if (!menu) return 1;

		void* currentItems = ContainerMenuGetCurrentItems(menu);
		void* playerList = ContainerMenuGetLeftItems(menu);
		if (currentItems == playerList) return 1; //PUT direction

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

	static void __cdecl Hook_ContainerTransferItem(SInt32 count)
	{
		if (!CheckContainerTransferItem(count))
			return;

		if (s_containerTransferItem)
			s_containerTransferItem(count);
	}

	template<typename T>
	static bool InstallEntryDetour(const char* name, Detours::JumpDetour& detour, UInt32 addr, void* hook,
		const UInt8* expected, UInt32 size, T& original)
	{
		if (std::memcmp((void*)addr, expected, size) != 0) {
			Log("OnPrePickUp: %s prologue at 0x%X differs from expected, skipping", name, addr);
			return false;
		}

		if (!detour.WriteRelJump(addr, hook, size)) {
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

		if (!g_eventManagerInterface) {
			Log("OnPrePickUpHandler: g_eventManagerInterface not ready, aborting Init");
			return false;
		}

		s_mainThreadId = GetCurrentThreadId();

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
		installed += InstallEntryDetour("PlayerCharacter::PickUpObject", s_playerPickUpDetour, kAddr_PlayerPickUp,
			(void*)Hook_PlayerPickUp, kPrologue_PlayerPickUp, sizeof(kPrologue_PlayerPickUp), s_playerPickUp);
		installed += InstallEntryDetour("Actor::PickUpObject", s_actorPickUpDetour, kAddr_ActorPickUp,
			(void*)Hook_ActorPickUp, kPrologue_ActorPickUp, sizeof(kPrologue_ActorPickUp), s_actorPickUp);
		installed += InstallEntryDetour("ContainerMenu::TransferItem", s_containerTransferItemDetour, kAddr_ContainerTransferItem,
			(void*)Hook_ContainerTransferItem, kPrologue_ContainerTransferItem, sizeof(kPrologue_ContainerTransferItem), s_containerTransferItem);

		Log("OnPrePickUp: %d/3 hooks installed", installed);
		return installed > 0;
	}

}
