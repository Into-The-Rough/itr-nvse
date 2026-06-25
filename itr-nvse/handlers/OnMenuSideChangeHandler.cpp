//fires when active side changes in Container/Barter menus (LT/RT, left/right arrows, or clicking)
//uses polling to avoid hook conflicts with JIP NVSE

#include "OnMenuSideChangeHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"
#include "internal/GameGlobals.h"
#include "internal/MenuLayout.h"

constexpr UInt32 kMenuType_Container = 1008;
constexpr UInt32 kMenuType_Barter = 1053;

static UInt32 g_lastContainerSide = 0xFFFFFFFF;
static UInt32 g_lastBarterSide = 0xFFFFFFFF;
static void* g_lastContainerMenu = nullptr;
static void* g_lastBarterMenu = nullptr;

static UInt32 GetCurrentSide(void* menu, UInt32 menuType) {
    return (menuType == kMenuType_Container) ? ContainerMenuGetCurrentSide(menu) : BarterMenuGetCurrentSide(menu);
}

static void DispatchSideChangeEvent(UInt32 menuID, UInt32 oldSide, UInt32 newSide) {
    if (oldSide == newSide) return;

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnMenuSideChange", nullptr,
            (int)menuID, (int)oldSide, (int)newSide);
}

namespace OnMenuSideChangeHandler {
void Update() {
    if (!g_eventManagerInterface) return;

    void* contMenu = GetContainerMenu();
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

    void* bartMenu = GetBarterMenu();
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

bool Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;
    return true;
}
}
