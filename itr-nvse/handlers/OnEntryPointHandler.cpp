//hooks BGSEntryPoint::HandleEntryPoint to detect when perk bonuses are applied

#include <vector>
#include <unordered_map>
#include <Windows.h>

#include "OnEntryPointHandler.h"
#include "internal/NVSEMinimal.h"

class BGSPerk;
class BGSPerkEntry;
class BGSEntryPointPerkEntry;

enum { kFormType_BGSPerk = 0x81 };

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

    //context stack for reentrancy safety - nested perk evaluations won't clobber outer context
    struct EntryPointContext {
        UInt8 entryPoint;
        Actor* actor;
        TESForm* filterForm1;
        BGSEntryPointPerkEntry* perkEntry;
    };
    std::vector<EntryPointContext> g_contextStack;

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

// PlayerCharacter singleton
static Actor** g_thePlayer = (Actor**)0x011DEA3C;

//push context onto stack (called from asm hook before ExecuteFunction)
static void __cdecl PushContext(UInt32 entryPoint, Actor* actor, TESForm* filterForm1, BGSEntryPointPerkEntry* perkEntry)
{
    OnEntryPointHandler::EntryPointContext ctx;
    ctx.entryPoint = (UInt8)entryPoint;
    ctx.actor = actor;
    ctx.filterForm1 = filterForm1;
    ctx.perkEntry = perkEntry;
    OnEntryPointHandler::g_contextStack.push_back(ctx);
}

//pop context from stack (called from asm hook after dispatch)
static void __cdecl PopContext()
{
    if (!OnEntryPointHandler::g_contextStack.empty())
        OnEntryPointHandler::g_contextStack.pop_back();
}

// Dispatch the entry point event using top of context stack
static void DispatchEntryPointEvent()
{
    if (OnEntryPointHandler::g_contextStack.empty()) return;
    if (!OnEntryPointHandler::g_mapBuilt) return;
    if (OnEntryPointHandler::g_callbacks.empty()) return;

    //use top of stack (current context)
    const auto& ctx = OnEntryPointHandler::g_contextStack.back();

    if (!ctx.perkEntry) return;

    // Look up parent perk
    auto it = OnEntryPointHandler::g_entryToPerkMap.find((UInt32)ctx.perkEntry);
    if (it == OnEntryPointHandler::g_entryToPerkMap.end()) return;

    BGSPerk* perk = it->second;
    UInt8 entryPoint = ctx.entryPoint;
    Actor* actor = ctx.actor;
    TESForm* filter1 = ctx.filterForm1;

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

        //push context onto stack for reentrancy safety
        //PushContext(entryPoint, actor, filterForm1, perkEntry)
        push [ebp-0x74]           //perkEntry
        push [ebp+0x10]           //filterForm1
        push [ebp+0x0C]           //actor
        movzx eax, byte ptr [ebp+0x08]
        push eax                  //entryPoint (as UInt32)
        call PushContext
        add esp, 16               //clean up 4 args (cdecl)

        // Call the original ExecuteFunction
        // Stack is now exactly as original code expected
        call dword ptr [s_ExecuteFunctionAddr]

        // ExecuteFunction returned - NOW dispatch the event, then pop context
        pushad
        pushfd
        call DoDispatch
        call PopContext
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

void OEPH_ClearCallbacks()
{
    OnEntryPointHandler::g_callbacks.clear();
    OnEntryPointHandler::g_contextStack.clear();
}
