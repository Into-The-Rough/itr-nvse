#include "BarterCommands.h"
#include "internal/Detours.h"
#include "internal/CallTemplates.h"
#include "internal/GameGlobals.h"
#include "internal/GameLayout.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/GameExtraData.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

#include <cstddef>
#include <unordered_set>

extern const _ExtractArgs ExtractArgs;
extern void Log(const char* fmt, ...);
extern void Console_Print(const char* fmt, ...);

namespace
{
	constexpr UInt32 kAddr_ShowBarterMenu = 0x704F80;
	constexpr UInt32 kAddr_PlayMenuSound = 0x706F30;
	constexpr UInt32 kAddr_BarterMenu_Close = 0x72D6D0;
	constexpr UInt32 kAddr_BarterMenu_ShouldHideItem = 0x7304B0;
	constexpr UInt32 kAddr_BarterMenu_TransferItem = 0x72F6F0;
	//sole call site of BarterMenu::CompleteTransaction 0x72FD10, in BarterMenu::DoClick
	constexpr UInt32 kAddr_BarterMenu_CompleteTransactionCall = 0x72D920;
	constexpr UInt32 kAddr_MenuItemsList_TileFromItem = 0x7A22D0;
	constexpr UInt32 kAddr_TESObjectREFR_GetObjectReference = 0x7AF430;
	constexpr UInt32 kAddr_MobileObject_IsTalkingThroughActivator = 0x574900;
	constexpr UInt32 kAddr_MobileObject_GetActivatorRef = 0x5E3FA0;
	constexpr UInt32 kAddr_Tile_IsFloatValueNotNull = 0xA01230;

	constexpr UInt32 kVtable_IsMobileObject = 0xFC;
	constexpr UInt32 kVtable_IsActor = 0x100;

	using ShowBarterMenu_t = void* (__cdecl*)(TESObjectREFR*, SInt32);
	using BarterMenuClose_t = void(__cdecl*)();
	using ShouldHideItem_t = bool(__cdecl*)(ExtraContainerChanges::EntryData*);
	using TransferItem_t = void(__cdecl*)(SInt32);
	using ProcessTransaction_t = void(__thiscall*)(void*);
	using TileFromItem_t = void* (__thiscall*)(void*, ExtraContainerChanges::EntryData**);
	using TESObjectREFR_GetObjectReference_t = TESForm* (__thiscall*)(TESObjectREFR*);
	using IsTalkingThroughActivator_t = bool(__thiscall*)(TESObjectREFR*);
	using GetActivatorRef_t = TESObjectREFR* (__thiscall*)(TESObjectREFR*);
	using TileIsFloatValueNotNull_t = bool(__thiscall*)(void*, UInt32);
	using RefPredicate_t = bool(__thiscall*)(TESObjectREFR*);

	const auto ShowBarterMenu = reinterpret_cast<ShowBarterMenu_t>(kAddr_ShowBarterMenu);
	const auto TileFromItem = reinterpret_cast<TileFromItem_t>(kAddr_MenuItemsList_TileFromItem);
	const auto GetObjectReference =
		reinterpret_cast<TESObjectREFR_GetObjectReference_t>(kAddr_TESObjectREFR_GetObjectReference);
	const auto IsTalkingThroughActivator =
		reinterpret_cast<IsTalkingThroughActivator_t>(kAddr_MobileObject_IsTalkingThroughActivator);
	const auto GetActivatorRef = reinterpret_cast<GetActivatorRef_t>(kAddr_MobileObject_GetActivatorRef);
	const auto TileIsFloatValueNotNull =
		reinterpret_cast<TileIsFloatValueNotNull_t>(kAddr_Tile_IsFloatValueNotNull);

	static_assert(offsetof(ExtraContainerChanges::EntryData, extendData) == 0x00,
		"EntryData extras offset changed");
	static_assert(offsetof(ExtraContainerChanges::EntryData, countDelta) == 0x04,
		"EntryData count offset changed");
	static_assert(offsetof(ExtraContainerChanges::EntryData, type) == 0x08,
		"EntryData form offset changed");
	static_assert(sizeof(ExtraContainerChanges::EntryData) == 0x0C,
		"EntryData must match engine ItemChange layout");

	struct BarterItemNode
	{
		ExtraContainerChanges::EntryData* item;
		BarterItemNode* next;
	};

	Detours::JumpDetour s_closeDetour;
	Detours::JumpDetour s_hideDetour;
	Detours::JumpDetour s_transferDetour;
	Detours::CallDetour s_processCall;

