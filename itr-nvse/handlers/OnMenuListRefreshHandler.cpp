//fires after a menu's tile list rebuilds (Inventory, Container, Barter, Recipe)
//detours each menu's refresh function and dispatches ITR:OnMenuListRefresh on completion

#include "OnMenuListRefreshHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/globals.h"

namespace OnMenuListRefreshHandler {

	constexpr UInt32 kAddr_InventoryMenu_UpdateList = 0x782A90;
	constexpr UInt32 kAddr_ContainerMenu_RefreshContainer = 0x75C280;
	constexpr UInt32 kAddr_BarterMenu_RefreshListbox = 0x72DC30;
	constexpr UInt32 kAddr_RecipeMenu_Refresh = 0x727680;

	constexpr UInt32 kMenuType_Inventory = 1002;
	constexpr UInt32 kMenuType_Container = 1008;
	constexpr UInt32 kMenuType_Barter = 1053;
	constexpr UInt32 kMenuType_Recipe = 1077;

	using InventoryMenu_UpdateList_t = void(__cdecl*)();
	using ContainerMenu_Refresh_t = void(__thiscall*)(void* this_, void* arg0);
	using BarterMenu_Refresh_t = void(__thiscall*)(void* this_, int arg0);
	using RecipeMenu_Refresh_t = void(__thiscall*)(void* this_, int arg1, int arg2);

	static Detours::JumpDetour s_invDetour;
	static Detours::JumpDetour s_contDetour;
	static Detours::JumpDetour s_bartDetour;
	static Detours::JumpDetour s_recipeDetour;

	static InventoryMenu_UpdateList_t s_origInventoryUpdate = nullptr;
	static ContainerMenu_Refresh_t s_origContainerRefresh = nullptr;
	static BarterMenu_Refresh_t s_origBarterRefresh = nullptr;
	static RecipeMenu_Refresh_t s_origRecipeRefresh = nullptr;

	static DWORD s_mainThreadId = 0;

	static void Dispatch(UInt32 menuID)
	{
		if (!g_eventManagerInterface) return;
		//a handler mutating inventory retriggers the rebuild we dispatched from, which
		//would recurse unbounded, so skip dispatch while one is already running
		static thread_local bool s_inDispatch = false;
		if (s_inDispatch) return;
		s_inDispatch = true;
		//these menu rebuilds can be pumped on an AI task thread when another mod routes the
		//UI pump off-main. plain DispatchEvent races NVSE's handler list and runs handler
		//scripts off-thread, so defer to the main thread there.
		if (GetCurrentThreadId() == s_mainThreadId)
			g_eventManagerInterface->DispatchEvent("ITR:OnMenuListRefresh", nullptr, (int)menuID);
		else if (g_eventManagerInterface->DispatchEventThreadSafe)
			g_eventManagerInterface->DispatchEventThreadSafe("ITR:OnMenuListRefresh", nullptr, nullptr, (int)menuID);
		s_inDispatch = false;
	}

	static void __cdecl Hook_InventoryMenu_UpdateList()
	{
		s_origInventoryUpdate();
		Dispatch(kMenuType_Inventory);
	}

	static void __fastcall Hook_ContainerMenu_RefreshContainerMenu(void* this_, void* edx, void* arg0)
	{
		s_origContainerRefresh(this_, arg0);
		Dispatch(kMenuType_Container);
	}

	static void __fastcall Hook_BarterMenu_RefreshListbox(void* this_, void* edx, int arg0)
	{
		s_origBarterRefresh(this_, arg0);
		Dispatch(kMenuType_Barter);
	}

	static void __fastcall Hook_RecipeMenu_Refresh(void* this_, void* edx, int arg1, int arg2)
	{
		s_origRecipeRefresh(this_, arg1, arg2);
		Dispatch(kMenuType_Recipe);
	}

	bool Init(void* nvseInterface)
	{
		NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
		if (nvse->isEditor) return false;

		s_mainThreadId = GetCurrentThreadId();

		return s_invDetour.WriteRelJump(kAddr_InventoryMenu_UpdateList, Hook_InventoryMenu_UpdateList, 5, reinterpret_cast<UInt8**>(&s_origInventoryUpdate))
			&& s_contDetour.WriteRelJump(kAddr_ContainerMenu_RefreshContainer, Hook_ContainerMenu_RefreshContainerMenu, 5, reinterpret_cast<UInt8**>(&s_origContainerRefresh))
			&& s_bartDetour.WriteRelJump(kAddr_BarterMenu_RefreshListbox, Hook_BarterMenu_RefreshListbox, 5, reinterpret_cast<UInt8**>(&s_origBarterRefresh))
			&& s_recipeDetour.WriteRelJump(kAddr_RecipeMenu_Refresh, Hook_RecipeMenu_Refresh, 9, reinterpret_cast<UInt8**>(&s_origRecipeRefresh));
	}
}
