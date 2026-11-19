#include "WeatherChangeEvent.h"

#include <cstddef>

#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"
#include "internal/GameLayout.h"
#include "internal/globals.h"

namespace {

	constexpr UInt32 kCallSite_UpdateWeather = 0x63ADCF; //Sky::Update -> UpdateWeather (0x63D1D0)
	constexpr UInt32 kForceWeatherCallSites[] = {0x5B764E, 0x5B76C1}; //Sky::ForceWeather (0x63D0E0)
	constexpr UInt32 kForceWeatherCallCount = sizeof(kForceWeatherCallSites) / sizeof(kForceWeatherCallSites[0]);

	enum {
		kPhase_Start = 0,
		kPhase_Complete = 1
	};

	using UpdateWeather_t = void(__thiscall*)(SkyView*);
	using ForceWeather_t = void(__thiscall*)(SkyView*, TESForm*, char);

	Detours::CallDetour g_updateDetour;
	Detours::CallDetour g_forceDetours[kForceWeatherCallCount];
	UInt32 g_dispatchDepth = 0;

	void Dispatch(int phase, TESForm* next, TESForm* prev)
	{
		if (!g_eventManagerInterface || g_dispatchDepth >= 16)
			return;
		g_dispatchDepth++;
		g_eventManagerInterface->DispatchEvent("ITR:OnWeatherChange", nullptr, phase, next, prev);
		g_dispatchDepth--;
	}

	void __fastcall Hook_UpdateWeather(SkyView* sky, void* edx)
	{
		TESForm* prevCurr = sky->currWeather;
		TESForm* prevTrans = sky->transWeather;
		auto original = reinterpret_cast<UpdateWeather_t>(g_updateDetour.GetOverwrittenAddr());
		original(sky);
		TESForm* next = sky->currWeather;
		TESForm* trans = sky->transWeather;

		if (next != prevCurr) {
			Dispatch(kPhase_Start, next, prevCurr);
			if (!trans)
				Dispatch(kPhase_Complete, next, prevCurr);
		} else if (prevTrans && !trans) {
			Dispatch(kPhase_Complete, next, prevTrans);
		}
	}

	template<UInt32 N>
	void __fastcall Hook_ForceWeather(SkyView* sky, void* edx, TESForm* weather, char flag)
	{
		TESForm* prevCurr = sky->currWeather;
		auto original = reinterpret_cast<ForceWeather_t>(g_forceDetours[N].GetOverwrittenAddr());
		original(sky, weather, flag);
		TESForm* next = sky->currWeather;
		if (next != prevCurr) {
			Dispatch(kPhase_Start, next, prevCurr);
			Dispatch(kPhase_Complete, next, prevCurr);
		}
	}
}

namespace WeatherChangeEvent {
	bool Init()
	{
		if (!g_updateDetour.WriteRelCall(kCallSite_UpdateWeather, Hook_UpdateWeather))
			return false;

		using ForceWeatherHook_t = void(__fastcall*)(SkyView*, void*, TESForm*, char);
		static const ForceWeatherHook_t hooks[kForceWeatherCallCount] = {
			Hook_ForceWeather<0>, Hook_ForceWeather<1>};

		for (UInt32 i = 0; i < kForceWeatherCallCount; i++) {
			if (!g_forceDetours[i].WriteRelCall(kForceWeatherCallSites[i], hooks[i]))
				return false;
		}

		return true;
	}
}
