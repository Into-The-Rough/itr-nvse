//fires when menu filter/category changes (Inventory, Container, Barter menus)
//uses polling to avoid hook conflicts with JIP NVSE

#include <vector>
#include <Windows.h>

#include "OnMenuFilterChangeHandler.h"
#include "internal/NVSEMinimal.h"

constexpr UInt32 kMenuType_Container = 1008;
constexpr UInt32 kMenuType_Barter = 1053;

constexpr UInt32 kOffset_InventoryMenu_Filter = 0x84;
constexpr UInt32 kOffset_ContainerMenu_LeftFilter = 0x8C;
constexpr UInt32 kOffset_ContainerMenu_RightFilter = 0x90;
constexpr UInt32 kOffset_BarterMenu_LeftFilter = 0x9C;
constexpr UInt32 kOffset_BarterMenu_RightFilter = 0xA0;

static NVSEScriptInterface* g_omfchScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_registeredOpcode = 0;

struct FilterChangeCallback {
    Script* script;
    UInt32 menuFilter;  //0 = all menus, otherwise specific menuID
};

static std::vector<FilterChangeCallback> g_callbacks;

//cached filter values for polling
static UInt32 g_lastInventoryFilter = 0xFFFFFFFF;
static UInt32 g_lastContainerLeftFilter = 0xFFFFFFFF;
static UInt32 g_lastContainerRightFilter = 0xFFFFFFFF;
static UInt32 g_lastBarterLeftFilter = 0xFFFFFFFF;
static UInt32 g_lastBarterRightFilter = 0xFFFFFFFF;
static void* g_lastInventoryMenu = nullptr;
static void* g_lastContainerMenu = nullptr;
static void* g_lastBarterMenu = nullptr;

static void DispatchFilterChangeEvent(UInt32 menuID, UInt32 oldFilter, UInt32 newFilter, UInt32 side) {
    if (!g_omfchScript || g_callbacks.empty()) return;
    if (oldFilter == newFilter) return;

    const auto snapshot = g_callbacks;

    for (const auto& cb : snapshot) {
        if (cb.menuFilter == 0 || cb.menuFilter == menuID) {
            if (cb.script) {
                g_omfchScript->CallFunctionAlt(cb.script, nullptr, 4, menuID, oldFilter, newFilter, side);
            }
        }
    }
}

