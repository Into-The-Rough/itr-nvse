//fires when menu filter/category changes (Inventory, Container, Barter menus)

#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnMenuFilterChangeHandler.h"
#include "internal/NVSEMinimal.h"

//menu IDs
constexpr UInt32 kMenuType_Inventory = 1002;
constexpr UInt32 kMenuType_Container = 1008;
constexpr UInt32 kMenuType_Barter = 1053;

//menu pointer addresses
constexpr UInt32 kAddr_InventoryMenuPtr = 0x11D9EA4;
constexpr UInt32 kAddr_ContainerMenuPtr = 0x11D93F8;
constexpr UInt32 kAddr_BarterMenuPtr = 0x11D8FA4;

//filter offsets
constexpr UInt32 kOffset_InventoryMenu_Filter = 0x84;
constexpr UInt32 kOffset_ContainerMenu_LeftFilter = 0x8C;
constexpr UInt32 kOffset_ContainerMenu_RightFilter = 0x90;
constexpr UInt32 kOffset_BarterMenu_LeftFilter = 0x9C;
constexpr UInt32 kOffset_BarterMenu_RightFilter = 0xA0;

//function addresses
constexpr UInt32 kAddr_InventoryMenu_SetCurrentTab = 0x706A40;
constexpr UInt32 kAddr_ContainerMenu_HandleClick = 0x75BE80;
constexpr UInt32 kAddr_BarterMenu_HandleClick = 0x72D770;

static FILE* g_omfchLogFile = nullptr;

static void OMFCH_Log(const char* fmt, ...) {
    if (!g_omfchLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_omfchLogFile, fmt, args);
    fprintf(g_omfchLogFile, "\n");
    fflush(g_omfchLogFile);
    va_end(args);
}

static NVSEScriptInterface* g_omfchScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_registeredOpcode = 0;

struct FilterChangeCallback {
    Script* script;
    UInt32 menuFilter;  //0 = all menus, otherwise specific menuID
};

static std::vector<FilterChangeCallback> g_callbacks;

static void DispatchFilterChangeEvent(UInt32 menuID, UInt32 oldFilter, UInt32 newFilter, UInt32 side) {
    if (!g_omfchScript || g_callbacks.empty()) return;
    if (oldFilter == newFilter) return;

    OMFCH_Log("Filter change: menu=%d old=%d new=%d side=%d", menuID, oldFilter, newFilter, side);

    for (const auto& cb : g_callbacks) {
        if (cb.menuFilter == 0 || cb.menuFilter == menuID) {
            if (cb.script) {
                OMFCH_Log("  Dispatching to callback 0x%08X", cb.script);
                g_omfchScript->CallFunctionAlt(cb.script, nullptr, 4, menuID, oldFilter, newFilter, side);
            }
        }
    }
}

//trampolines
static UInt32 g_originalSetCurrentTab = 0;
static UInt32 g_originalContainerHandleClick = 0;
static UInt32 g_originalBarterHandleClick = 0;
static UInt8* g_trampolineSetTab = nullptr;
static UInt8* g_trampolineContainerClick = nullptr;
static UInt8* g_trampolineBarterClick = nullptr;

static void PatchWrite32(UInt32 addr, UInt32 data) {
    DWORD oldProtect;
    VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
    *(UInt32*)addr = data;
    VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
}

static void PatchWrite8(UInt32 addr, UInt8 data) {
    DWORD oldProtect;
    VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
    *(UInt8*)addr = data;
    VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
}

static void WriteRelJump(UInt32 src, UInt32 dst) {
    PatchWrite8(src, 0xE9);
    PatchWrite32(src + 1, dst - src - 5);
}

