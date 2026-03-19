//hooks Actor::StealAlarm (0x8BFA40) to dispatch events when items are stolen

#include "OnStealHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"

namespace OnStealHandler {
    bool g_hookInstalled = false;

    static Actor* s_thief = nullptr;
    static TESObjectREFR* s_target = nullptr;
    static TESForm* s_item = nullptr;
    static SInt32 s_quantity = 0;
    static SInt32 s_value = 0;
    static TESObjectREFR* s_owner = nullptr;
}

constexpr UInt32 kAddr_StealAlarm = 0x8BFA40;
constexpr UInt32 kAddr_StealAlarmBody = 0x8BFA45;

static void DispatchStealEvent()
{
    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnSteal",
            reinterpret_cast<TESObjectREFR*>(OnStealHandler::s_thief),
            OnStealHandler::s_thief, OnStealHandler::s_target,
            OnStealHandler::s_item, OnStealHandler::s_owner,
            OnStealHandler::s_quantity);
}

static __declspec(naked) void StealAlarmHook()
{
    __asm
    {
        mov     OnStealHandler::s_thief, ecx
        mov     eax, [esp+4]
        mov     OnStealHandler::s_target, eax
        mov     eax, [esp+8]
        mov     OnStealHandler::s_item, eax
        mov     eax, [esp+0Ch]
        mov     OnStealHandler::s_quantity, eax
        mov     eax, [esp+10h]
        mov     OnStealHandler::s_value, eax
        mov     eax, [esp+14h]
        mov     OnStealHandler::s_owner, eax

        pushad
        pushfd
        call    DispatchStealEvent
        popfd
        popad

        push    ebp
        mov     ebp, esp
        push    0FFFFFFFFh
        mov     eax, kAddr_StealAlarmBody
        jmp     eax
    }
}

namespace OnStealHandler {
bool Init(void* nvseInterface)
{
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    if (!g_hookInstalled) {
        SafeWrite::WriteRelJump(kAddr_StealAlarm, (UInt32)StealAlarmHook);
        g_hookInstalled = true;
    }

    return true;
}
}
