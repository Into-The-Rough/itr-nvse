//hooks CombatController::SetActionProcedure and SetMovementProcedure to fire events

#include <cstdint>
#include <vector>
#include <cstdio>
#include <Windows.h>

#include "OnCombatProcedureHandler.h"

using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using SInt32 = int32_t;

class TESForm;
class TESObjectREFR;
class Actor;
class Script;
class ScriptEventList;

struct PluginInfo {
    enum { kInfoVersion = 1 };
    UInt32      infoVersion;
    const char* name;
    UInt32      version;
};

struct CommandInfo;
struct ParamInfo;

using PluginHandle = UInt32;
constexpr PluginHandle kPluginHandle_Invalid = 0xFFFFFFFF;

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

enum {
    kInterface_Serialization = 0,
    kInterface_Console,
    kInterface_Messaging,
    kInterface_CommandTable,
    kInterface_StringVar,
    kInterface_ArrayVar,
    kInterface_Script,
    kInterface_Data,
};

struct NVSEArrayVarInterface {
    struct Element {
        UInt8 pad[16];
    };
};

struct NVSEScriptInterface {
    enum { kVersion = 1 };

    bool    (*CallFunction)(Script* funcScript, TESObjectREFR* callingObj,
                TESObjectREFR* container, NVSEArrayVarInterface::Element* result,
                UInt8 numArgs, ...);
    int     (*GetFunctionParams)(Script* funcScript, UInt8* paramTypesOut);
    bool    (*ExtractArgsEx)(ParamInfo* paramInfo, void* scriptDataIn,
                UInt32* scriptDataOffset, Script* scriptObj, ScriptEventList* eventList, ...);
    bool    (*ExtractFormatStringArgs)(UInt32 fmtStringPos, char* buffer,
                ParamInfo* paramInfo, void* scriptDataIn, UInt32* scriptDataOffset,
                Script* scriptObj, ScriptEventList* eventList, UInt32 maxParams, ...);
    bool    (*CallFunctionAlt)(Script* funcScript, TESObjectREFR* callingObj,
                UInt8 numArgs, ...);
};

#define COMMAND_ARGS        void* paramInfo, void* scriptData, TESObjectREFR* thisObj, \
                            UInt32 containingObj, Script* scriptObj, ScriptEventList* eventList, \
                            double* result, UInt32* opcodeOffsetPtr

#define EXTRACT_ARGS_EX     paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList

using CommandExecuteFunc = bool (*)(COMMAND_ARGS);
using CommandParseFunc = bool (*)(UInt32, void*, void*, void*);
using CommandEvalFunc = bool (*)(TESObjectREFR*, void*, void*, double*);

struct ParamInfo {
    const char* typeStr;
    UInt32      typeID;
    UInt32      isOptional;
};

struct CommandInfo {
    const char*         longName;
    const char*         shortName;
    UInt32              opcode;
    const char*         helpText;
    UInt16              needsParent;
    UInt16              numParams;
    ParamInfo*          params;
    CommandExecuteFunc  execute;
    CommandParseFunc    parse;
    CommandEvalFunc     eval;
    UInt32              flags;
};

enum ParamType {
    kParamType_Integer      = 0x01,
    kParamType_AnyForm      = 0x3D,
};

enum FormType {
    kFormType_Script = 0x11,
};

#define DEFINE_COMMAND_PLUGIN(name, desc, needsParent, numParams, params) \
    extern bool Cmd_##name##_Execute(COMMAND_ARGS); \
    static CommandInfo kCommandInfo_##name = { \
        #name, "", 0, desc, needsParent, numParams, params, \
        Cmd_##name##_Execute, nullptr, nullptr, 0 \
    }

namespace SafeWrite {
    inline void Write8(UInt32 addr, UInt8 data) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(UInt8*)addr = data;
        VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
    }

    inline void Write32(UInt32 addr, UInt32 data) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(UInt32*)addr = data;
        VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
    }

    inline void WriteRelJump(UInt32 src, UInt32 dst) {
        Write8(src, 0xE9);
        Write32(src + 1, dst - src - 5);
    }

    inline void WriteRelCall(UInt32 src, UInt32 dst) {
        Write8(src, 0xE8);
        Write32(src + 1, dst - src - 5);
    }
}

static FILE* g_ocphLogFile = nullptr;

static void OCPH_Log(const char* fmt, ...)
{
    if (!g_ocphLogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_ocphLogFile, fmt, args);
    fprintf(g_ocphLogFile, "\n");
    fflush(g_ocphLogFile);
    va_end(args);
}

static PluginHandle g_ocphPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_ocphScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_ocphOpcode = 0;

