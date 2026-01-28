//hooks BGSEntryPoint::HandleEntryPoint to detect when perk bonuses are applied

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <Windows.h>

#include "OnEntryPointHandler.h"


class TESForm;
class TESObjectREFR;
class Actor;
class Script;
class ScriptEventList;
class BGSPerk;
class BGSPerkEntry;
class BGSEntryPointPerkEntry;

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
    kFormType_BGSPerk = 0x81,
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

    inline void WriteRelCall(UInt32 src, UInt32 dst) {
        Write8(src, 0xE8);
        Write32(src + 1, dst - src - 5);
    }

    inline void WriteRelJump(UInt32 src, UInt32 dst) {
        Write8(src, 0xE9);
        Write32(src + 1, dst - src - 5);
    }
}

// tList node structure
template <typename T>
struct tListNode {
    T* data;
    tListNode<T>* next;
};

// tList structure
template <typename T>
struct tList {
    tListNode<T> first;

    tListNode<T>* Head() { return &first; }
};

// BGSPerkEntry base class (0x08 bytes)
class BGSPerkEntry {
public:
    void** vtable;      // 00
    UInt8 rank;         // 04
    UInt8 priority;     // 05
    UInt16 type;        // 06
};

// BGSEntryPointPerkEntry (0x14 bytes)
class BGSEntryPointPerkEntry : public BGSPerkEntry {
public:
    UInt8 entryPoint;   // 08
    UInt8 function;     // 09
    UInt8 conditionTabs;// 0A
    UInt8 pad0B;        // 0B
    void* data;         // 0C
    void* conditions;   // 10
};

// BGSPerk structure (partial, just what we need)
// Total size is 0x50, entries list at offset 0x48
class BGSPerk {
public:
    // TESForm base (0x18 bytes)
    // TESFullName at 0x18 (0x0C bytes)
    // TESDescription at 0x24 (0x08 bytes)
    // TESIcon at 0x2C (0x0C bytes)
    // PerkData at 0x38 (0x08 bytes)
    // ConditionList at 0x40 (0x08 bytes)
    // entries at 0x48
    UInt8 pad00[0x48];                  // 00-47
    tList<BGSPerkEntry> entries;        // 48
};

// DataHandler for iterating forms
struct DataHandler {
    UInt8 pad[0x3A0];
    // More fields we don't need
};

static PluginHandle g_oephPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_oephScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_oephOpcode = 0;

namespace OnEntryPointHandler {
    // Map from perk entry pointer to parent perk
    std::unordered_map<UInt32, BGSPerk*> g_entryToPerkMap;

    // Callback info with optional entry point filter
    struct CallbackInfo {
        Script* callback;
        SInt32 entryPointFilter;  // -1 = all entry points
    };
    std::vector<CallbackInfo> g_callbacks;

    // Current context (captured at HandleEntryPoint start)
    UInt8 g_currentEntryPoint = 0;
    Actor* g_currentActor = nullptr;
    TESForm* g_currentFilterForm1 = nullptr;
    TESForm* g_currentFilterForm2 = nullptr;

    bool g_hookInstalled = false;
    bool g_mapBuilt = false;
}

// VTable address for BGSEntryPointPerkEntry
constexpr UInt32 kVtbl_BGSEntryPointPerkEntry = 0x1046D0C;

// Hook addresses (IDA decimal -> hex conversion verified)
// HandleEntryPoint: 6183152 decimal = 0x5E58F0 hex
// ExecuteFunction:  6183744 decimal = 0x5E5B40 hex
// Call site:        6183609 decimal = 0x5E5AB9 hex
constexpr UInt32 kAddr_HandleEntryPoint = 0x5E58F0;
constexpr UInt32 kAddr_ExecuteFunctionCall = 0x5E5AB9;
constexpr UInt32 kAddr_ExecuteFunction = 0x5E5B40;

// Return address after call (0x5E5AB9 + 5 = 0x5E5ABE)
constexpr UInt32 kAddr_AfterExecuteFunctionCall = 0x5E5ABE;

// Addresses for iterating forms
constexpr UInt32 kAddr_DataHandler = 0x011C3F2C;

