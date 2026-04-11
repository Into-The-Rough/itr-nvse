//hooks Actor::SetAnimAction call at 0x894081 in FiresWeapon when weapon jams

#include "OnWeaponJamHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"

static thread_local Actor* g_jamActor = nullptr;

namespace OnWeaponJamHandler {
    bool g_hookInstalled = false;
}

static TESObjectWEAP* GetActorCurrentWeapon(Actor* actor)
{
    if (!actor) return nullptr;

    UInt32 pProcess = *(UInt32*)((UInt8*)actor + 0x68);
    if (!pProcess) return nullptr;

    UInt32 vtable = *(UInt32*)pProcess;
    if (!vtable) return nullptr;

    typedef UInt32 (__thiscall *GetCurrentWeapon_t)(UInt32 pProcess);
    UInt32 itemChange = ((GetCurrentWeapon_t)(*(UInt32*)(vtable + 82 * 4)))(pProcess);
    if (!itemChange) return nullptr;

    return (TESObjectWEAP*)(*(UInt32*)(itemChange + 0x08));
}

static void __cdecl SaveJamActor(Actor* actor)
{
    g_jamActor = actor;
}

static void __cdecl DispatchWeaponJamEvent()
{
    if (!g_jamActor) return;

    TESObjectWEAP* weapon = GetActorCurrentWeapon(g_jamActor);
    if (!weapon) return;

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEventThreadSafe("ITR:OnWeaponJam",
            nullptr, reinterpret_cast<TESObjectREFR*>(g_jamActor),
            g_jamActor, weapon);
}

static UInt32 s_SetAnimActionAddr = 0x8A73E0;

static __declspec(naked) void Hook_SetAnimAction_Jam()
{
    __asm {
        push ecx                              //stash thiscall this (actor) for restore
        push ecx                              //cdecl arg for SaveJamActor
        call SaveJamActor
        add esp, 4                            //cdecl cleanup of pushed arg
        pop ecx                               //restore actor for downstream SetAnimAction
        pushad
        pushfd
        call DispatchWeaponJamEvent           //fires event via the stashed g_jamActor
        popfd
        popad
        jmp dword ptr [s_SetAnimActionAddr]   //tail into real SetAnimAction, ecx already set
    }
}

namespace OnWeaponJamHandler {
bool Init(void* nvseInterface)
{
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    if (!g_hookInstalled) {
        SafeWrite::WriteRelCall(0x894081, (UInt32)Hook_SetAnimAction_Jam);
        g_hookInstalled = true;
    }

    return true;
}
}
