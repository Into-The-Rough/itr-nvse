//fires when active side changes in Container/Barter menus (LT/RT, left/right arrows, or clicking)

#include <cstdint>
#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnMenuSideChangeHandler.h"

class TESForm;
class TESObjectREFR;
class Script;
class ScriptEventList;

struct CommandInfo;
struct ParamInfo;

using PluginHandle = UInt32;

struct NVSEInterface {
    UInt32  nvseVersion;
    UInt32  runtimeVersion;
    UInt32  editorVersion;
    UInt32  isEditor;
    bool    (*RegisterCommand)(CommandInfo* info);
    void    (*SetOpcodeBase)(UInt32 opcode);
    void*   (*QueryInterface)(UInt32 id);
    PluginHandle (*GetPluginHandle)(void);
    bool    (*RegisterTypedCommand)(CommandInfo* info, UInt32 retnType);
    const char* (*GetRuntimeDirectory)(void);
};

enum { kInterface_Script = 6 };
enum { kRetnType_Default = 0 };

struct NVSEArrayVarInterface {
    struct Element { UInt8 pad[16]; };
};

struct NVSEScriptInterface {
    bool (*CallFunction)(Script*, TESObjectREFR*, TESObjectREFR*, NVSEArrayVarInterface::Element*, UInt8, ...);
    int (*GetFunctionParams)(Script*, UInt8*);
    bool (*ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...);
    bool (*ExtractFormatStringArgs)(UInt32, char*, ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, UInt32, ...);
    bool (*CallFunctionAlt)(Script*, TESObjectREFR*, UInt8, ...);
};

#define COMMAND_ARGS void* paramInfo, void* scriptData, TESObjectREFR* thisObj, \
    UInt32 containingObj, Script* scriptObj, ScriptEventList* eventList, \
    double* result, UInt32* opcodeOffsetPtr

using CommandExecuteFunc = bool (*)(COMMAND_ARGS);

struct ParamInfo {
    const char* typeStr;
    UInt32 typeID;
    UInt32 isOptional;
};

struct CommandInfo {
    const char* longName;
    const char* shortName;
    UInt32 opcode;
    const char* helpText;
    UInt16 needsParent;
    UInt16 numParams;
    ParamInfo* params;
    CommandExecuteFunc execute;
    void* parse;
    void* eval;
    UInt32 flags;
};

enum { kParamType_Integer = 0x01, kParamType_AnyForm = 0x3D };
enum { kFormType_Script = 0x11 };

#define DEFINE_COMMAND_PLUGIN(name, desc, needsParent, numParams, params) \
    extern bool Cmd_##name##_Execute(COMMAND_ARGS); \
    static CommandInfo kCommandInfo_##name = { \
        #name, "", 0, desc, needsParent, numParams, params, \
        Cmd_##name##_Execute, nullptr, nullptr, 0 \
    }

//menu IDs
constexpr UInt32 kMenuType_Container = 1008;
constexpr UInt32 kMenuType_Barter = 1053;

//function addresses
constexpr UInt32 kAddr_ContainerMenu_HandleMouseover = 0x75CD70;
constexpr UInt32 kAddr_BarterMenu_HandleMouseover = 0x72F770;

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

//trampolines
static UInt32 g_originalContainerHandleMouseover = 0;
static UInt32 g_originalBarterHandleMouseover = 0;
static UInt8* g_trampolineContainerMouseover = nullptr;
static UInt8* g_trampolineBarterMouseover = nullptr;

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

