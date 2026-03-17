//hooks Actor::TryDropWeapon (0x89F580) to dispatch events when actors drop weapons

#include "OnWeaponDropHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"

namespace OnWeaponDropHandler {
    bool g_hookInstalled = false;
    static thread_local Actor* s_actor = nullptr;
}

constexpr UInt32 kAddr_TryDropWeapon = 0x89F580;
constexpr UInt32 kAddr_TryDropWeaponBody = 0x89F586;

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

static void __cdecl SaveDropActor(Actor* actor)
{
    OnWeaponDropHandler::s_actor = actor;
}

static void __cdecl DispatchWeaponDropEvent()
{
    auto* actor = OnWeaponDropHandler::s_actor;
    if (!actor) return;

    auto* weapon = GetActorCurrentWeapon(actor);
    if (!weapon) return;

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEventThreadSafe("ITR:OnWeaponDrop",
            nullptr, reinterpret_cast<TESObjectREFR*>(actor),
            actor, weapon);
}

static __declspec(naked) void TryDropWeaponHook()
{
    __asm
    {
        push    ecx
        push    ecx
        call    SaveDropActor
        add     esp, 4
        pop     ecx

        pushad
        pushfd
        call    DispatchWeaponDropEvent
        popfd
        popad

        push    ebp
        mov     ebp, esp
        sub     esp, 3Ch
        mov     eax, kAddr_TryDropWeaponBody
        jmp     eax
    }
}

bool OWDH_Init(void* nvseInterface)
{
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    if (!OnWeaponDropHandler::g_hookInstalled) {
        SafeWrite::WriteRelJump(kAddr_TryDropWeapon, (UInt32)TryDropWeaponHook);
        SafeWrite::Write8(kAddr_TryDropWeapon + 5, 0x90);
        OnWeaponDropHandler::g_hookInstalled = true;
    }

    return true;
}
