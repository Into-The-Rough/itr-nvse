//fires when menu filter/category changes (Inventory, Container, Barter menus)
//uses polling to avoid hook conflicts with JIP NVSE

#include "OnMenuFilterChangeHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"

constexpr UInt32 kMenuType_Container = 1008;
constexpr UInt32 kMenuType_Barter = 1053;

constexpr UInt32 kOffset_InventoryMenu_Filter = 0x84;
constexpr UInt32 kOffset_ContainerMenu_LeftFilter = 0x8C;
constexpr UInt32 kOffset_ContainerMenu_RightFilter = 0x90;
constexpr UInt32 kOffset_BarterMenu_LeftFilter = 0x9C;
constexpr UInt32 kOffset_BarterMenu_RightFilter = 0xA0;

static UInt32 g_lastInventoryFilter = 0xFFFFFFFF;
static UInt32 g_lastContainerLeftFilter = 0xFFFFFFFF;
static UInt32 g_lastContainerRightFilter = 0xFFFFFFFF;
static UInt32 g_lastBarterLeftFilter = 0xFFFFFFFF;
static UInt32 g_lastBarterRightFilter = 0xFFFFFFFF;
static void* g_lastInventoryMenu = nullptr;
static void* g_lastContainerMenu = nullptr;
static void* g_lastBarterMenu = nullptr;

static void DispatchFilterChangeEvent(UInt32 menuID, UInt32 oldFilter, UInt32 newFilter, UInt32 side) {
    if (oldFilter == newFilter) return;

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnMenuFilterChange", nullptr,
            (int)menuID, (int)oldFilter, (int)newFilter, (int)side);
}

void OMFCH_Update() {
    if (!g_eventManagerInterface) return;

    void* invMenu = *(void**)0x11D9EA4;
    if (invMenu) {
        if (invMenu != g_lastInventoryMenu) {
            g_lastInventoryFilter = *(UInt32*)((UInt8*)invMenu + kOffset_InventoryMenu_Filter);
            g_lastInventoryMenu = invMenu;
        } else {
            UInt32 currentFilter = *(UInt32*)((UInt8*)invMenu + kOffset_InventoryMenu_Filter);
            if (currentFilter != g_lastInventoryFilter && g_lastInventoryFilter != 0xFFFFFFFF)
                DispatchFilterChangeEvent(1002, g_lastInventoryFilter, currentFilter, 0);
            g_lastInventoryFilter = currentFilter;
        }
    } else {
        g_lastInventoryMenu = nullptr;
        g_lastInventoryFilter = 0xFFFFFFFF;
    }

    void* contMenu = *(void**)0x11D93F8;
    if (contMenu) {
        if (contMenu != g_lastContainerMenu) {
            g_lastContainerLeftFilter = *(UInt32*)((UInt8*)contMenu + kOffset_ContainerMenu_LeftFilter);
            g_lastContainerRightFilter = *(UInt32*)((UInt8*)contMenu + kOffset_ContainerMenu_RightFilter);
            g_lastContainerMenu = contMenu;
        } else {
            UInt32 leftFilter = *(UInt32*)((UInt8*)contMenu + kOffset_ContainerMenu_LeftFilter);
            UInt32 rightFilter = *(UInt32*)((UInt8*)contMenu + kOffset_ContainerMenu_RightFilter);
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

    void* bartMenu = *(void**)0x11D8FA4;
    if (bartMenu) {
        if (bartMenu != g_lastBarterMenu) {
            g_lastBarterLeftFilter = *(UInt32*)((UInt8*)bartMenu + kOffset_BarterMenu_LeftFilter);
            g_lastBarterRightFilter = *(UInt32*)((UInt8*)bartMenu + kOffset_BarterMenu_RightFilter);
            g_lastBarterMenu = bartMenu;
        } else {
            UInt32 leftFilter = *(UInt32*)((UInt8*)bartMenu + kOffset_BarterMenu_LeftFilter);
            UInt32 rightFilter = *(UInt32*)((UInt8*)bartMenu + kOffset_BarterMenu_RightFilter);
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
}

bool OMFCH_Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;
    return true;
}