void OMFCH_Update() {
    if (g_callbacks.empty()) return;

    //check inventory menu
    void* invMenu = *(void**)0x11D9EA4; //InventoryMenu
    if (invMenu) {
        if (invMenu != g_lastInventoryMenu) {
            g_lastInventoryFilter = *(UInt32*)((UInt8*)invMenu + kOffset_InventoryMenu_Filter);
            g_lastInventoryMenu = invMenu;
        } else {
            UInt32 currentFilter = *(UInt32*)((UInt8*)invMenu + kOffset_InventoryMenu_Filter);
            if (currentFilter != g_lastInventoryFilter && g_lastInventoryFilter != 0xFFFFFFFF) {
                DispatchFilterChangeEvent(1002, g_lastInventoryFilter, currentFilter, 0); //InventoryMenu
            }
            g_lastInventoryFilter = currentFilter;
        }
    } else {
        g_lastInventoryMenu = nullptr;
        g_lastInventoryFilter = 0xFFFFFFFF;
    }

    //check container menu
    void* contMenu = *(void**)0x11D93F8; //ContainerMenu
    if (contMenu) {
        if (contMenu != g_lastContainerMenu) {
            g_lastContainerLeftFilter = *(UInt32*)((UInt8*)contMenu + kOffset_ContainerMenu_LeftFilter);
            g_lastContainerRightFilter = *(UInt32*)((UInt8*)contMenu + kOffset_ContainerMenu_RightFilter);
            g_lastContainerMenu = contMenu;
        } else {
            UInt32 leftFilter = *(UInt32*)((UInt8*)contMenu + kOffset_ContainerMenu_LeftFilter);
            UInt32 rightFilter = *(UInt32*)((UInt8*)contMenu + kOffset_ContainerMenu_RightFilter);
            if (leftFilter != g_lastContainerLeftFilter && g_lastContainerLeftFilter != 0xFFFFFFFF) {
                DispatchFilterChangeEvent(kMenuType_Container, g_lastContainerLeftFilter, leftFilter, 0);
            }
            if (rightFilter != g_lastContainerRightFilter && g_lastContainerRightFilter != 0xFFFFFFFF) {
                DispatchFilterChangeEvent(kMenuType_Container, g_lastContainerRightFilter, rightFilter, 1);
            }
            g_lastContainerLeftFilter = leftFilter;
            g_lastContainerRightFilter = rightFilter;
        }
    } else {
        g_lastContainerMenu = nullptr;
        g_lastContainerLeftFilter = 0xFFFFFFFF;
        g_lastContainerRightFilter = 0xFFFFFFFF;
    }

    //check barter menu
    void* bartMenu = *(void**)0x11D8FA4; //BarterMenu
    if (bartMenu) {
        if (bartMenu != g_lastBarterMenu) {
            g_lastBarterLeftFilter = *(UInt32*)((UInt8*)bartMenu + kOffset_BarterMenu_LeftFilter);
            g_lastBarterRightFilter = *(UInt32*)((UInt8*)bartMenu + kOffset_BarterMenu_RightFilter);
            g_lastBarterMenu = bartMenu;
        } else {
            UInt32 leftFilter = *(UInt32*)((UInt8*)bartMenu + kOffset_BarterMenu_LeftFilter);
            UInt32 rightFilter = *(UInt32*)((UInt8*)bartMenu + kOffset_BarterMenu_RightFilter);
            if (leftFilter != g_lastBarterLeftFilter && g_lastBarterLeftFilter != 0xFFFFFFFF) {
                DispatchFilterChangeEvent(kMenuType_Barter, g_lastBarterLeftFilter, leftFilter, 0);
            }
            if (rightFilter != g_lastBarterRightFilter && g_lastBarterRightFilter != 0xFFFFFFFF) {
                DispatchFilterChangeEvent(kMenuType_Barter, g_lastBarterRightFilter, rightFilter, 1);
            }
            g_lastBarterLeftFilter = leftFilter;
            g_lastBarterRightFilter = rightFilter;
        }
    } else {
        g_lastBarterMenu = nullptr;
        g_lastBarterLeftFilter = 0xFFFFFFFF;
        g_lastBarterRightFilter = 0xFFFFFFFF;
    }
}

static ParamInfo kParams_SetOnMenuFilterChangeEventHandler[3] = {
    {"setOrRemove", kParamType_Integer, 0},
    {"handler",     kParamType_AnyForm, 0},
    {"menuFilter",  kParamType_Integer, 1},  //optional: 0=all, 1002=Inventory, 1008=Container, 1053=Barter
};

DEFINE_COMMAND_PLUGIN(SetOnMenuFilterChangeEventHandler,
    "Registers/unregisters callback for menu filter change events. Args: menuID, oldFilter, newFilter, side (0=left, 1=right)",
    0, 3, kParams_SetOnMenuFilterChangeEventHandler);

bool Cmd_SetOnMenuFilterChangeEventHandler_Execute(COMMAND_ARGS) {
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

unsigned int OMFCH_GetOpcode() {
    return g_registeredOpcode;
}

bool OMFCH_Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    g_omfchScript = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (!g_omfchScript) {
        return false;
    }
    g_ExtractArgsEx = g_omfchScript->ExtractArgsEx;

    //no hooks needed - uses polling via OMFCH_Update()

    nvse->SetOpcodeBase(0x4032);
    nvse->RegisterCommand(&kCommandInfo_SetOnMenuFilterChangeEventHandler);
    g_registeredOpcode = 0x4032;

    return true;
}

void OMFCH_ClearCallbacks()
{
    g_callbacks.clear();
}