constexpr UInt32 kAddr_SetActionProcedure = 0x980110;
constexpr UInt32 kAddr_SetMovementProcedure = 0x9801B0;
constexpr UInt32 kAddr_GetPackageOwner = 0x97AE90;
constexpr UInt32 kAddr_GetBaseForm = 0x6F2070;

struct CallbackEntry {
    Script* callback;
    TESForm* actorFilter;
    SInt32 procFilter;
};

static void* g_combatController = nullptr;
static void* g_procedure = nullptr;
static bool g_isActionProcedure = false;

namespace OnCombatProcedureHandler {
    std::vector<CallbackEntry> g_callbacks;
    bool g_hookInstalled = false;
    static UInt32 s_trampolineActionAddr = 0;
    static UInt32 s_trampolineMovementAddr = 0;
}

static UInt8* g_trampolineAction = nullptr;
static UInt8* g_trampolineMovement = nullptr;

static Actor* GetPackageOwner(void* combatController)
{
    if (!combatController) return nullptr;
    typedef Actor* (__thiscall *GetPackageOwner_t)(void*);
    return ((GetPackageOwner_t)kAddr_GetPackageOwner)(combatController);
}

//vtable addresses for each procedure type (0-12)
static UInt32 s_vtableMap[13] = {0};
static bool s_vtablesInitialized = false;

//scan constructor for "mov [reg], vtable_addr" pattern
static UInt32 ReadVtableFromConstructor(UInt32 ctorAddr)
{
    for (UInt32 i = 0; i < 100; i++) {
        UInt8 byte1 = *(UInt8*)(ctorAddr + i);
        UInt8 byte2 = *(UInt8*)(ctorAddr + i + 1);
        if (byte1 == 0xC7 && (byte2 == 0x00 || byte2 == 0x01)) {
            UInt32 vtable = *(UInt32*)(ctorAddr + i + 2);
            if ((vtable & 0xFFFF0000) == 0x01090000) {
                return vtable;
            }
        }
    }
    return 0;
}

static void InitVtables()
{
    if (s_vtablesInitialized) return;

    //constructor addresses for procedure types 0-12
    s_vtableMap[0] = ReadVtableFromConstructor(0x9D0890);   //AttackRanged
    s_vtableMap[1] = ReadVtableFromConstructor(0x9CC0C0);   //AttackMelee
    s_vtableMap[2] = ReadVtableFromConstructor(0x9CADF0);   //AttackGrenade
    s_vtableMap[3] = ReadVtableFromConstructor(0x9CBAD0);   //AttackLow
    s_vtableMap[4] = ReadVtableFromConstructor(0x9D5AD0);   //Evade
    s_vtableMap[5] = ReadVtableFromConstructor(0x9DA720);   //SwitchWeapon
    s_vtableMap[6] = ReadVtableFromConstructor(0x9D69F0);   //Move
    s_vtableMap[7] = ReadVtableFromConstructor(0x9D2440);   //BeInCover
    s_vtableMap[8] = ReadVtableFromConstructor(0x9CA5F0);   //ActivateObject
    s_vtableMap[9] = ReadVtableFromConstructor(0x9D6030);   //HideFromTarget
    s_vtableMap[10] = ReadVtableFromConstructor(0x9D8B60);  //Search
    s_vtableMap[11] = ReadVtableFromConstructor(0x9DAA10);  //UseCombatItem
    s_vtableMap[12] = ReadVtableFromConstructor(0x9D3A00);  //EngageTarget

    OCPH_Log("Vtables initialized:");
    for (int i = 0; i <= 12; i++) {
        OCPH_Log("  Type %d: 0x%08X", i, s_vtableMap[i]);
    }

    s_vtablesInitialized = true;
}

static UInt32 GetProcedureType(void* procedure)
{
    if (!procedure) return (UInt32)-1;

    if (!s_vtablesInitialized) {
        InitVtables();
    }

    UInt32 vtable = *(UInt32*)procedure;

    for (int i = 0; i <= 12; i++) {
        if (s_vtableMap[i] == vtable) {
            return i;
        }
    }

    OCPH_Log("Unknown procedure vtable: 0x%08X", vtable);
    return (UInt32)-1;
}

static TESForm* GetActorBaseForm(Actor* actor)
{
    if (!actor) return nullptr;
    typedef TESForm* (__thiscall *GetBaseForm_t)(void*);
    return ((GetBaseForm_t)kAddr_GetBaseForm)(actor);
}

