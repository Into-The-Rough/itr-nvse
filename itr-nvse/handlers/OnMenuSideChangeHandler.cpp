//fires when active side changes in Container/Barter menus (LT/RT, left/right arrows, or clicking)
//uses polling to avoid hook conflicts with JIP NVSE

#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnMenuSideChangeHandler.h"
#include "internal/NVSEMinimal.h"

//menu IDs
constexpr UInt32 kMenuType_Container = 1008;
constexpr UInt32 kMenuType_Barter = 1053;

//menu pointer addresses
constexpr UInt32 kAddr_ContainerMenuPtr = 0x11D93F8;
constexpr UInt32 kAddr_BarterMenuPtr = 0x11D8FA4;

//offsets for active side pointer
constexpr UInt32 kOffset_ContainerMenu_ActiveList = 0xF8;  //this[62]
constexpr UInt32 kOffset_ContainerMenu_LeftList = 0x98;    //this + 38
constexpr UInt32 kOffset_BarterMenu_ActiveList = 0x108;    //this[66]
constexpr UInt32 kOffset_BarterMenu_LeftList = 0xA8;       //this + 42

static FILE* g_omschLogFile = nullptr;

static void OMSCH_Log(const char* fmt, ...) {
    if (!g_omschLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_omschLogFile, fmt, args);
    fprintf(g_omschLogFile, "\n");
    fflush(g_omschLogFile);
    va_end(args);
}

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
    UInt32 activeListOffset = (menuType == kMenuType_Container) ? kOffset_ContainerMenu_ActiveList : kOffset_BarterMenu_ActiveList;
    UInt32 leftListOffset = (menuType == kMenuType_Container) ? kOffset_ContainerMenu_LeftList : kOffset_BarterMenu_LeftList;

    UInt32 activeList = *(UInt32*)((UInt8*)menu + activeListOffset);
    UInt32 leftList = (UInt32)menu + leftListOffset;

    return (activeList == leftList) ? 0 : 1;  //0 = left, 1 = right
}

static void DispatchSideChangeEvent(UInt32 menuID, UInt32 oldSide, UInt32 newSide) {
    if (!g_omschScript || g_callbacks.empty()) return;
    if (oldSide == newSide) return;

    OMSCH_Log("Side change: menu=%d old=%d new=%d", menuID, oldSide, newSide);

    for (const auto& cb : g_callbacks) {
        if (cb.menuFilter == 0 || cb.menuFilter == menuID) {
            if (cb.script) {
                OMSCH_Log("  Dispatching to callback 0x%08X", cb.script);
                g_omschScript->CallFunctionAlt(cb.script, nullptr, 3, menuID, oldSide, newSide);
            }
        }
    }
}

void OMSCH_Update() {
    if (g_callbacks.empty()) return;

    //check container menu
    void* contMenu = *(void**)kAddr_ContainerMenuPtr;
    if (contMenu) {
        if (contMenu != g_lastContainerMenu) {
            //menu just opened, initialize cache
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
    void* bartMenu = *(void**)kAddr_BarterMenuPtr;
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
        OMSCH_Log("SetOnMenuSideChangeEventHandler: Failed to extract args");
        return true;
    }

    if (!handlerForm || *((UInt8*)handlerForm + 4) != kFormType_Script) {
        OMSCH_Log("SetOnMenuSideChangeEventHandler: Invalid handler script");
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
            OMSCH_Log("SetOnMenuSideChangeEventHandler: Added callback 0x%08X menuFilter=%d", script, menuFilter);
        }
        *result = 1;
    } else {
        for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ++it) {
            if (it->script == script && it->menuFilter == menuFilter) {
                g_callbacks.erase(it);
                OMSCH_Log("SetOnMenuSideChangeEventHandler: Removed callback 0x%08X", script);
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

    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnMenuSideChangeHandler.log");
    g_omschLogFile = fopen(logPath, "w");

    OMSCH_Log("OnMenuSideChangeHandler initializing (polling mode)...");

    g_omschScript = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (!g_omschScript) {
        OMSCH_Log("ERROR: Failed to get script interface");
        return false;
    }
    g_ExtractArgsEx = g_omschScript->ExtractArgsEx;

    //no hooks needed - uses polling via OMSCH_Update()

    nvse->SetOpcodeBase(0x4033);
    nvse->RegisterCommand(&kCommandInfo_SetOnMenuSideChangeEventHandler);
    g_registeredOpcode = 0x4033;

    OMSCH_Log("Registered SetOnMenuSideChangeEventHandler at opcode 0x4033");
    OMSCH_Log("OnMenuSideChangeHandler initialized successfully");
    return true;
}

void OMSCH_ClearCallbacks()
{
    g_callbacks.clear();
    OMSCH_Log("Callbacks cleared on game load");
}
