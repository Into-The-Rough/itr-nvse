//fires when an actor becomes frenzied (brain condition goes to 0)

#include "OnFrenzyHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/EngineHelpers.h"
#include "internal/GameLayout.h"

constexpr UInt32 kAddr_LimbCondition_HandleChange = 0x8B9240;
static Detours::JumpDetour s_detour;

static void DispatchFrenzyEvent(Actor* actor) {
    if (!actor) return;

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEventThreadSafe("ITR:OnFrenzy", nullptr, nullptr, (TESForm*)actor);
}

static void __cdecl Hook_LimbCondition_HandleChange(
    void* actorValueOwner,
    UInt32 avIndex,
    float oldValue,
    float delta,
    void* attackerAVO
) {
    Actor* actor = ActorValueOwnerToActor(static_cast<ActorValueOwner*>(actorValueOwner));

    bool willFrenzy = false;
    if (avIndex == 31 && oldValue > 0.0f && delta < 0.0f) {
        float newValue = oldValue + delta;
        if (newValue <= 0.0f)
            willFrenzy = true;
    }

    typedef void(__cdecl* HandleChange_t)(void*, UInt32, float, float, void*);
    s_detour.GetTrampoline<HandleChange_t>()(actorValueOwner, avIndex, oldValue, delta, attackerAVO);

    if (willFrenzy && actor)
        DispatchFrenzyEvent(actor);
}

namespace OnFrenzyHandler {
bool Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    //prologue: push ebp; mov ebp,esp; sub esp,0x310 = 9 bytes
    if (!s_detour.WriteRelJump(kAddr_LimbCondition_HandleChange, Hook_LimbCondition_HandleChange, 9))
        return false;

    return true;
}
}
