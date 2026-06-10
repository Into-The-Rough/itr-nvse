//hooks Actor::SetAnimAction call at 0x894081 in FiresWeapon when weapon jams

#include "OnWeaponJamHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"

namespace OnWeaponJamHandler {
    bool g_hookInstalled = false;
}

static Detours::CallDetour s_setAnimActionCall;
using SetAnimAction_t = int(__thiscall*)(Actor*, int, void*);

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

static void DispatchWeaponJamEvent(Actor* actor)
{
    if (!actor) return;

    TESObjectWEAP* weapon = GetActorCurrentWeapon(actor);
    if (!weapon) return;

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEventThreadSafe("ITR:OnWeaponJam",
            nullptr, reinterpret_cast<TESObjectREFR*>(actor),
            actor, weapon);
}

static int __fastcall Hook_SetAnimAction_Jam(Actor* actor, void*, int action, void* sequence)
{
    DispatchWeaponJamEvent(actor);

    auto original = reinterpret_cast<SetAnimAction_t>(s_setAnimActionCall.GetOverwrittenAddr());
    return original ? original(actor, action, sequence) : 0;
}

namespace OnWeaponJamHandler {
bool Init(void* nvseInterface)
{
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    if (!g_hookInstalled) {
        if (!s_setAnimActionCall.WriteRelCall(0x894081, Hook_SetAnimAction_Jam))
            return false;
        g_hookInstalled = true;
    }

    return true;
}
}
