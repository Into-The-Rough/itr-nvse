#include "BarterCommands.h"
#include "internal/Detours.h"
#include "internal/CallTemplates.h"
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
	constexpr UInt32 kAddr_BarterMenu_ProcessTransaction = 0x72FD10;
	constexpr UInt32 kAddr_MenuItemsList_TileFromItem = 0x7A22D0;
	constexpr UInt32 kAddr_TESObjectREFR_GetObjectReference = 0x7AF430;
	constexpr UInt32 kAddr_BGSTalkingActivator_GetActor = 0x516BF0;
	constexpr UInt32 kAddr_MobileObject_IsTalkingThroughActivator = 0x574900;
	constexpr UInt32 kAddr_MobileObject_GetActivatorRef = 0x5E3FA0;
	constexpr UInt32 kAddr_Tile_IsFloatValueNotNull = 0xA01230;

	constexpr UInt32 kVtable_IsMobileObject = 0xFC;
	constexpr UInt32 kVtable_IsActor = 0x100;

	constexpr UInt32 kOffset_MerchantRef = 0x80;
	constexpr UInt32 kOffset_LeftItems = 0xA8;
	constexpr UInt32 kOffset_CurrentItems = 0x108;
	constexpr UInt32 kOffset_LeftBarter = 0x10C;

	using ShowBarterMenu_t = void* (__cdecl*)(TESObjectREFR*, SInt32);
	using BarterMenuClose_t = void(__cdecl*)();
	using ShouldHideItem_t = bool(__cdecl*)(ExtraContainerChanges::EntryData*);
	using TransferItem_t = void(__cdecl*)(SInt32);
	using ProcessTransaction_t = void* (__thiscall*)(void*);
	using TileFromItem_t = void* (__thiscall*)(void*, ExtraContainerChanges::EntryData**);
	using TESObjectREFR_GetObjectReference_t = TESForm* (__thiscall*)(TESObjectREFR*);
	using TalkingActivatorGetActor_t = TESObjectREFR* (__thiscall*)(TESForm*);
	using IsTalkingThroughActivator_t = bool(__thiscall*)(TESObjectREFR*);
	using GetActivatorRef_t = TESObjectREFR* (__thiscall*)(TESObjectREFR*);
	using TileIsFloatValueNotNull_t = bool(__thiscall*)(void*, UInt32);
	using RefPredicate_t = bool(__thiscall*)(TESObjectREFR*);

	const auto ShowBarterMenu = reinterpret_cast<ShowBarterMenu_t>(kAddr_ShowBarterMenu);
	const auto TileFromItem = reinterpret_cast<TileFromItem_t>(kAddr_MenuItemsList_TileFromItem);
	const auto GetObjectReference =
		reinterpret_cast<TESObjectREFR_GetObjectReference_t>(kAddr_TESObjectREFR_GetObjectReference);
	const auto GetTalkingActivatorActor =
		reinterpret_cast<TalkingActivatorGetActor_t>(kAddr_BGSTalkingActivator_GetActor);
	const auto IsTalkingThroughActivator =
		reinterpret_cast<IsTalkingThroughActivator_t>(kAddr_MobileObject_IsTalkingThroughActivator);
	const auto GetActivatorRef = reinterpret_cast<GetActivatorRef_t>(kAddr_MobileObject_GetActivatorRef);
	const auto TileIsFloatValueNotNull =
		reinterpret_cast<TileIsFloatValueNotNull_t>(kAddr_Tile_IsFloatValueNotNull);
	void** g_barterMenu = reinterpret_cast<void**>(0x11D8FA4);
	ExtraContainerChanges::EntryData** g_barterMenuSelection =
		reinterpret_cast<ExtraContainerChanges::EntryData**>(0x11D8FA8);
	UInt32* g_barterMenuTraitIsBarterSelected = reinterpret_cast<UInt32*>(0x11D8FB4);

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
	Detours::JumpDetour s_processDetour;

	BarterMenuClose_t s_origClose = nullptr;
	ShouldHideItem_t s_origShouldHideItem = nullptr;
	TransferItem_t s_origTransferItem = nullptr;
	ProcessTransaction_t s_origProcessTransaction = nullptr;

	BGSListForm* s_whitelist = nullptr;
	UInt32 s_merchantRefID = 0;
	bool s_hooksInstalled = false;
	bool s_openingWhitelistedMenu = false;

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

		return GetTalkingActivatorActor(baseForm);
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

	void* GetBarterMenu()
	{
		return g_barterMenu ? *g_barterMenu : nullptr;
	}

	TESObjectREFR* GetMerchantRef(void* menu)
	{
		return menu ? *reinterpret_cast<TESObjectREFR**>(reinterpret_cast<UInt8*>(menu) + kOffset_MerchantRef) : nullptr;
	}

	void* LeftItems(void* menu)
	{
		return menu ? reinterpret_cast<UInt8*>(menu) + kOffset_LeftItems : nullptr;
	}

	void* CurrentItems(void* menu)
	{
		return menu ? *reinterpret_cast<void**>(reinterpret_cast<UInt8*>(menu) + kOffset_CurrentItems) : nullptr;
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
		return tile && g_barterMenuTraitIsBarterSelected &&
			TileIsFloatValueNotNull(tile, *g_barterMenuTraitIsBarterSelected);
	}

	void* SelectedTile(void* menu)
	{
		if (!menu || !g_barterMenuSelection)
			return nullptr;

		return EntryTile(CurrentItems(menu), *g_barterMenuSelection);
	}

	bool IsSelectedItemAlreadyBartered(void* menu)
	{
		return IsBarterSelectedTile(SelectedTile(menu));
	}

	bool IsActive()
	{
		void* menu = GetBarterMenu();
		if (!s_whitelist || !menu)
			return false;

		auto* merchant = GetMerchantRef(menu);
		return !s_merchantRefID || (merchant && merchant->refID == s_merchantRefID);
	}

	bool SameForm(TESForm* lhs, TESForm* rhs)
	{
		return lhs && rhs && lhs == rhs;
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
			if (SameForm(form, item))
				return true;
			if (form && form->typeID == kFormType_ListForm &&
				FormListContains(static_cast<BGSListForm*>(form), item, visited))
				return true;
		}

		return false;
	}

	bool WhitelistContains(TESForm* item)
	{
		std::unordered_set<UInt32> visited;
		return FormListContains(s_whitelist, item, visited);
	}

	void* LeftSideTile(void* menu, ExtraContainerChanges::EntryData* entry)
	{
		return menu ? EntryTile(LeftItems(menu), entry) : nullptr;
	}

	bool ShouldBlockPlayerItem(TESForm* item)
	{
		return IsActive() && item && !WhitelistContains(item);
	}

	bool ShouldBlockSelectedTransfer()
	{
		void* menu = GetBarterMenu();
		if (!IsActive() || !IsPlayerSideSelected(menu) || !g_barterMenuSelection)
			return false;
		if (IsSelectedItemAlreadyBartered(menu))
			return false;

		auto* entry = *g_barterMenuSelection;
		return entry && ShouldBlockPlayerItem(entry->type);
	}

	bool HasBlockedQueuedSale(void* menu)
	{
		if (!IsActive() || !menu)
			return false;

		auto* node = reinterpret_cast<BarterItemNode*>(reinterpret_cast<UInt8*>(menu) + kOffset_LeftBarter);
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
		Console_Print("ShowBarterMenuWhitelist >> item is not in whitelist");
	}

	void SetActive(TESObjectREFR* merchant, BGSListForm* whitelist)
	{
		s_whitelist = whitelist;
		s_merchantRefID = merchant ? merchant->refID : 0;
	}

	void __cdecl Hook_Close()
	{
		if (s_origClose)
			s_origClose();
		if (!s_openingWhitelistedMenu)
			BarterCommands::ClearState();
	}

	bool __cdecl Hook_ShouldHideItem(ExtraContainerChanges::EntryData* entry)
	{
		const bool hidden = s_origShouldHideItem ? s_origShouldHideItem(entry) : false;
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

		if (s_origTransferItem)
			s_origTransferItem(count);
	}

	void* __fastcall Hook_ProcessTransaction(void* menu, void*)
	{
		if (HasBlockedQueuedSale(menu))
		{
			NotifyBlockedSale();
			return menu;
		}

		return s_origProcessTransaction ? s_origProcessTransaction(menu) : menu;
	}

	static ParamInfo kParams_ShowBarterMenuWhitelist[2] = {
		{"whitelist", kParamType_AnyForm, 0},
		{"barterFlag", kParamType_Integer, 1},
	};
}