	BarterMenuClose_t s_origClose = nullptr;
	ShouldHideItem_t s_origShouldHideItem = nullptr;
	TransferItem_t s_origTransferItem = nullptr;
	ProcessTransaction_t s_origProcessTransaction = nullptr;

	BGSListForm* s_filterList = nullptr;
	UInt32 s_merchantRefID = 0;
	bool s_hooksInstalled = false;
	bool s_openingFilteredMenu = false;
	bool s_blacklistMode = false;

	bool CallRefPredicate(TESObjectREFR* ref, UInt32 vtableOffset)
	{
		auto** vtbl = ref ? *reinterpret_cast<void***>(ref) : nullptr;
		if (!vtbl)
			return false;

		auto fn = reinterpret_cast<RefPredicate_t>(vtbl[vtableOffset / sizeof(void*)]);
		return fn && fn(ref);
	}

	TESObjectREFR* ResolveTalkingActivatorActor(TESForm* baseForm)
	{
		if (!baseForm || baseForm->typeID != kFormType_TalkingActivator)
			return nullptr;

		return BGSTalkingActivatorGetTalkingActor(baseForm);
	}

	TESObjectREFR* ResolveBarterMerchant(TESObjectREFR* ref)
	{
		if (!ref || ref == PlayerCharacter::GetSingleton())
			return nullptr;

		TESObjectREFR* mobileRef = nullptr;
		TESObjectREFR* merchant = nullptr;
		if (CallRefPredicate(ref, kVtable_IsMobileObject))
		{
			mobileRef = ref;
		}
		else
		{
			merchant = ResolveTalkingActivatorActor(GetObjectReference(ref));
		}

		if (mobileRef)
		{
			if (CallRefPredicate(mobileRef, kVtable_IsActor))
			{
				merchant = mobileRef;
			}
			else if (IsTalkingThroughActivator(mobileRef))
			{
				auto* activatorRef = GetActivatorRef(mobileRef);
				merchant = activatorRef ? ResolveTalkingActivatorActor(GetObjectReference(activatorRef)) : nullptr;
			}
		}

		return merchant;
	}

	TESObjectREFR* GetMerchantRef(void* menu)
	{
		return BarterMenuGetMerchantRef(menu);
	}

	void* LeftItems(void* menu)
	{
		return BarterMenuGetLeftItems(menu);
	}

	void* CurrentItems(void* menu)
	{
		return BarterMenuGetCurrentItems(menu);
	}

	bool IsPlayerSideSelected(void* menu)
	{
		return menu && CurrentItems(menu) == LeftItems(menu);
	}

	void* EntryTile(void* list, ExtraContainerChanges::EntryData* entry)
	{
		if (!list || !entry)
			return nullptr;

		auto* lookupEntry = entry;
		return TileFromItem(list, &lookupEntry);
	}

	bool IsBarterSelectedTile(void* tile)
	{
		UInt32 trait = GetBarterMenuSelectedTrait();
		return tile && trait && TileIsFloatValueNotNull(tile, trait);
	}

	void* SelectedTile(void* menu)
	{
		if (!menu)
			return nullptr;

		auto* entry = static_cast<ExtraContainerChanges::EntryData*>(GetBarterMenuSelection());
		return EntryTile(CurrentItems(menu), entry);
	}

	bool IsSelectedItemAlreadyBartered(void* menu)
	{
		return IsBarterSelectedTile(SelectedTile(menu));
	}

	bool IsActive()
	{
		void* menu = GetBarterMenu();
		if (!s_filterList || !menu)
			return false;

		auto* merchant = GetMerchantRef(menu);
		return !s_merchantRefID || (merchant && merchant->refID == s_merchantRefID);
	}

	bool FormListContains(BGSListForm* list, TESForm* item, std::unordered_set<UInt32>& visited)
	{
		if (!list || !item)
			return false;
		if (!visited.insert(list->refID).second)
			return false;

		for (auto* node = list->list.Head(); node && node->Item(); node = node->Next())
		{
			auto* form = node->Item();
			if (form == item)
				return true;
			if (form && form->typeID == kFormType_ListForm &&
				FormListContains(static_cast<BGSListForm*>(form), item, visited))
				return true;
		}

		return false;
	}

	bool FilterListContains(TESForm* item)
	{
		std::unordered_set<UInt32> visited;
		return FormListContains(s_filterList, item, visited);
	}