//InventoryMenu::SetCurrentTab hook
//original: void __cdecl InventoryMenu::SetCurrentTab(signed int filter)
//prologue: 55 8B EC 83 3D A4 9E 1D 01 00 (10 bytes: push ebp; mov ebp,esp; cmp g_inventoryMenu,0)
static void __cdecl Hook_InventoryMenu_SetCurrentTab(UInt32 newFilter) {
    void* menu = *(void**)kAddr_InventoryMenuPtr;
    UInt32 oldFilter = 0;

    OMFCH_Log("InventoryMenu::SetCurrentTab called, newFilter=%d menu=%p", newFilter, menu);

    if (menu) {
        oldFilter = *(UInt32*)((UInt8*)menu + kOffset_InventoryMenu_Filter);
        OMFCH_Log("  oldFilter=%d", oldFilter);
    }

    //call original
    ((void(__cdecl*)(UInt32))g_originalSetCurrentTab)(newFilter);

    //dispatch event if changed
    if (menu && oldFilter != newFilter) {
        DispatchFilterChangeEvent(kMenuType_Inventory, oldFilter, newFilter, 0);
    }
}

//ContainerMenu::HandleClick hook
//original: void __thiscall ContainerMenu::HandleClick(unsigned __int64 clickID)
//prologue: 55 8B EC 6A FF (5 bytes: push ebp; mov ebp,esp; push -1)
static void __fastcall Hook_ContainerMenu_HandleClick(void* menu, void* edx, UInt32 clickIDLo, UInt32 clickIDHi) {
    UInt32 oldLeft = *(UInt32*)((UInt8*)menu + kOffset_ContainerMenu_LeftFilter);
    UInt32 oldRight = *(UInt32*)((UInt8*)menu + kOffset_ContainerMenu_RightFilter);

    OMFCH_Log("ContainerMenu::HandleClick called, clickID=%u:%u oldLeft=%d oldRight=%d", clickIDHi, clickIDLo, oldLeft, oldRight);

    //call original - pass both halves of the 64-bit clickID
    ((void(__thiscall*)(void*, UInt32, UInt32))g_originalContainerHandleClick)(menu, clickIDLo, clickIDHi);

    //check for changes
    UInt32 newLeft = *(UInt32*)((UInt8*)menu + kOffset_ContainerMenu_LeftFilter);
    UInt32 newRight = *(UInt32*)((UInt8*)menu + kOffset_ContainerMenu_RightFilter);

    OMFCH_Log("  after: newLeft=%d newRight=%d", newLeft, newRight);

    if (oldLeft != newLeft) {
        DispatchFilterChangeEvent(kMenuType_Container, oldLeft, newLeft, 0);
    }
    if (oldRight != newRight) {
        DispatchFilterChangeEvent(kMenuType_Container, oldRight, newRight, 1);
    }
}

//BarterMenu::HandleClick hook
//original: void __thiscall BarterMenu::HandleClick(unsigned __int64 clickID)
static void __fastcall Hook_BarterMenu_HandleClick(void* menu, void* edx, UInt32 clickIDLo, UInt32 clickIDHi) {
    UInt32 oldLeft = *(UInt32*)((UInt8*)menu + kOffset_BarterMenu_LeftFilter);
    UInt32 oldRight = *(UInt32*)((UInt8*)menu + kOffset_BarterMenu_RightFilter);

    OMFCH_Log("BarterMenu::HandleClick called, clickID=%u:%u oldLeft=%d oldRight=%d", clickIDHi, clickIDLo, oldLeft, oldRight);

    //call original - pass both halves of the 64-bit clickID
    ((void(__thiscall*)(void*, UInt32, UInt32))g_originalBarterHandleClick)(menu, clickIDLo, clickIDHi);

    //check for changes
    UInt32 newLeft = *(UInt32*)((UInt8*)menu + kOffset_BarterMenu_LeftFilter);
    UInt32 newRight = *(UInt32*)((UInt8*)menu + kOffset_BarterMenu_RightFilter);

    OMFCH_Log("  after: newLeft=%d newRight=%d", newLeft, newRight);

    if (oldLeft != newLeft) {
        DispatchFilterChangeEvent(kMenuType_Barter, oldLeft, newLeft, 0);
    }
    if (oldRight != newRight) {
        DispatchFilterChangeEvent(kMenuType_Barter, oldRight, newRight, 1);
    }
}