DEFINE_COMMAND_PLUGIN(ShowBarterMenuWhitelist,
	"shows barter menu while allowing the player to sell only items in a FormList",
	1, 2, kParams_ShowBarterMenuWhitelist);

bool Cmd_ShowBarterMenuWhitelist_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESForm* whitelistForm = nullptr;
	UInt32 barterFlag = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &whitelistForm, &barterFlag))
		return true;
	if (!s_hooksInstalled)
		return true;
	if (!thisObj || !whitelistForm || whitelistForm->typeID != kFormType_ListForm)
		return true;

	auto* merchant = ResolveBarterMerchant(thisObj);
	if (!merchant)
		return true;

	SetActive(nullptr, static_cast<BGSListForm*>(whitelistForm));
	s_openingWhitelistedMenu = true;
	void* menu = ShowBarterMenu(merchant, barterFlag);
	s_openingWhitelistedMenu = false;
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

namespace BarterCommands
{
	void RemoveHooks()
	{
		if (s_processDetour.IsInstalled())
			s_processDetour.Remove();
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

		if (s_processDetour.WriteRelJump(kAddr_BarterMenu_ProcessTransaction, Hook_ProcessTransaction, 10))
		{
			s_origProcessTransaction = s_processDetour.GetTrampoline<ProcessTransaction_t>();
		}
		else
		{
			RemoveHooks();
			Log("BarterCommands: failed to hook BarterMenu::ProcessTransaction");
			return false;
		}

		s_hooksInstalled = true;
		return true;
	}

	void ClearState()
	{
		s_whitelist = nullptr;
		s_merchantRefID = 0;
		s_openingWhitelistedMenu = false;
	}

	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_ShowBarterMenuWhitelist);
	}
}