void OEPH_BuildEntryMap()
{
    OnEntryPointHandler::g_entryToPerkMap.clear();

    // Get DataHandler
    DataHandler** pDataHandler = (DataHandler**)kAddr_DataHandler;
    if (!pDataHandler || !*pDataHandler) return;

    // DataHandler has tList<BGSPerk> perkList at offset 0x178
    tList<BGSPerk>* perkList = (tList<BGSPerk>*)((UInt8*)*pDataHandler + 0x178);

    UInt32 perkCount = 0;
    UInt32 totalEntries = 0;

    // Iterate the perk list
    tListNode<BGSPerk>* perkNode = (tListNode<BGSPerk>*)perkList;
    while (perkNode && perkNode->data) {
        BGSPerk* perk = perkNode->data;
        perkCount++;

        // Iterate perk entries
        tListNode<BGSPerkEntry>* node = perk->entries.Head();
        while (node && node->data) {
            BGSPerkEntry* entry = node->data;

            // Check if it's an entry point perk entry (check vtable)
            if (*(UInt32*)entry == kVtbl_BGSEntryPointPerkEntry) {
                OnEntryPointHandler::g_entryToPerkMap[(UInt32)entry] = perk;
                totalEntries++;
            }

            node = node->next;
        }

        perkNode = perkNode->next;
    }

    OnEntryPointHandler::g_mapBuilt = true;
}

// Globals for hook communication
static BGSEntryPointPerkEntry* g_currentPerkEntry = nullptr;

// PlayerCharacter singleton
static Actor** g_thePlayer = (Actor**)0x011DEA3C;

// Dispatch the entry point event
static void DispatchEntryPointEvent()
{
    if (!g_currentPerkEntry) return;
    if (!OnEntryPointHandler::g_mapBuilt) return;
    if (OnEntryPointHandler::g_callbacks.empty()) return;

    // Look up parent perk
    auto it = OnEntryPointHandler::g_entryToPerkMap.find((UInt32)g_currentPerkEntry);
    if (it == OnEntryPointHandler::g_entryToPerkMap.end()) return;

    BGSPerk* perk = it->second;
    UInt8 entryPoint = OnEntryPointHandler::g_currentEntryPoint;
    Actor* actor = OnEntryPointHandler::g_currentActor;
    TESForm* filter1 = OnEntryPointHandler::g_currentFilterForm1;

    // Fire callbacks that match the filter
    for (const auto& cb : OnEntryPointHandler::g_callbacks) {
        if (cb.entryPointFilter != -1 && cb.entryPointFilter != entryPoint) {
            continue;  // Filter doesn't match
        }

        if (g_oephScript && cb.callback) {
            // Call UDF with: perk, entryPointID, actor, filterForm1
            g_oephScript->CallFunctionAlt(
                cb.callback,
                reinterpret_cast<TESObjectREFR*>(actor),
                4,
                perk,           // arg1: the perk
                entryPoint,     // arg2: entry point ID
                actor,          // arg3: actor (usually player)
                filter1         // arg4: filter form (weapon, etc.)
            );
        }
    }
}


// Function to dispatch event - called from asm hook
static void __cdecl DoDispatch()
{
    DispatchEntryPointEvent();
}

// Address to jump to for the call site hook
static UInt32 s_ExecuteFunctionAddr = kAddr_ExecuteFunction;

// Saved return address for post-call dispatch
static UInt32 s_savedReturnAddr = 0;

// Hook at call site inside HandleEntryPoint (0x5E5AB9)
// This lets us access HandleEntryPoint's stack frame to get perk entry
static __declspec(naked) void Hook_ExecuteFunctionCall()
{
    __asm {
        // Stack: [esp] = return address (0x5E5ABE) pushed by "call Hook_ExecuteFunctionCall"
        // ebp still points to HandleEntryPoint's frame
        // [ebp+0x08] = entryPointID (first arg to HandleEntryPoint)
        // [ebp+0x0C] = actor (second arg to HandleEntryPoint)
        // [ebp+0x10] = filter1 (third arg, vararg)
        // [ebp-0x74] = perk entry (local var v8)

        // Pop the return address so stack is correct for ExecuteFunction
        pop eax
        mov s_savedReturnAddr, eax

        // Capture entry point ID (it's a UInt8)
        movzx eax, byte ptr [ebp+0x08]
        mov OnEntryPointHandler::g_currentEntryPoint, al

        // Capture actor
        mov eax, [ebp+0x0C]
        mov OnEntryPointHandler::g_currentActor, eax

        // Capture filter form 1 (weapon, etc.)
        mov eax, [ebp+0x10]
        mov OnEntryPointHandler::g_currentFilterForm1, eax

        // Capture perk entry
        mov eax, [ebp-0x74]
        mov g_currentPerkEntry, eax

        // Call the original ExecuteFunction
        // Stack is now exactly as original code expected
        call dword ptr [s_ExecuteFunctionAddr]

        // ExecuteFunction returned - NOW dispatch the event
        pushad
        pushfd
        call DoDispatch
        popfd
        popad

        // Jump back to HandleEntryPoint (not ret, since we popped the return addr)
        jmp dword ptr [s_savedReturnAddr]
    }
}

