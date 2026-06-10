//hooks Actor::StealAlarm (0x8BFA40) to dispatch events when items are stolen

#include "OnStealHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"
#include "internal/Detours.h"

namespace OnStealHandler {
    bool g_hookInstalled = false;
}

constexpr UInt32 kAddr_StealAlarm = 0x8BFA40;
static Detours::JumpDetour s_stealAlarmDetour;
typedef void(__thiscall* StealAlarm_t)(Actor*, TESObjectREFR*, TESForm*, SInt32, UInt32, TESObjectREFR*);
static StealAlarm_t s_stealAlarm = nullptr;

static void __fastcall DispatchStealEvent(Actor* thief, TESObjectREFR* target, TESForm* item, SInt32 quantity, TESObjectREFR* owner)
{
    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnSteal",
            reinterpret_cast<TESObjectREFR*>(thief),
            thief, target, item, owner, quantity);
}

static void __fastcall StealAlarmHook(Actor* thief, void*, TESObjectREFR* target, TESForm* item, SInt32 quantity, UInt32 arg4, TESObjectREFR* owner)
{
    DispatchStealEvent(thief, target, item, quantity, owner);
    if (s_stealAlarm)
        s_stealAlarm(thief, target, item, quantity, arg4, owner);
}

namespace OnStealHandler {
bool Init(void* nvseInterface)
{
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    if (!g_hookInstalled) {
        if (!s_stealAlarmDetour.WriteRelJump(kAddr_StealAlarm, StealAlarmHook, 5))
            return false;
        s_stealAlarm = s_stealAlarmDetour.GetTrampoline<StealAlarm_t>();
        g_hookInstalled = true;
    }

    return true;
}
}
