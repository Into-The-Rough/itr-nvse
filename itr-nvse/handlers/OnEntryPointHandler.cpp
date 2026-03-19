//hooks BGSEntryPoint::HandleEntryPoint to detect when perk bonuses are applied

#include <unordered_map>
#include <vector>

#include "OnEntryPointHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"

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

class BGSPerkEntry {
public:
    void** vtable;
    UInt8 rank;
    UInt8 priority;
    UInt16 type;
};

class BGSEntryPointPerkEntry : public BGSPerkEntry {
public:
    UInt8 entryPoint;
    UInt8 function;
    UInt8 conditionTabs;
    UInt8 pad0B;
    void* data;
    void* conditions;
};

class BGSPerk {
public:
    UInt8 pad00[0x48];
    tList<BGSPerkEntry> entries;
};

struct DataHandler {
    UInt8 pad[0x3A0];
};

namespace OnEntryPointHandler {
    //double-buffered: build into new map, swap pointer atomically
    //readers always see a complete map, never a half-built one
    typedef std::unordered_map<UInt32, BGSPerk*> EntryMap;
    static EntryMap g_mapA, g_mapB;
    static EntryMap* volatile g_activeMap = nullptr;

    struct EntryPointContext {
        UInt8 entryPoint;
        Actor* actor;
        TESForm* filterForm1;
        BGSEntryPointPerkEntry* perkEntry;
        UInt32 returnAddr;
    };

    //per-thread to avoid races between AI worker threads
    static thread_local std::vector<EntryPointContext> g_contextStack;
    static thread_local UInt32 g_savedReturnAddr = 0;

    bool g_hookInstalled = false;
    static bool g_useMapB = false; //which buffer to build into next
}

namespace OnEntryPointHandler {
void BuildEntryMap()
{
    using namespace OnEntryPointHandler;

    //build into the inactive buffer while readers use the active one
    EntryMap* buildMap = g_useMapB ? &g_mapB : &g_mapA;
    buildMap->clear();

    DataHandler** pDataHandler = (DataHandler**)0x11C3F2C;
    if (!pDataHandler || !*pDataHandler) return;

    tList<BGSPerk>* perkList = (tList<BGSPerk>*)((UInt8*)*pDataHandler + 0x178);

    tListNode<BGSPerk>* perkNode = (tListNode<BGSPerk>*)perkList;
    while (perkNode && perkNode->data) {
        BGSPerk* perk = perkNode->data;

        tListNode<BGSPerkEntry>* node = perk->entries.Head();
        while (node && node->data) {
            BGSPerkEntry* entry = node->data;

            if (*(UInt32*)entry == 0x1046D0C)
                (*buildMap)[(UInt32)entry] = perk;

            node = node->next;
        }

        perkNode = perkNode->next;
    }

    //atomic swap - readers instantly see the new complete map
    InterlockedExchangePointer((volatile PVOID*)&g_activeMap, buildMap);
    g_useMapB = !g_useMapB;
}
}

static UInt32 s_ExecuteFunctionAddr = 0x5E5B40;

static void __cdecl PushContext(UInt32 entryPoint, Actor* actor, TESForm* filterForm1, BGSEntryPointPerkEntry* perkEntry)
{
    OnEntryPointHandler::EntryPointContext ctx;
    ctx.entryPoint = (UInt8)entryPoint;
    ctx.actor = actor;
    ctx.filterForm1 = filterForm1;
    ctx.perkEntry = perkEntry;
    ctx.returnAddr = OnEntryPointHandler::g_savedReturnAddr;
    OnEntryPointHandler::g_contextStack.push_back(ctx);
}

static void DispatchEntryPointEvent()
{
    if (OnEntryPointHandler::g_contextStack.empty()) return;

    auto* map = OnEntryPointHandler::g_activeMap;
    if (!map) return;

    const auto& ctx = OnEntryPointHandler::g_contextStack.back();
    if (!ctx.perkEntry) return;

    auto it = map->find((UInt32)ctx.perkEntry);
    if (it == map->end()) return;

    BGSPerk* perk = it->second;

    if (g_eventManagerInterface && ctx.actor)
        g_eventManagerInterface->DispatchEventThreadSafe("ITR:OnEntryPoint",
            nullptr, reinterpret_cast<TESObjectREFR*>(ctx.actor),
            (TESForm*)perk, (int)ctx.entryPoint, (TESForm*)ctx.actor, ctx.filterForm1);
}

static void __cdecl DoDispatchAndPop()
{
    DispatchEntryPointEvent();
    if (!OnEntryPointHandler::g_contextStack.empty()) {
        OnEntryPointHandler::g_savedReturnAddr = OnEntryPointHandler::g_contextStack.back().returnAddr;
        OnEntryPointHandler::g_contextStack.pop_back();
    }
}

static void __cdecl SaveReturnAddr(UInt32 addr)
{
    OnEntryPointHandler::g_savedReturnAddr = addr;
}

static UInt32 __cdecl GetReturnAddr()
{
    return OnEntryPointHandler::g_savedReturnAddr;
}

static __declspec(naked) void Hook_ExecuteFunctionCall()
{
    __asm {
        pop eax
        push eax
        call SaveReturnAddr
        add esp, 4

        push [ebp-0x74]
        push [ebp+0x10]
        push [ebp+0x0C]
        movzx eax, byte ptr [ebp+0x08]
        push eax
        call PushContext
        add esp, 16

        call dword ptr [s_ExecuteFunctionAddr]

        pushad
        pushfd
        call DoDispatchAndPop
        popfd
        popad

        call GetReturnAddr
        jmp eax
    }
}

namespace OnEntryPointHandler {
bool Init(void* nvseInterface)
{
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    if (!g_hookInstalled) {
        SafeWrite::WriteRelCall(0x5E5AB9, (UInt32)Hook_ExecuteFunctionCall);
        g_hookInstalled = true;
    }

    return true;
}
}