static void InitHooks()
{
    if (OnEntryPointHandler::g_hookInstalled) return;

    // Hook the call to ExecuteFunction inside HandleEntryPoint
    // Call site: 6183609 decimal = 0x5E5AB9 hex
    SafeWrite::WriteRelCall(kAddr_ExecuteFunctionCall, (UInt32)Hook_ExecuteFunctionCall);

    OnEntryPointHandler::g_hookInstalled = true;
}

static bool AddCallback(Script* callback, SInt32 entryPointFilter)
{
    if (!callback) return false;

    // Check for duplicate
    for (const auto& cb : OnEntryPointHandler::g_callbacks) {
        if (cb.callback == callback && cb.entryPointFilter == entryPointFilter) {
            return false;
        }
    }

    OnEntryPointHandler::g_callbacks.push_back({callback, entryPointFilter});

    if (!OnEntryPointHandler::g_hookInstalled) {
        InitHooks();
    }

    return true;
}

static bool RemoveCallback(Script* callback, SInt32 entryPointFilter)
{
    if (!callback) return false;

    for (auto it = OnEntryPointHandler::g_callbacks.begin();
         it != OnEntryPointHandler::g_callbacks.end(); ++it) {
        if (it->callback == callback && it->entryPointFilter == entryPointFilter) {
            OnEntryPointHandler::g_callbacks.erase(it);
            return true;
        }
    }
    return false;
}

static ParamInfo kParams_EntryPointHandler[3] = {
    {"callback",         kParamType_AnyForm, 0},
    {"addRemove",        kParamType_Integer, 0},
    {"entryPointFilter", kParamType_Integer, 1},  // Optional: -1 or omit for all
};

DEFINE_COMMAND_PLUGIN(SetOnEntryPointEventHandler,
    "Registers/unregisters a callback for perk entry point events. "
    "Callback receives: perk, entryPointID, actor, filterForm. "
    "Optional filter: 0=WeaponDamage, 37=ReloadSpeed, -1=all",
    0, 3, kParams_EntryPointHandler);

bool Cmd_SetOnEntryPointEventHandler_Execute(COMMAND_ARGS)
{
    *result = 0;

    TESForm* callbackForm = nullptr;
    UInt32 addRemove = 0;
    SInt32 entryPointFilter = -1;  // Default: all entry points

    if (!g_ExtractArgsEx(
            reinterpret_cast<ParamInfo*>(paramInfo),
            scriptData,
            opcodeOffsetPtr,
            scriptObj,
            eventList,
            &callbackForm,
            &addRemove,
            &entryPointFilter))
    {
        return true;
    }

    if (!callbackForm) return true;

    UInt8 typeID = *((UInt8*)callbackForm + 4);
    if (typeID != kFormType_Script) return true;

    Script* callback = reinterpret_cast<Script*>(callbackForm);

    if (addRemove) {
        if (AddCallback(callback, entryPointFilter)) {
            *result = 1;
        }
    } else {
        if (RemoveCallback(callback, entryPointFilter)) {
            *result = 1;
        }
    }

    return true;
}

bool OEPH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);

    if (nvse->isEditor) return false;

    g_oephPluginHandle = nvse->GetPluginHandle();

    // Get script interface
    g_oephScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_oephScript) return false;

    g_ExtractArgsEx = g_oephScript->ExtractArgsEx;

    // Register command at opcode 0x3B17
    nvse->SetOpcodeBase(0x4014);
    nvse->RegisterCommand(&kCommandInfo_SetOnEntryPointEventHandler);
    g_oephOpcode = 0x4014;

    return true;
}

unsigned int OEPH_GetOpcode()
{
    return g_oephOpcode;
}