static bool PassesActorFilter(Actor* actor, TESForm* filter)
{
    if (!filter) return true;
    if ((TESForm*)actor == filter) return true;
    TESForm* baseForm = GetActorBaseForm(actor);
    if (baseForm == filter) return true;
    return false;
}

static bool PassesProcFilter(UInt32 procType, SInt32 filter)
{
    if (filter < 0) return true;
    return (SInt32)procType == filter;
}

static void DispatchCombatProcedureEvent()
{
    if (!g_combatController || !g_procedure) return;

    Actor* actor = GetPackageOwner(g_combatController);
    if (!actor) return;

    UInt32 procType = GetProcedureType(g_procedure);

    OCPH_Log("DispatchCombatProcedureEvent: actor=0x%08X procType=%d isAction=%d",
             actor, procType, g_isActionProcedure ? 1 : 0);

    for (const CallbackEntry& entry : OnCombatProcedureHandler::g_callbacks) {
        if (!g_ocphScript || !entry.callback) continue;

        if (!PassesActorFilter(actor, entry.actorFilter)) continue;
        if (!PassesProcFilter(procType, entry.procFilter)) continue;

        OCPH_Log("  Calling callback 0x%08X", entry.callback);
        g_ocphScript->CallFunctionAlt(
            entry.callback,
            reinterpret_cast<TESObjectREFR*>(actor),
            3,
            actor,
            procType,
            g_isActionProcedure ? 1 : 0
        );
    }
}

static __declspec(naked) void Hook_SetActionProcedure()
{
    __asm {
        mov g_combatController, ecx
        mov eax, [esp+4]
        mov g_procedure, eax
        mov g_isActionProcedure, 1

        pushad
        pushfd
        call DispatchCombatProcedureEvent
        popfd
        popad

        jmp dword ptr [OnCombatProcedureHandler::s_trampolineActionAddr]
    }
}

static __declspec(naked) void Hook_SetMovementProcedure()
{
    __asm {
        mov g_combatController, ecx
        mov eax, [esp+4]
        mov g_procedure, eax
        mov g_isActionProcedure, 0

        pushad
        pushfd
        call DispatchCombatProcedureEvent
        popfd
        popad

        jmp dword ptr [OnCombatProcedureHandler::s_trampolineMovementAddr]
    }
}

static UInt8* AllocateTrampoline(UInt32 funcAddr, UInt32 prologueSize)
{
    UInt8* buffer = (UInt8*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!buffer) return nullptr;

    memcpy(buffer, (void*)funcAddr, prologueSize);

    buffer[prologueSize] = 0xE9;
    UInt32 jumpTarget = funcAddr + prologueSize;
    *(UInt32*)(buffer + prologueSize + 1) = jumpTarget - (UInt32)(buffer + prologueSize) - 5;

    return buffer;
}

static void InitHooks()
{
    if (OnCombatProcedureHandler::g_hookInstalled) return;

    OCPH_Log("InitHooks: Installing combat procedure hooks...");

    //both functions: push ebp; mov ebp,esp; sub esp,10h = 6 bytes
    constexpr UInt32 kPrologueSize = 6;

    g_trampolineAction = AllocateTrampoline(kAddr_SetActionProcedure, kPrologueSize);
    g_trampolineMovement = AllocateTrampoline(kAddr_SetMovementProcedure, kPrologueSize);

    if (!g_trampolineAction || !g_trampolineMovement) {
        OCPH_Log("ERROR: Failed to allocate trampolines");
        return;
    }

    OnCombatProcedureHandler::s_trampolineActionAddr = (UInt32)g_trampolineAction;
    OnCombatProcedureHandler::s_trampolineMovementAddr = (UInt32)g_trampolineMovement;

    OCPH_Log("Trampolines created:");
    OCPH_Log("  Action: 0x%08X", g_trampolineAction);
    OCPH_Log("  Movement: 0x%08X", g_trampolineMovement);

    SafeWrite::WriteRelJump(kAddr_SetActionProcedure, (UInt32)Hook_SetActionProcedure);
    SafeWrite::Write8(kAddr_SetActionProcedure + 5, 0x90);

    SafeWrite::WriteRelJump(kAddr_SetMovementProcedure, (UInt32)Hook_SetMovementProcedure);
    SafeWrite::Write8(kAddr_SetMovementProcedure + 5, 0x90);

    OnCombatProcedureHandler::g_hookInstalled = true;
    OCPH_Log("InitHooks: Hooks installed successfully");
    OCPH_Log("  ActionProcedure: 0x%08X -> Hook -> Trampoline", kAddr_SetActionProcedure);
    OCPH_Log("  MovementProcedure: 0x%08X -> Hook -> Trampoline", kAddr_SetMovementProcedure);
}

