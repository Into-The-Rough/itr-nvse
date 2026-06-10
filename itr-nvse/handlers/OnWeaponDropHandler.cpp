//hooks Actor::TryDropWeapon (0x89F580) to dispatch events when actors drop weapons

#include "OnWeaponDropHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"

namespace OnWeaponDropHandler {
    bool g_hookInstalled = false;
}

static Detours::JumpDetour s_tryDropWeaponDetour;
using TryDropWeapon_t = int(__thiscall*)(Actor*);
static TryDropWeapon_t s_originalTryDropWeapon = nullptr;

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

static void DispatchWeaponDropEvent(Actor* actor)
{
    if (!actor) return;

    auto* weapon = GetActorCurrentWeapon(actor);
    if (!weapon) return;

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEventThreadSafe("ITR:OnWeaponDrop",
            nullptr, reinterpret_cast<TESObjectREFR*>(actor),
            actor, weapon);
}

static int __fastcall TryDropWeaponHook(Actor* actor, void*)
{
    DispatchWeaponDropEvent(actor);
    return s_originalTryDropWeapon ? s_originalTryDropWeapon(actor) : 0;
}

namespace OnWeaponDropHandler {
bool Init(void* nvseInterface)
{
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    if (!g_hookInstalled) {
        if (!s_tryDropWeaponDetour.WriteRelJump((UInt32)Engine::Actor_TryDropWeapon, TryDropWeaponHook, 6))
            return false;
        s_originalTryDropWeapon = s_tryDropWeaponDetour.GetTrampoline<TryDropWeapon_t>();
        g_hookInstalled = true;
    }

    return true;
}
}