//ContainerMenu::HandleMouseover hook
//original: void __thiscall ContainerMenu::HandleMouseover(unsigned __int64 clickID)
//prologue: 55 8B EC 83 EC 08 (6 bytes: push ebp; mov ebp,esp; sub esp,8)
static void __fastcall Hook_ContainerMenu_HandleMouseover(void* menu, void* edx, UInt32 clickIDLo, UInt32 clickIDHi) {
    UInt32 oldSide = GetCurrentSide(menu, kMenuType_Container);

    OMSCH_Log("ContainerMenu::HandleMouseover called, clickID=%u:%u oldSide=%d", clickIDHi, clickIDLo, oldSide);

    //call original
    ((void(__thiscall*)(void*, UInt32, UInt32))g_originalContainerHandleMouseover)(menu, clickIDLo, clickIDHi);

    UInt32 newSide = GetCurrentSide(menu, kMenuType_Container);

    OMSCH_Log("  after: newSide=%d", newSide);

    if (oldSide != newSide) {
        DispatchSideChangeEvent(kMenuType_Container, oldSide, newSide);
    }
}

//BarterMenu::HandleMouseover hook
//original: void __thiscall BarterMenu::HandleMouseover(unsigned __int64 clickID)
//prologue: 55 8B EC 83 EC 08 (6 bytes: push ebp; mov ebp,esp; sub esp,8)
static void __fastcall Hook_BarterMenu_HandleMouseover(void* menu, void* edx, UInt32 clickIDLo, UInt32 clickIDHi) {
    UInt32 oldSide = GetCurrentSide(menu, kMenuType_Barter);

    OMSCH_Log("BarterMenu::HandleMouseover called, clickID=%u:%u oldSide=%d", clickIDHi, clickIDLo, oldSide);

    //call original
    ((void(__thiscall*)(void*, UInt32, UInt32))g_originalBarterHandleMouseover)(menu, clickIDLo, clickIDHi);

    UInt32 newSide = GetCurrentSide(menu, kMenuType_Barter);

    OMSCH_Log("  after: newSide=%d", newSide);

    if (oldSide != newSide) {
        DispatchSideChangeEvent(kMenuType_Barter, oldSide, newSide);
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
    //ContainerMenu::HandleMouseover - 6 byte prologue
    g_trampolineContainerMouseover = CreateTrampoline(kAddr_ContainerMenu_HandleMouseover, 6);
    if (g_trampolineContainerMouseover) {
        g_originalContainerHandleMouseover = (UInt32)g_trampolineContainerMouseover;
        WriteRelJump(kAddr_ContainerMenu_HandleMouseover, (UInt32)Hook_ContainerMenu_HandleMouseover);
        PatchWrite8(kAddr_ContainerMenu_HandleMouseover + 5, 0x90);
        OMSCH_Log("Hooked ContainerMenu::HandleMouseover at 0x%08X", kAddr_ContainerMenu_HandleMouseover);
    }

    //BarterMenu::HandleMouseover - 6 byte prologue
    g_trampolineBarterMouseover = CreateTrampoline(kAddr_BarterMenu_HandleMouseover, 6);
    if (g_trampolineBarterMouseover) {
        g_originalBarterHandleMouseover = (UInt32)g_trampolineBarterMouseover;
        WriteRelJump(kAddr_BarterMenu_HandleMouseover, (UInt32)Hook_BarterMenu_HandleMouseover);
        PatchWrite8(kAddr_BarterMenu_HandleMouseover + 5, 0x90);
        OMSCH_Log("Hooked BarterMenu::HandleMouseover at 0x%08X", kAddr_BarterMenu_HandleMouseover);
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

    OMSCH_Log("OnMenuSideChangeHandler initializing...");

    g_omschScript = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (!g_omschScript) {
        OMSCH_Log("ERROR: Failed to get script interface");
        return false;
    }
    g_ExtractArgsEx = g_omschScript->ExtractArgsEx;

    InstallHooks();

    nvse->SetOpcodeBase(0x4033);
    nvse->RegisterCommand(&kCommandInfo_SetOnMenuSideChangeEventHandler);
    g_registeredOpcode = 0x4033;

    OMSCH_Log("Registered SetOnMenuSideChangeEventHandler at opcode 0x4033");
    OMSCH_Log("OnMenuSideChangeHandler initialized successfully");
    return true;
}
