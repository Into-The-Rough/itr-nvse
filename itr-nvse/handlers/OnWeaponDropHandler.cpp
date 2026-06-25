//hooks Actor::TryDropWeapon (0x89F580) to dispatch events when actors drop weapons

#include "OnWeaponDropHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EngineFunctions.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"
#include "internal/GameLayout.h"

namespace OnWeaponDropHandler {
    bool g_hookInstalled = false;
}

static Detours::JumpDetour s_tryDropWeaponDetour;
using TryDropWeapon_t = int(__thiscall*)(Actor*);
static TryDropWeapon_t s_originalTryDropWeapon = nullptr;

static TESObjectWEAP* GetActorCurrentWeapon(Actor* actor)
{
    if (!actor) return nullptr;

    BaseProcess* process = actor->baseProcess;
    if (!process) return nullptr;

    auto* weaponInfo = process->GetWeaponInfo();
    return weaponInfo ? weaponInfo->weapon : nullptr;
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
