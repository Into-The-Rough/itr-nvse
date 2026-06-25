//fires when menu filter/category changes (Inventory, Container, Barter, Recipe menus)
//uses polling to avoid hook conflicts with JIP NVSE

#include "OnMenuFilterChangeHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"
#include "internal/GameGlobals.h"
#include "internal/MenuLayout.h"

constexpr UInt32 kMenuType_Container = 1008;
constexpr UInt32 kMenuType_Barter = 1053;
constexpr UInt32 kMenuType_Recipe = 1077;

static UInt32 g_lastInventoryFilter = 0xFFFFFFFF;
static UInt32 g_lastContainerLeftFilter = 0xFFFFFFFF;
static UInt32 g_lastContainerRightFilter = 0xFFFFFFFF;
static UInt32 g_lastBarterLeftFilter = 0xFFFFFFFF;
static UInt32 g_lastBarterRightFilter = 0xFFFFFFFF;
static void* g_lastInventoryMenu = nullptr;
static void* g_lastContainerMenu = nullptr;
static void* g_lastBarterMenu = nullptr;
static int g_lastRecipeCategory = -2; //-2=uninitialized, -1=All, 0..N=category index
static void* g_lastRecipeMenu = nullptr;

static void DispatchFilterChangeEvent(UInt32 menuID, UInt32 oldFilter, UInt32 newFilter, UInt32 side) {
    if (oldFilter == newFilter) return;

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnMenuFilterChange", nullptr,
            (int)menuID, (int)oldFilter, (int)newFilter, (int)side);
}

namespace OnMenuFilterChangeHandler {
void Update() {
    if (!g_eventManagerInterface) return;

    void* invMenu = GetInventoryMenu();
    if (invMenu) {
        if (invMenu != g_lastInventoryMenu) {
            g_lastInventoryFilter = InventoryMenuGetFilter(invMenu);
            g_lastInventoryMenu = invMenu;
        } else {
            UInt32 currentFilter = InventoryMenuGetFilter(invMenu);
            if (currentFilter != g_lastInventoryFilter && g_lastInventoryFilter != 0xFFFFFFFF)
                DispatchFilterChangeEvent(1002, g_lastInventoryFilter, currentFilter, 0);
            g_lastInventoryFilter = currentFilter;
        }
    } else {
        g_lastInventoryMenu = nullptr;
        g_lastInventoryFilter = 0xFFFFFFFF;
    }

    void* contMenu = GetContainerMenu();
    if (contMenu) {
        if (contMenu != g_lastContainerMenu) {
            g_lastContainerLeftFilter = ContainerMenuGetLeftFilter(contMenu);
            g_lastContainerRightFilter = ContainerMenuGetRightFilter(contMenu);
            g_lastContainerMenu = contMenu;
        } else {
            UInt32 leftFilter = ContainerMenuGetLeftFilter(contMenu);
            UInt32 rightFilter = ContainerMenuGetRightFilter(contMenu);
            if (leftFilter != g_lastContainerLeftFilter && g_lastContainerLeftFilter != 0xFFFFFFFF)
                DispatchFilterChangeEvent(kMenuType_Container, g_lastContainerLeftFilter, leftFilter, 0);
            if (rightFilter != g_lastContainerRightFilter && g_lastContainerRightFilter != 0xFFFFFFFF)
                DispatchFilterChangeEvent(kMenuType_Container, g_lastContainerRightFilter, rightFilter, 1);
            g_lastContainerLeftFilter = leftFilter;
            g_lastContainerRightFilter = rightFilter;
        }
    } else {
        g_lastContainerMenu = nullptr;
        g_lastContainerLeftFilter = 0xFFFFFFFF;
        g_lastContainerRightFilter = 0xFFFFFFFF;
    }

    void* bartMenu = GetBarterMenu();
    if (bartMenu) {
        if (bartMenu != g_lastBarterMenu) {
            g_lastBarterLeftFilter = BarterMenuGetLeftFilter(bartMenu);
            g_lastBarterRightFilter = BarterMenuGetRightFilter(bartMenu);
            g_lastBarterMenu = bartMenu;
        } else {
            UInt32 leftFilter = BarterMenuGetLeftFilter(bartMenu);
            UInt32 rightFilter = BarterMenuGetRightFilter(bartMenu);
            if (leftFilter != g_lastBarterLeftFilter && g_lastBarterLeftFilter != 0xFFFFFFFF)
                DispatchFilterChangeEvent(kMenuType_Barter, g_lastBarterLeftFilter, leftFilter, 0);
            if (rightFilter != g_lastBarterRightFilter && g_lastBarterRightFilter != 0xFFFFFFFF)
                DispatchFilterChangeEvent(kMenuType_Barter, g_lastBarterRightFilter, rightFilter, 1);
            g_lastBarterLeftFilter = leftFilter;
            g_lastBarterRightFilter = rightFilter;
        }
    } else {
        g_lastBarterMenu = nullptr;
        g_lastBarterLeftFilter = 0xFFFFFFFF;
        g_lastBarterRightFilter = 0xFFFFFFFF;
    }

    void* recipeMenu = GetRecipeMenu();
    if (recipeMenu) {
        int currentCategory = GetRecipeMenuCategory();
        if (recipeMenu != g_lastRecipeMenu) {
            g_lastRecipeCategory = currentCategory;
            g_lastRecipeMenu = recipeMenu;
        } else {
            if (currentCategory != g_lastRecipeCategory && g_lastRecipeCategory != -2)
                DispatchFilterChangeEvent(kMenuType_Recipe, (UInt32)g_lastRecipeCategory, (UInt32)currentCategory, 0);
            g_lastRecipeCategory = currentCategory;
        }
    } else {
        g_lastRecipeMenu = nullptr;
        g_lastRecipeCategory = -2;
    }
}

bool Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;
    return true;
}
}
