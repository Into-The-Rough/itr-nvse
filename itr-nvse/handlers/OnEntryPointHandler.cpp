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

template <typename T>
struct tListNode {
    T* data;
    tListNode<T>* next;
};

template <typename T>
struct tList {
    tListNode<T> first;

    tListNode<T>* Head() { return &first; }
};

class BGSPerkEntry { //0x08

public:
    void** vtable;      // 00
    UInt8 rank;         // 04
    UInt8 priority;     // 05
    UInt16 type;        // 06
};

class BGSEntryPointPerkEntry : public BGSPerkEntry { //0x14

public:
    UInt8 entryPoint;   // 08
    UInt8 function;     // 09
    UInt8 conditionTabs;// 0A
    UInt8 pad0B;        // 0B
    void* data;         // 0C
    void* conditions;   // 10
};

class BGSPerk { //0x50
public:
    UInt8 pad00[0x48];                  //00-47 (TESForm+TESFullName+TESDescription+TESIcon+PerkData+ConditionList)
    tList<BGSPerkEntry> entries;        //48
};

struct DataHandler {
    UInt8 pad[0x3A0];
};

static PluginHandle g_oephPluginHandle = kPluginHandle_Invalid;
static NVSEScriptInterface* g_oephScript = nullptr;
static bool (*g_ExtractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
static UInt32 g_oephOpcode = 0;

namespace OnEntryPointHandler {
    std::unordered_map<UInt32, BGSPerk*> g_entryToPerkMap;

    struct CallbackInfo {
        Script* callback;
        SInt32 entryPointFilter;
    };
    std::vector<CallbackInfo> g_callbacks;

    //context stack for reentrancy safety - nested perk evaluations won't clobber outer context
    struct EntryPointContext {
        UInt8 entryPoint;
        Actor* actor;
        TESForm* filterForm1;
        BGSEntryPointPerkEntry* perkEntry;
        UInt32 returnAddr;
    };
    std::vector<EntryPointContext> g_contextStack;

    bool g_hookInstalled = false;
    bool g_mapBuilt = false;
}

void OEPH_BuildEntryMap()
{
    OnEntryPointHandler::g_entryToPerkMap.clear();

    DataHandler** pDataHandler = (DataHandler**)0x11C3F2C;
    if (!pDataHandler || !*pDataHandler) return;

    tList<BGSPerk>* perkList = (tList<BGSPerk>*)((UInt8*)*pDataHandler + 0x178); //perkList
    UInt32 perkCount = 0;
    UInt32 totalEntries = 0;

    tListNode<BGSPerk>* perkNode = (tListNode<BGSPerk>*)perkList;
    while (perkNode && perkNode->data) {
        BGSPerk* perk = perkNode->data;
        perkCount++;

        tListNode<BGSPerkEntry>* node = perk->entries.Head();
        while (node && node->data) {
            BGSPerkEntry* entry = node->data;

            if (*(UInt32*)entry == 0x1046D0C) { //vtbl BGSEntryPointPerkEntry
                OnEntryPointHandler::g_entryToPerkMap[(UInt32)entry] = perk;
                totalEntries++;
            }

            node = node->next;
        }

        perkNode = perkNode->next;
    }

    OnEntryPointHandler::g_mapBuilt = true;
}

static Actor** g_thePlayer = (Actor**)0x011DEA3C;

static UInt32 s_ExecuteFunctionAddr = 0x5E5B40; //ExecuteFunction
static UInt32 s_savedReturnAddr = 0;

//push context onto stack (called from asm hook before ExecuteFunction)
//s_savedReturnAddr must be set before calling this
static void __cdecl PushContext(UInt32 entryPoint, Actor* actor, TESForm* filterForm1, BGSEntryPointPerkEntry* perkEntry)
{
    OnEntryPointHandler::EntryPointContext ctx;
    ctx.entryPoint = (UInt8)entryPoint;
    ctx.actor = actor;
    ctx.filterForm1 = filterForm1;
    ctx.perkEntry = perkEntry;
    ctx.returnAddr = s_savedReturnAddr;
    OnEntryPointHandler::g_contextStack.push_back(ctx);
}

static void DispatchEntryPointEvent()
{
    if (OnEntryPointHandler::g_contextStack.empty()) return;
    if (!OnEntryPointHandler::g_mapBuilt) return;
    if (OnEntryPointHandler::g_callbacks.empty()) return;

    //use top of stack (current context)
    const auto& ctx = OnEntryPointHandler::g_contextStack.back();

    if (!ctx.perkEntry) return;

    auto it = OnEntryPointHandler::g_entryToPerkMap.find((UInt32)ctx.perkEntry);
    if (it == OnEntryPointHandler::g_entryToPerkMap.end()) return;

    BGSPerk* perk = it->second;
    UInt8 entryPoint = ctx.entryPoint;
    Actor* actor = ctx.actor;
    TESForm* filter1 = ctx.filterForm1;

    //snapshot for reentrancy safety (perk chains can re-enter HandleEntryPoint)
    const auto callbackSnapshot = OnEntryPointHandler::g_callbacks;

    for (const auto& cb : callbackSnapshot) {
        if (cb.entryPointFilter != -1 && cb.entryPointFilter != entryPoint) {
            continue;
        }

        if (g_oephScript && cb.callback) {
            g_oephScript->CallFunctionAlt(
                cb.callback,
                reinterpret_cast<TESObjectREFR*>(actor),
                4,
                perk,
                entryPoint,
                actor,
                filter1
            );
        }
    }
}


//dispatch event then pop context, restoring return addr from stack
static void __cdecl DoDispatchAndPop()
{
    DispatchEntryPointEvent();
    if (!OnEntryPointHandler::g_contextStack.empty()) {
        s_savedReturnAddr = OnEntryPointHandler::g_contextStack.back().returnAddr;
        OnEntryPointHandler::g_contextStack.pop_back();
    }
}

static __declspec(naked) void Hook_ExecuteFunctionCall()
{
    __asm {
        //ebp = HandleEntryPoint's frame
        //[ebp+0x08]=entryPointID, [ebp+0x0C]=actor, [ebp+0x10]=filter1, [ebp-0x74]=perkEntry
        pop eax
        mov s_savedReturnAddr, eax

        push [ebp-0x74]           //perkEntry
        push [ebp+0x10]           //filterForm1
        push [ebp+0x0C]           //actor
        movzx eax, byte ptr [ebp+0x08]
        push eax                  //entryPoint
        call PushContext
        add esp, 16

        call dword ptr [s_ExecuteFunctionAddr]

        pushad
        pushfd
        call DoDispatchAndPop
        popfd
        popad

        jmp dword ptr [s_savedReturnAddr]
    }
}

static void InitHooks()
{
    if (OnEntryPointHandler::g_hookInstalled) return;

    SafeWrite::WriteRelCall(0x5E5AB9, (UInt32)Hook_ExecuteFunctionCall); //ExecuteFunction call site

    OnEntryPointHandler::g_hookInstalled = true;
}

static bool AddCallback(Script* callback, SInt32 entryPointFilter)
{
    if (!callback) return false;

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
    {"entryPointFilter", kParamType_Integer, 1},
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
    SInt32 entryPointFilter = -1;

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

    g_oephScript = reinterpret_cast<NVSEScriptInterface*>(
        nvse->QueryInterface(kInterface_Script));

    if (!g_oephScript) return false;

    g_ExtractArgsEx = g_oephScript->ExtractArgsEx;

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
