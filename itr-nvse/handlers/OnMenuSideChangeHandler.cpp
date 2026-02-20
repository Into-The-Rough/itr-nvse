//fires when active side changes in Container/Barter menus (LT/RT, left/right arrows, or clicking)
//uses polling to avoid hook conflicts with JIP NVSE

#include "OnMenuSideChangeHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"

constexpr UInt32 kMenuType_Container = 1008;
constexpr UInt32 kMenuType_Barter = 1053;

static UInt32 g_lastContainerSide = 0xFFFFFFFF;
static UInt32 g_lastBarterSide = 0xFFFFFFFF;
static void* g_lastContainerMenu = nullptr;
static void* g_lastBarterMenu = nullptr;

static UInt32 GetCurrentSide(void* menu, UInt32 menuType) {
    UInt32 activeListOffset = (menuType == kMenuType_Container) ? 0xF8 : 0x108;
    UInt32 leftListOffset = (menuType == kMenuType_Container) ? 0x98 : 0xA8;

    UInt32 activeList = *(UInt32*)((UInt8*)menu + activeListOffset);
    UInt32 leftList = (UInt32)menu + leftListOffset;

    return (activeList == leftList) ? 0 : 1;
}

static void DispatchSideChangeEvent(UInt32 menuID, UInt32 oldSide, UInt32 newSide) {
    if (oldSide == newSide) return;

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnMenuSideChange", nullptr,
            (int)menuID, (int)oldSide, (int)newSide);
}

void OMSCH_Update() {
    if (!g_eventManagerInterface) return;

    void* contMenu = *(void**)0x11D93F8;
    if (contMenu) {
        if (contMenu != g_lastContainerMenu) {
            g_lastContainerSide = GetCurrentSide(contMenu, kMenuType_Container);
            g_lastContainerMenu = contMenu;
        } else {
            UInt32 currentSide = GetCurrentSide(contMenu, kMenuType_Container);
            if (currentSide != g_lastContainerSide && g_lastContainerSide != 0xFFFFFFFF)
                DispatchSideChangeEvent(kMenuType_Container, g_lastContainerSide, currentSide);
            g_lastContainerSide = currentSide;
        }
    } else {
        g_lastContainerMenu = nullptr;
        g_lastContainerSide = 0xFFFFFFFF;
    }

    void* bartMenu = *(void**)0x11D8FA4;
    if (bartMenu) {
        if (bartMenu != g_lastBarterMenu) {
            g_lastBarterSide = GetCurrentSide(bartMenu, kMenuType_Barter);
            g_lastBarterMenu = bartMenu;
        } else {
            UInt32 currentSide = GetCurrentSide(bartMenu, kMenuType_Barter);
            if (currentSide != g_lastBarterSide && g_lastBarterSide != 0xFFFFFFFF)
                DispatchSideChangeEvent(kMenuType_Barter, g_lastBarterSide, currentSide);
            g_lastBarterSide = currentSide;
        }
    } else {
        g_lastBarterMenu = nullptr;
        g_lastBarterSide = 0xFFFFFFFF;
    }
}

bool OMSCH_Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;
    return true;
}