static UInt8* CreateTrampoline(UInt32 originalAddr, int prologueBytes) {
    UInt8* trampoline = (UInt8*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return nullptr;

    DWORD oldProtect;
    VirtualProtect((void*)originalAddr, prologueBytes, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(trampoline, (void*)originalAddr, prologueBytes);
    VirtualProtect((void*)originalAddr, prologueBytes, oldProtect, &oldProtect);

    //jmp back to original + prologueBytes
    trampoline[prologueBytes] = 0xE9;
    *(UInt32*)(trampoline + prologueBytes + 1) = (originalAddr + prologueBytes) - ((UInt32)trampoline + prologueBytes + 5);

    return trampoline;
}

static void InstallHooks() {
    //InventoryMenu::SetCurrentTab - prologue is 10 bytes
    //55 8B EC 83 3D A4 9E 1D 01 00 = push ebp; mov ebp,esp; cmp g_inventoryMenu,0
    g_trampolineSetTab = CreateTrampoline(kAddr_InventoryMenu_SetCurrentTab, 10);
    if (g_trampolineSetTab) {
        g_originalSetCurrentTab = (UInt32)g_trampolineSetTab;
        WriteRelJump(kAddr_InventoryMenu_SetCurrentTab, (UInt32)Hook_InventoryMenu_SetCurrentTab);
        for (int i = 5; i < 10; i++)
            PatchWrite8(kAddr_InventoryMenu_SetCurrentTab + i, 0x90);
        OMFCH_Log("Hooked InventoryMenu::SetCurrentTab at 0x%08X", kAddr_InventoryMenu_SetCurrentTab);
    }

    //ContainerMenu::HandleClick - prologue is 5 bytes
    //55 8B EC 6A FF = push ebp; mov ebp,esp; push -1
    g_trampolineContainerClick = CreateTrampoline(kAddr_ContainerMenu_HandleClick, 5);
    if (g_trampolineContainerClick) {
        g_originalContainerHandleClick = (UInt32)g_trampolineContainerClick;
        WriteRelJump(kAddr_ContainerMenu_HandleClick, (UInt32)Hook_ContainerMenu_HandleClick);
        OMFCH_Log("Hooked ContainerMenu::HandleClick at 0x%08X", kAddr_ContainerMenu_HandleClick);
    }

    //BarterMenu::HandleClick - prologue is 6 bytes
    //55 8B EC 83 EC 14 = push ebp; mov ebp,esp; sub esp,14h
    g_trampolineBarterClick = CreateTrampoline(kAddr_BarterMenu_HandleClick, 6);
    if (g_trampolineBarterClick) {
        g_originalBarterHandleClick = (UInt32)g_trampolineBarterClick;
        WriteRelJump(kAddr_BarterMenu_HandleClick, (UInt32)Hook_BarterMenu_HandleClick);
        PatchWrite8(kAddr_BarterMenu_HandleClick + 5, 0x90);
        OMFCH_Log("Hooked BarterMenu::HandleClick at 0x%08X", kAddr_BarterMenu_HandleClick);
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
        OMFCH_Log("SetOnMenuFilterChangeEventHandler: Failed to extract args");
        return true;
    }

    if (!handlerForm || *((UInt8*)handlerForm + 4) != kFormType_Script) {
        OMFCH_Log("SetOnMenuFilterChangeEventHandler: Invalid handler script");
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
            OMFCH_Log("SetOnMenuFilterChangeEventHandler: Added callback 0x%08X menuFilter=%d", script, menuFilter);
        }
        *result = 1;
    } else {
        for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ++it) {
            if (it->script == script && it->menuFilter == menuFilter) {
                g_callbacks.erase(it);
                OMFCH_Log("SetOnMenuFilterChangeEventHandler: Removed callback 0x%08X", script);
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

    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnMenuFilterChangeHandler.log");
    g_omfchLogFile = fopen(logPath, "w");

    OMFCH_Log("OnMenuFilterChangeHandler initializing...");

    g_omfchScript = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (!g_omfchScript) {
        OMFCH_Log("ERROR: Failed to get script interface");
        return false;
    }
    g_ExtractArgsEx = g_omfchScript->ExtractArgsEx;

    InstallHooks();

    nvse->SetOpcodeBase(0x4032);
    nvse->RegisterCommand(&kCommandInfo_SetOnMenuFilterChangeEventHandler);
    g_registeredOpcode = 0x4032;

    OMFCH_Log("Registered SetOnMenuFilterChangeEventHandler at opcode 0x4032");
    OMFCH_Log("OnMenuFilterChangeHandler initialized successfully");
    return true;
}