	void* LeftSideTile(void* menu, ExtraContainerChanges::EntryData* entry)
	{
		return menu ? EntryTile(LeftItems(menu), entry) : nullptr;
	}

	bool ShouldBlockPlayerItem(TESForm* item)
	{
		if (!IsActive() || !item)
			return false;
		const bool inList = FilterListContains(item);
		return s_blacklistMode ? inList : !inList;
	}

	bool ShouldBlockSelectedTransfer()
	{
		void* menu = GetBarterMenu();
		if (!IsActive() || !IsPlayerSideSelected(menu))
			return false;
		if (IsSelectedItemAlreadyBartered(menu))
			return false;

		auto* entry = static_cast<ExtraContainerChanges::EntryData*>(GetBarterMenuSelection());
		return entry && ShouldBlockPlayerItem(entry->type);
	}

	bool HasBlockedQueuedSale(void* menu)
	{
		if (!IsActive() || !menu)
			return false;

		auto* node = reinterpret_cast<BarterItemNode*>(BarterMenuGetLeftBarter(menu));
		for (; node && node->item; node = node->next)
		{
			if (ShouldBlockPlayerItem(node->item->type))
				return true;
		}

		return false;
	}

	void NotifyBlockedSale()
	{
		CdeclCall<void>(kAddr_PlayMenuSound, 2);
		Console_Print(s_blacklistMode
			? "ShowBarterMenuBlacklist >> item is in blacklist"
			: "ShowBarterMenuWhitelist >> item is not in whitelist");
	}

	void SetActive(TESObjectREFR* merchant, BGSListForm* filterList, bool blacklistMode)
	{
		s_filterList = filterList;
		s_merchantRefID = merchant ? merchant->refID : 0;
		s_blacklistMode = blacklistMode;
	}

	void __cdecl Hook_Close()
	{
		s_origClose();
		if (!s_openingFilteredMenu)
			BarterCommands::ClearState();
	}

	bool __cdecl Hook_ShouldHideItem(ExtraContainerChanges::EntryData* entry)
	{
		const bool hidden = s_origShouldHideItem(entry);
		if (hidden || !IsActive())
			return hidden;

		void* menu = GetBarterMenu();
		auto* leftTile = LeftSideTile(menu, entry);
		if (!leftTile)
			return hidden;
		if (IsBarterSelectedTile(leftTile))
			return hidden;

		return ShouldBlockPlayerItem(entry ? entry->type : nullptr);
	}

	void __cdecl Hook_TransferItem(SInt32 count)
	{
		if (ShouldBlockSelectedTransfer())
		{
			NotifyBlockedSale();
			return;
		}

		s_origTransferItem(count);
	}

	void __fastcall Hook_ProcessTransaction(void* menu, void*)
	{
		if (HasBlockedQueuedSale(menu))
		{
			NotifyBlockedSale();
			return;
		}

		s_origProcessTransaction(menu);
	}

	static ParamInfo kParams_ShowBarterMenuWhitelist[2] = {
		{"whitelist", kParamType_AnyForm, 0},
		{"barterFlag", kParamType_Integer, 1},
	};

	static ParamInfo kParams_ShowBarterMenuBlacklist[2] = {
		{"blacklist", kParamType_AnyForm, 0},
		{"barterFlag", kParamType_Integer, 1},
	};

	bool ExecuteFilteredBarter(COMMAND_ARGS, bool blacklistMode)
	{
		*result = 0;

		TESForm* listForm = nullptr;
		UInt32 barterFlag = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &listForm, &barterFlag))
			return true;
		if (!s_hooksInstalled)
			return true;
		if (!thisObj || !listForm || listForm->typeID != kFormType_ListForm)
			return true;

		auto* merchant = ResolveBarterMerchant(thisObj);
		if (!merchant)
			return true;

		SetActive(nullptr, static_cast<BGSListForm*>(listForm), blacklistMode);
		s_openingFilteredMenu = true;
		void* menu = ShowBarterMenu(merchant, barterFlag);
		s_openingFilteredMenu = false;
		if (!menu)
		{
			BarterCommands::ClearState();
			return true;
		}

		auto* menuMerchant = GetMerchantRef(GetBarterMenu());
		s_merchantRefID = menuMerchant ? menuMerchant->refID : merchant->refID;

		*result = 1;
		return true;
	}
}

DEFINE_COMMAND_PLUGIN(ShowBarterMenuWhitelist,
	"shows barter menu while allowing the player to sell only items in a FormList",
	1, 2, kParams_ShowBarterMenuWhitelist);

