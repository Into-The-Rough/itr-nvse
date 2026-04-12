//hooks Actor::StealAlarm (0x8BFA40) to dispatch events when items are stolen

#include "OnStealHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"
#include "internal/SafeWrite.h"

namespace OnStealHandler {
    bool g_hookInstalled = false;
}

constexpr UInt32 kAddr_StealAlarm = 0x8BFA40;
constexpr UInt32 kAddr_StealAlarmBody = 0x8BFA45;

static void __fastcall DispatchStealEvent(Actor* thief, TESObjectREFR* target, TESForm* item, SInt32 quantity, TESObjectREFR* owner)
{
    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnSteal",
            reinterpret_cast<TESObjectREFR*>(thief),
            thief, target, item, owner, quantity);
}

static __declspec(naked) void StealAlarmHook()
{
    __asm
    {
        pushfd                          //save flags
        pushad                          //save all regs (32 bytes, pushad frame starts at esp)
        mov     ecx, [esp+18h]          //thief = saved ecx slot in pushad frame (thiscall this)
        mov     edx, [esp+28h]          //target = stack arg1, past pushad(32)+pushfd(4)+retaddr(4)
        mov     eax, [esp+2Ch]          //item = stack arg2
        mov     ebx, [esp+30h]          //quantity = stack arg3
        mov     esi, [esp+38h]          //owner = stack arg5 (arg4 at +34h is not forwarded by this hook)
        push    esi                     //owner  (fastcall stack[2])
        push    ebx                     //quantity  (fastcall stack[1])
        push    eax                     //item  (fastcall stack[0])
        call    DispatchStealEvent      //ecx=thief, edx=target already loaded
        popad                           //restore regs (fastcall callee popped stack args)
        popfd                           //restore flags

        push    ebp                     //replay stolen prologue bytes
        mov     ebp, esp
        push    0FFFFFFFFh              //SEH sentinel
        mov     eax, kAddr_StealAlarmBody
        jmp     eax                     //resume real StealAlarm past the 5-byte jmp patch
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