static bool AddCallback(Script* callback, TESForm* actorFilter, SInt32 procFilter)
{
    OCPH_Log("AddCallback: callback=0x%08X actorFilter=0x%08X procFilter=%d",
             callback, actorFilter, procFilter);

    if (!callback) return false;

    for (const CallbackEntry& entry : OnCombatProcedureHandler::g_callbacks) {
        if (entry.callback == callback &&
            entry.actorFilter == actorFilter &&
            entry.procFilter == procFilter) {
            OCPH_Log("AddCallback: Duplicate entry, skipping");
            return false;
        }
    }

    OnCombatProcedureHandler::g_callbacks.push_back({callback, actorFilter, procFilter});

    if (!OnCombatProcedureHandler::g_hookInstalled) {
        InitHooks();
    }

    OCPH_Log("AddCallback: Added (total: %d)", (int)OnCombatProcedureHandler::g_callbacks.size());
    return true;
}

static bool RemoveCallback(Script* callback, TESForm* actorFilter, SInt32 procFilter)
{
    if (!callback) return false;

    for (auto it = OnCombatProcedureHandler::g_callbacks.begin();
         it != OnCombatProcedureHandler::g_callbacks.end(); ++it) {
        if (it->callback == callback &&
            it->actorFilter == actorFilter &&
            it->procFilter == procFilter) {
            OnCombatProcedureHandler::g_callbacks.erase(it);
            OCPH_Log("RemoveCallback: Removed callback 0x%08X", callback);
            return true;
        }
    }
    return false;
}

static ParamInfo kParams_CombatProcHandler[4] = {
    {"callback",    kParamType_AnyForm, 0},
    {"addRemove",   kParamType_Integer, 0},
    {"actorFilter", kParamType_AnyForm, 1},
    {"procFilter",  kParamType_Integer, 1},
};

DEFINE_COMMAND_PLUGIN(SetOnCombatProcedureStartEventHandler,
    "Registers callback for combat procedure start. Callback: (actor, procType, isAction)",
    0, 4, kParams_CombatProcHandler);

bool Cmd_SetOnCombatProcedureStartEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;
    OCPH_Log("SetOnCombatProcedureStartEventHandler called");

    TESForm* callbackForm = nullptr;
    UInt32 addRemove = 0;
    TESForm* actorFilter = nullptr;
    SInt32 procFilter = -1;

    if (!g_ExtractArgsEx(
            reinterpret_cast<ParamInfo*>(paramInfo),
            scriptData,
            opcodeOffsetPtr,
            scriptObj,
            eventList,
            &callbackForm,
            &addRemove,
            &actorFilter,
            &procFilter))
    {
        OCPH_Log("Failed to extract args");
        return true;
    }

    OCPH_Log("Extracted: callback=0x%08X add=%d actorFilter=0x%08X procFilter=%d",
             callbackForm, addRemove, actorFilter, procFilter);

    if (!callbackForm) {
        OCPH_Log("Callback is null");
        return true;
    }

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) {
        OCPH_Log("Callback is not a script (typeID: %02X)", typeID);
        return true;
    }

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        if (AddCallback(callback, actorFilter, procFilter)) {
            *result = 1;
        }
    } else {
        if (RemoveCallback(callback, actorFilter, procFilter)) {
            *result = 1;
        }
    }

    return true;
}

bool OCPH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    char logPath[MAX_PATH];
    GetModuleFileNameA(nullptr, logPath, MAX_PATH);
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    strcat_s(logPath, "\\Data\\NVSE\\Plugins\\OnCombatProcedureHandler.log");
    g_ocphLogFile = fopen(logPath, "w");

    OCPH_Log("OnCombatProcedureHandler module initializing...");

    g_ocphPluginHandle = nvse->GetPluginHandle();

    g_ocphScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_ocphScript) {
        OCPH_Log("ERROR: Failed to get script interface");
        return false;
    }

    g_ExtractArgsEx = g_ocphScript->ExtractArgsEx;
    OCPH_Log("Script interface at 0x%08X", g_ocphScript);

    nvse->SetOpcodeBase(0x3B18);
    nvse->RegisterCommand(&kCommandInfo_SetOnCombatProcedureStartEventHandler);
    g_ocphOpcode = 0x3B18;

    OCPH_Log("Registered SetOnCombatProcedureStartEventHandler at opcode 0x%04X", g_ocphOpcode);
    OCPH_Log("OnCombatProcedureHandler module initialized successfully");

    return true;
}

unsigned int OCPH_GetOpcode()
{
    return g_ocphOpcode;
}
