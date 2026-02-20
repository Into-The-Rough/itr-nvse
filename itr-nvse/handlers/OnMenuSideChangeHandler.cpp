//fires when active side changes in Container/Barter menus (LT/RT, left/right arrows, or clicking)
//uses polling to avoid hook conflicts with JIP NVSE

#include <vector>
#include <Windows.h>

#include "OnMenuSideChangeHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"

constexpr UInt32 kMenuType_Container = 1008;
constexpr UInt32 kMenuType_Barter = 1053;

static NVSEScriptInterface* g_omschScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_registeredOpcode = 0;

struct SideChangeCallback {
    Script* script;
    UInt32 menuFilter;  //0 = all menus, otherwise specific menuID
};

static std::vector<SideChangeCallback> g_callbacks;

//cached side values for polling
static UInt32 g_lastContainerSide = 0xFFFFFFFF;
static UInt32 g_lastBarterSide = 0xFFFFFFFF;
static void* g_lastContainerMenu = nullptr;
static void* g_lastBarterMenu = nullptr;

static UInt32 GetCurrentSide(void* menu, UInt32 menuType) {
    UInt32 activeListOffset = (menuType == kMenuType_Container) ? 0xF8 : 0x108; //activeList
    UInt32 leftListOffset = (menuType == kMenuType_Container) ? 0x98 : 0xA8; //leftList

    UInt32 activeList = *(UInt32*)((UInt8*)menu + activeListOffset);
    UInt32 leftList = (UInt32)menu + leftListOffset;

    return (activeList == leftList) ? 0 : 1;  //0 = left, 1 = right
}

static void DispatchSideChangeEvent(UInt32 menuID, UInt32 oldSide, UInt32 newSide) {
    if (oldSide == newSide) return;

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnMenuSideChange", nullptr,
            (int)menuID, (int)oldSide, (int)newSide);

    if (!g_omschScript || g_callbacks.empty()) return;

    const auto snapshot = g_callbacks;

    for (const auto& cb : snapshot) {
        if (cb.menuFilter == 0 || cb.menuFilter == menuID) {
            if (cb.script) {
                g_omschScript->CallFunctionAlt(cb.script, nullptr, 3, menuID, oldSide, newSide);
            }
        }
    }
}

void OMSCH_Update() {
    if (g_callbacks.empty() && !g_eventManagerInterface) return;

    //check container menu
    void* contMenu = *(void**)0x11D93F8; //ContainerMenu
    if (contMenu) {
        if (contMenu != g_lastContainerMenu) {
            g_lastContainerSide = GetCurrentSide(contMenu, kMenuType_Container);
            g_lastContainerMenu = contMenu;
        } else {
            UInt32 currentSide = GetCurrentSide(contMenu, kMenuType_Container);
            if (currentSide != g_lastContainerSide && g_lastContainerSide != 0xFFFFFFFF) {
                DispatchSideChangeEvent(kMenuType_Container, g_lastContainerSide, currentSide);
            }
            g_lastContainerSide = currentSide;
        }
    } else {
        g_lastContainerMenu = nullptr;
        g_lastContainerSide = 0xFFFFFFFF;
    }

    //check barter menu
    void* bartMenu = *(void**)0x11D8FA4; //BarterMenu
    if (bartMenu) {
        if (bartMenu != g_lastBarterMenu) {
            g_lastBarterSide = GetCurrentSide(bartMenu, kMenuType_Barter);
            g_lastBarterMenu = bartMenu;
        } else {
            UInt32 currentSide = GetCurrentSide(bartMenu, kMenuType_Barter);
            if (currentSide != g_lastBarterSide && g_lastBarterSide != 0xFFFFFFFF) {
                DispatchSideChangeEvent(kMenuType_Barter, g_lastBarterSide, currentSide);
            }
            g_lastBarterSide = currentSide;
        }
    } else {
        g_lastBarterMenu = nullptr;
        g_lastBarterSide = 0xFFFFFFFF;
    }
}

static ParamInfo kParams_SetOnMenuSideChangeEventHandler[3] = {
    {"setOrRemove", kParamType_Integer, 0},
    {"handler",     kParamType_AnyForm, 0},
    {"menuFilter",  kParamType_Integer, 1},  //optional: 0=all, 1008=Container, 1053=Barter
};

DEFINE_COMMAND_PLUGIN(SetOnMenuSideChangeEventHandler,
    "Registers/unregisters callback for menu side change events. Args: menuID, oldSide, newSide (0=left, 1=right)",
    0, 3, kParams_SetOnMenuSideChangeEventHandler);

bool Cmd_SetOnMenuSideChangeEventHandler_Execute(COMMAND_ARGS) {
    *result = 0;

    UInt32 setOrRemove = 0;
    TESForm* handlerForm = nullptr;
    UInt32 menuFilter = 0;

    if (!g_ExtractArgsEx((ParamInfo*)paramInfo, scriptData, opcodeOffsetPtr,
            scriptObj, eventList, &setOrRemove, &handlerForm, &menuFilter)) {
        return true;
    }

    if (!handlerForm || *((UInt8*)handlerForm + 4) != kFormType_Script) {
        return true;
    }

    Script* script = (Script*)handlerForm;

    if (setOrRemove) {
        bool found = false;
        for (const auto& cb : g_callbacks) {
            if (cb.script == script && cb.menuFilter == menuFilter) {
                found = true;
                break;
            }
        }
        if (!found) {
            g_callbacks.push_back({script, menuFilter});
        }
        *result = 1;
    } else {
        for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ++it) {
            if (it->script == script && it->menuFilter == menuFilter) {
                g_callbacks.erase(it);
                *result = 1;
                break;
            }
        }
    }

    return true;
}

unsigned int OMSCH_GetOpcode() {
    return g_registeredOpcode;
}

bool OMSCH_Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    g_omschScript = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (!g_omschScript) {
        return false;
    }
    g_ExtractArgsEx = g_omschScript->ExtractArgsEx;

    //no hooks needed - uses polling via OMSCH_Update()

    nvse->SetOpcodeBase(0x4033);
    nvse->RegisterCommand(&kCommandInfo_SetOnMenuSideChangeEventHandler);
    g_registeredOpcode = 0x4033;

    return true;
}

void OMSCH_ClearCallbacks()
{
    g_callbacks.clear();
}