DEFINE_COMMAND_PLUGIN(ShowBarterMenuBlacklist,
	"shows barter menu while preventing the player from selling items in a FormList",
	1, 2, kParams_ShowBarterMenuBlacklist);

bool Cmd_ShowBarterMenuWhitelist_Execute(COMMAND_ARGS)
{
	return ExecuteFilteredBarter(paramInfo, scriptData, thisObj, containingObj,
		scriptObj, eventList, result, opcodeOffsetPtr, false);
}

bool Cmd_ShowBarterMenuBlacklist_Execute(COMMAND_ARGS)
{
	return ExecuteFilteredBarter(paramInfo, scriptData, thisObj, containingObj,
		scriptObj, eventList, result, opcodeOffsetPtr, true);
}

namespace BarterCommands
{
	void RemoveHooks()
	{
		if (s_processCall.IsInstalled())
			s_processCall.Remove();
		if (s_transferDetour.IsInstalled())
			s_transferDetour.Remove();
		if (s_hideDetour.IsInstalled())
			s_hideDetour.Remove();
		if (s_closeDetour.IsInstalled())
			s_closeDetour.Remove();

		s_origClose = nullptr;
		s_origShouldHideItem = nullptr;
		s_origTransferItem = nullptr;
		s_origProcessTransaction = nullptr;
		s_hooksInstalled = false;
	}

	bool InitHooks()
	{
		//bail if another plugin already patched the prologue
		static const UInt8 kPrologue_Close[6]     = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 };
		static const UInt8 kPrologue_ShouldHide[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x30 };
		static const UInt8 kPrologue_Transfer[6]  = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24 };

		auto CheckPrologue = [](const char* name, UInt32 addr, const UInt8* expected, UInt32 size) -> bool {
			if (memcmp((void*)addr, expected, size) != 0)
			{
				Log("BarterCommands: %s prologue at 0x%X differs from expected, skipping (another plugin hooked here?)", name, addr);
				return false;
			}
			return true;
		};

		if (!CheckPrologue("BarterMenu::Close", kAddr_BarterMenu_Close, kPrologue_Close, 6))
			return false;
		if (!CheckPrologue("BarterMenu::ShouldHideItem", kAddr_BarterMenu_ShouldHideItem, kPrologue_ShouldHide, 6))
			return false;
		if (!CheckPrologue("BarterMenu::TransferItem", kAddr_BarterMenu_TransferItem, kPrologue_Transfer, 6))
			return false;

		if (s_closeDetour.WriteRelJump(kAddr_BarterMenu_Close, Hook_Close, 6))
		{
			s_origClose = s_closeDetour.GetTrampoline<BarterMenuClose_t>();
		}
		else
		{
			Log("BarterCommands: failed to hook BarterMenu::Close");
			return false;
		}

		if (s_hideDetour.WriteRelJump(kAddr_BarterMenu_ShouldHideItem, Hook_ShouldHideItem, 6))
		{
			s_origShouldHideItem = s_hideDetour.GetTrampoline<ShouldHideItem_t>();
		}
		else
		{
			RemoveHooks();
			Log("BarterCommands: failed to hook BarterMenu::ShouldHideItem");
			return false;
		}

		if (s_transferDetour.WriteRelJump(kAddr_BarterMenu_TransferItem, Hook_TransferItem, 6))
		{
			s_origTransferItem = s_transferDetour.GetTrampoline<TransferItem_t>();
		}
		else
		{
			RemoveHooks();
			Log("BarterCommands: failed to hook BarterMenu::TransferItem");
			return false;
		}

		if (s_processCall.WriteRelCall(kAddr_BarterMenu_CompleteTransactionCall, Hook_ProcessTransaction))
		{
			s_origProcessTransaction = (ProcessTransaction_t)s_processCall.GetOverwrittenAddr();
		}
		else
		{
			RemoveHooks();
			Log("BarterCommands: CompleteTransaction call site at 0x%X is not an E8 call", kAddr_BarterMenu_CompleteTransactionCall);
			return false;
		}

		s_hooksInstalled = true;
		return true;
	}

	void ClearState()
	{
		s_filterList = nullptr;
		s_merchantRefID = 0;
		s_openingFilteredMenu = false;
		s_blacklistMode = false;
	}

	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_ShowBarterMenuWhitelist);
		nvse->RegisterCommand(&kCommandInfo_ShowBarterMenuBlacklist);
	}
}
