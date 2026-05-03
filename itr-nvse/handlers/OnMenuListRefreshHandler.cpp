//fires after a menu's tile list rebuilds (Inventory, Container, Barter, Recipe)
//detours each menu's refresh function and dispatches ITR:OnMenuListRefresh on completion

#include "OnMenuListRefreshHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/globals.h"

namespace OnMenuListRefreshHandler {

constexpr UInt32 kAddr_InventoryMenu_UpdateList         = 0x782A90;
constexpr UInt32 kAddr_ContainerMenu_RefreshContainer   = 0x75C280;
constexpr UInt32 kAddr_BarterMenu_RefreshListbox        = 0x72DC30;
constexpr UInt32 kAddr_RecipeMenu_Refresh               = 0x727680;

constexpr UInt32 kMenuType_Inventory = 1002;
constexpr UInt32 kMenuType_Container = 1008;
constexpr UInt32 kMenuType_Barter    = 1053;
constexpr UInt32 kMenuType_Recipe    = 1077;

typedef void(__cdecl*   InventoryMenu_UpdateList_t)();
typedef void(__thiscall* ContainerMenu_Refresh_t)(void* this_, void* arg0);
typedef void(__thiscall* BarterMenu_Refresh_t)(void* this_, int arg0);
typedef void(__thiscall* RecipeMenu_Refresh_t)(void* this_, int arg1, int arg2);

static Detours::JumpDetour s_invDetour;
static Detours::JumpDetour s_contDetour;
static Detours::JumpDetour s_bartDetour;
static Detours::JumpDetour s_recipeDetour;

static InventoryMenu_UpdateList_t s_origInventoryUpdate = nullptr;
static ContainerMenu_Refresh_t    s_origContainerRefresh = nullptr;
static BarterMenu_Refresh_t       s_origBarterRefresh = nullptr;
static RecipeMenu_Refresh_t       s_origRecipeRefresh = nullptr;

static void Dispatch(UInt32 menuID) {
	if (g_eventManagerInterface)
		g_eventManagerInterface->DispatchEvent("ITR:OnMenuListRefresh", nullptr, (int)menuID);
}

static void __cdecl Hook_InventoryMenu_UpdateList() {
	if (s_origInventoryUpdate) s_origInventoryUpdate();
	Dispatch(kMenuType_Inventory);
}

static void __fastcall Hook_ContainerMenu_RefreshContainerMenu(void* this_, void* /*edx*/, void* arg0) {
	if (s_origContainerRefresh) s_origContainerRefresh(this_, arg0);
	Dispatch(kMenuType_Container);
}

static void __fastcall Hook_BarterMenu_RefreshListbox(void* this_, void* /*edx*/, int arg0) {
	if (s_origBarterRefresh) s_origBarterRefresh(this_, arg0);
	Dispatch(kMenuType_Barter);
}

static void __fastcall Hook_RecipeMenu_Refresh(void* this_, void* /*edx*/, int arg1, int arg2) {
	if (s_origRecipeRefresh) s_origRecipeRefresh(this_, arg1, arg2);
	Dispatch(kMenuType_Recipe);
}

bool Init(void* nvseInterface) {
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;

	bool allOk = true;

	if (s_invDetour.WriteRelJump(kAddr_InventoryMenu_UpdateList, Hook_InventoryMenu_UpdateList, 5)) {
		s_origInventoryUpdate = s_invDetour.GetTrampoline<InventoryMenu_UpdateList_t>();
		if (!s_origInventoryUpdate) { Log("OnMenuListRefreshHandler: inventory trampoline missing"); s_invDetour.Remove(); allOk = false; }
	} else {
		Log("OnMenuListRefreshHandler: failed to hook InventoryMenu::UpdateList");
		allOk = false;
	}

	if (s_contDetour.WriteRelJump(kAddr_ContainerMenu_RefreshContainer, Hook_ContainerMenu_RefreshContainerMenu, 5)) {
		s_origContainerRefresh = s_contDetour.GetTrampoline<ContainerMenu_Refresh_t>();
		if (!s_origContainerRefresh) { Log("OnMenuListRefreshHandler: container trampoline missing"); s_contDetour.Remove(); allOk = false; }
	} else {
		Log("OnMenuListRefreshHandler: failed to hook ContainerMenu::RefreshContainerMenu");
		allOk = false;
	}

	if (s_bartDetour.WriteRelJump(kAddr_BarterMenu_RefreshListbox, Hook_BarterMenu_RefreshListbox, 5)) {
		s_origBarterRefresh = s_bartDetour.GetTrampoline<BarterMenu_Refresh_t>();
		if (!s_origBarterRefresh) { Log("OnMenuListRefreshHandler: barter trampoline missing"); s_bartDetour.Remove(); allOk = false; }
	} else {
		Log("OnMenuListRefreshHandler: failed to hook BarterMenu::RefreshListbox");
		allOk = false;
	}

	if (s_recipeDetour.WriteRelJump(kAddr_RecipeMenu_Refresh, Hook_RecipeMenu_Refresh, 9)) {
		s_origRecipeRefresh = s_recipeDetour.GetTrampoline<RecipeMenu_Refresh_t>();
		if (!s_origRecipeRefresh) { Log("OnMenuListRefreshHandler: recipe trampoline missing"); s_recipeDetour.Remove(); allOk = false; }
	} else {
		Log("OnMenuListRefreshHandler: failed to hook RecipeMenu::Refresh");
		allOk = false;
	}

	return allOk;
}

}
