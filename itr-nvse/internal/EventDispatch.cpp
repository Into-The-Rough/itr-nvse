//xNVSE event registration for ITR events
#include "NVSEPluginAPI.h"
#include "EventDispatch.h"
#include "globals.h"

NVSEEventManagerInterface* g_eventManagerInterface = nullptr;

namespace EventDispatch {
void InitEventManager(void* nvseInterface)
{
	auto* nvse = reinterpret_cast<NVSEInterface*>(nvseInterface);
	g_eventManagerInterface = reinterpret_cast<NVSEEventManagerInterface*>(
		nvse->QueryInterface(kInterface_EventManager));

	if (!g_eventManagerInterface)
		return;
}

static constexpr int kProbePriority = -9999;
static constexpr LONG kProbeRefreshFrames = 30;

bool ListenerProbe::Install()
{
	if (!g_eventManagerInterface ||
		!g_eventManagerInterface->SetNativeEventHandlerWithPriority ||
		!g_eventManagerInterface->RemoveNativeEventHandlerWithPriority ||
		g_pluginHandle == kPluginHandle_Invalid)
	{
		installed = false;
		InterlockedExchange(&hasListeners, TRUE);
		return false;
	}

	g_eventManagerInterface->RemoveNativeEventHandlerWithPriority(eventName, handler, kProbePriority);
	installed = g_eventManagerInterface->SetNativeEventHandlerWithPriority(
		eventName, handler, g_pluginHandle, handlerName, kProbePriority);
	InterlockedExchange(&refreshCounter, kProbeRefreshFrames);
	InterlockedExchange(&hasListeners, TRUE);
	return installed;
}

bool ListenerProbe::Refresh(bool force)
{
	if (!installed)
		Install();

	if (!installed || !g_eventManagerInterface || !g_eventManagerInterface->IsEventHandlerFirst) {
		InterlockedExchange(&hasListeners, TRUE);
		return true;
	}

	//increment returns the new value, <= keeps the old-value-vs-budget cadence
	if (!force && InterlockedIncrement(&refreshCounter) <= kProbeRefreshFrames)
		return hasListeners != 0;

	InterlockedExchange(&refreshCounter, 0);
	LONG listeners = g_eventManagerInterface->IsEventHandlerFirst(
		eventName, handler, kProbePriority,
		nullptr, 0, nullptr, 0, nullptr, 0) ? FALSE : TRUE;
	InterlockedExchange(&hasListeners, listeners);
	return listeners != 0;
}

bool DispatchConsoleCommand(const char* commandName, const char* fullCommand, TESObjectREFR* calleeRef)
{
	if (!g_eventManagerInterface) return false;
	return g_eventManagerInterface->DispatchEvent("ITR:OnConsoleCommand", nullptr,
		commandName, fullCommand, calleeRef);
}

static bool EventResultAsBool(const NVSEArrayVarInterface::Element& result)
{
	switch (result.GetType()) {
	case NVSEArrayVarInterface::Element::kType_Numeric: return result.num != 0.0;
	case NVSEArrayVarInterface::Element::kType_Form:    return result.form != nullptr;
	case NVSEArrayVarInterface::Element::kType_Array:   return result.arr != nullptr;
	case NVSEArrayVarInterface::Element::kType_String:  return result.str && result.str[0] != '\0';
	default: return false;
	}
}

bool DispatchShowOffPreActivate(TESObjectREFR* player, TESForm* baseForm, TESObjectREFR* invRef)
{
	if (!g_eventManagerInterface) return true;

	UInt32 shouldActivate = 1;

	auto resultCallback = [](NVSEArrayVarInterface::Element& result, void* shouldActivateAddr) -> bool
	{
		UInt32& shouldActivateRef = *static_cast<UInt32*>(shouldActivateAddr);
		if (shouldActivateRef && result.IsValid())
			shouldActivateRef = EventResultAsBool(result) ? 1 : 0;
		return true;
	};

	auto retn = g_eventManagerInterface->DispatchEventAlt(
		"ShowOff:OnPreActivateInventoryItem",
		resultCallback,
		&shouldActivate,
		player,
		baseForm,
		invRef,
		&shouldActivate,
		static_cast<UInt32>(0));

	UInt32 isSpecialActivation = 0;
	auto retnAlt = g_eventManagerInterface->DispatchEventAlt(
		"ShowOff:OnPreActivateInventoryItemAlt",
		resultCallback,
		&shouldActivate,
		player,
		baseForm,
		invRef,
		&shouldActivate,
		static_cast<UInt32>(0),
		isSpecialActivation);

	//unknown events never invoke the callback, only treat the result as authoritative if either event exists
	if (retn == NVSEEventManagerInterface::kRetn_UnknownEvent &&
		retnAlt == NVSEEventManagerInterface::kRetn_UnknownEvent)
		return true;

	return shouldActivate != 0;
}

void RegisterEvents()
{
	if (!g_eventManagerInterface) return;

	using P = NVSEEventManagerInterface::ParamType;
	using F = NVSEEventManagerInterface::EventFlags;

	//xNVSE stores the param array pointer, not a copy - must be static
	static P twoForms[] = { P::eParamType_AnyForm, P::eParamType_AnyForm };
	static P oneForm[] = { P::eParamType_AnyForm };
	static P stealParams[] = { P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_Int };

	g_eventManagerInterface->RegisterEvent("ITR:OnWeaponJam", 2, twoForms, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnWeaponDrop", 2, twoForms, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnFrenzy", 1, oneForm, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnSteal", 5, stealParams, F::kFlag_FlushOnLoad);

	static P witnessedParams[] = {
		P::eParamType_AnyForm,  //witness
		P::eParamType_AnyForm,  //perpetrator
		P::eParamType_Int,      //crimeType
		P::eParamType_AnyForm,  //victim / target
		P::eParamType_Int,      //detectionValue
	};
	g_eventManagerInterface->RegisterEvent("ITR:OnWitnessed", 5, witnessedParams, F::kFlag_FlushOnLoad);

	static P cornerParams[] = { P::eParamType_String, P::eParamType_Int, P::eParamType_String, P::eParamType_String, P::eParamType_Float, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnCornerMessage", 6, cornerParams, F::kFlag_FlushOnLoad);

	static P dialogueParams[] = { P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_String, P::eParamType_String, P::eParamType_Float };
	g_eventManagerInterface->RegisterEvent("ITR:OnDialogueText", 6, dialogueParams, F::kFlag_FlushOnLoad);

	static P oneInt[] = { P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnDoubleTap", 1, oneInt, F::kFlag_FlushOnLoad);

	static P keyHeldParams[] = { P::eParamType_Int, P::eParamType_Float };
	g_eventManagerInterface->RegisterEvent("ITR:OnKeyHeld", 2, keyHeldParams, F::kFlag_FlushOnLoad);

	static P combatProcParams[] = { P::eParamType_AnyForm, P::eParamType_Int, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnCombatProcedure", 3, combatProcParams, F::kFlag_FlushOnLoad);

	g_eventManagerInterface->RegisterEvent("ITR:OnConsoleOpen", 0, nullptr, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnConsoleClose", 0, nullptr, F::kFlag_FlushOnLoad);
	static P consoleCommandParams[] = { P::eParamType_String, P::eParamType_String, P::eParamType_AnyForm };
	g_eventManagerInterface->RegisterEvent("ITR:OnConsoleCommand", 3, consoleCommandParams, F::kFlag_FlushOnLoad);

	static P entryPointParams[] = { P::eParamType_AnyForm, P::eParamType_Int, P::eParamType_AnyForm, P::eParamType_AnyForm };
	g_eventManagerInterface->RegisterEvent("ITR:OnEntryPoint", 4, entryPointParams, F::kFlag_FlushOnLoad);

	static P landedParams[] = { P::eParamType_AnyForm, P::eParamType_Float };
	g_eventManagerInterface->RegisterEvent("ITR:OnActorLanded", 2, landedParams, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnJumpStart", 1, oneForm, F::kFlag_FlushOnLoad);

	static P twoInts[] = { P::eParamType_Int, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnKeyDisabled", 2, twoInts, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnKeyEnabled", 2, twoInts, F::kFlag_FlushOnLoad);

	static P fourInts[] = { P::eParamType_Int, P::eParamType_Int, P::eParamType_Int, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnMenuFilterChange", 4, fourInts, F::kFlag_FlushOnLoad);

	static P threeInts[] = { P::eParamType_Int, P::eParamType_Int, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnMenuSideChange", 3, threeInts, F::kFlag_FlushOnLoad);

	g_eventManagerInterface->RegisterEvent("ITR:OnMenuListRefresh", 1, oneInt, F::kFlag_FlushOnLoad);

	static P soundParams[] = { P::eParamType_String, P::eParamType_Int, P::eParamType_AnyForm };
	g_eventManagerInterface->RegisterEvent("ITR:OnSoundPlayed", 3, soundParams, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnSoundCompleted", 3, soundParams, F::kFlag_FlushOnLoad);

	static P contactParams[] = { P::eParamType_AnyForm, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnContactBegin", 2, contactParams, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnContactEnd", 2, contactParams, F::kFlag_FlushOnLoad);

	//impactData, x, y, z, normalX, normalY, normalZ, projectile, target, weapon, material
	static P impactSpawnParams[] = {
		P::eParamType_AnyForm,
		P::eParamType_Float, P::eParamType_Float, P::eParamType_Float,
		P::eParamType_Float, P::eParamType_Float, P::eParamType_Float,
		P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_AnyForm,
		P::eParamType_Int,
	};
	g_eventManagerInterface->RegisterEvent("ITR:OnImpactDataSpawn", 11, impactSpawnParams, F::kFlag_FlushOnLoad);

	//impactData, x, y, z, normalX, normalY, normalZ
	static P sprayDecalParams[] = {
		P::eParamType_AnyForm,
		P::eParamType_Float, P::eParamType_Float, P::eParamType_Float,
		P::eParamType_Float, P::eParamType_Float, P::eParamType_Float,
	};
	g_eventManagerInterface->RegisterEvent("ITR:OnSprayDecal", 7, sprayDecalParams, F::kFlag_FlushOnLoad);

	//actor, impactData, x, y, z, dx, dy, dz, hitLocation, source, weapon
	static P woundSprayParams[] = {
		P::eParamType_AnyForm, P::eParamType_AnyForm,
		P::eParamType_Float, P::eParamType_Float, P::eParamType_Float,
		P::eParamType_Float, P::eParamType_Float, P::eParamType_Float,
		P::eParamType_Int,
		P::eParamType_AnyForm, P::eParamType_AnyForm,
	};
	g_eventManagerInterface->RegisterEvent("ITR:OnWoundSpray", 11, woundSprayParams, F::kFlag_FlushOnLoad);

	//actor, shooter, weapon, distance
	static P nearMissParams[] = { P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_Float };
	g_eventManagerInterface->RegisterEvent("ITR:OnNearMiss", 4, nearMissParams, F::kFlag_FlushOnLoad);

	g_eventManagerInterface->RegisterEvent("ITR:OnCasinoBan", 1, oneForm, F::kFlag_FlushOnLoad);

	//target, parentForm, effectItemIndex, caster
	static P effectAppliedParams[] = {
		P::eParamType_AnyForm, P::eParamType_AnyForm, P::eParamType_Int, P::eParamType_AnyForm,
	};
	g_eventManagerInterface->RegisterEvent("ITR:OnEffectApplied", 4, effectAppliedParams, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnEffectRemoved", 4, effectAppliedParams, F::kFlag_FlushOnLoad);

	static P weatherParams[] = { P::eParamType_Int, P::eParamType_AnyForm, P::eParamType_AnyForm };
	g_eventManagerInterface->RegisterEvent("ITR:OnWeatherChange", 3, weatherParams, F::kFlag_FlushOnLoad);

	g_eventManagerInterface->RegisterEvent("ITR:OnVATSEnter", 1, oneForm, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnVATSLeave", 1, oneInt, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnKillCamStart", 1, oneForm, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnKillCamEnd", 1, oneForm, F::kFlag_FlushOnLoad);

	//actor, cause (knockdown: 1 force, 2 paralysis, 3 havok) / actor, phase (getup: 0 begin, 1 complete)
	static P actorIntParams[] = { P::eParamType_AnyForm, P::eParamType_Int };
	g_eventManagerInterface->RegisterEvent("ITR:OnKnockdown", 2, actorIntParams, F::kFlag_FlushOnLoad);
	g_eventManagerInterface->RegisterEvent("ITR:OnGetUp", 2, actorIntParams, F::kFlag_FlushOnLoad);
}

}
