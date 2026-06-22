#include "WeatherChangeEvent.h"

#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"

namespace {
	struct Sky {
		UInt8    pad00[0x10];
		TESForm* currWeather;   //0x10
		TESForm* transWeather;  //0x14 outgoing weather, non-null only during a blend
	};

	constexpr UInt32 kCallSite_UpdateWeather = 0x63ADCF; //sole caller, inside Sky::Update
	constexpr UInt32 kAddr_ForceWeather = 0x63D0E0;
	constexpr UInt32 kForcePrologueSize = 7; //push ebp, mov ebp esp, push ecx, mov [ebp-4] ecx

	enum { kPhase_Start = 0, kPhase_Complete = 1 };

	using UpdateWeather_t = void(__thiscall*)(Sky*);
	using ForceWeather_t = void(__thiscall*)(Sky*, TESForm*, char);

	Detours::CallDetour g_updateDetour;
	Detours::JumpDetour g_forceDetour;
	UpdateWeather_t g_origUpdate = nullptr;
	ForceWeather_t g_origForce = nullptr;

	void Dispatch(int phase, TESForm* next, TESForm* prev) {
		if (g_eventManagerInterface)
			g_eventManagerInterface->DispatchEvent("ITR:OnWeatherChange", nullptr, phase, next, prev);
	}

	void __fastcall UpdateWeatherHook(Sky* sky, void*) {
		TESForm* prevCurr = sky->currWeather;
		TESForm* prevTrans = sky->transWeather;
		g_origUpdate(sky);

		if (sky->currWeather != prevCurr) {
			Dispatch(kPhase_Start, sky->currWeather, prevCurr);
			if (!sky->transWeather)
				Dispatch(kPhase_Complete, sky->currWeather, prevCurr);
		}
		else if (prevTrans && !sky->transWeather) {
			Dispatch(kPhase_Complete, sky->currWeather, prevTrans);
		}
	}

	void __fastcall ForceWeatherHook(Sky* sky, void*, TESForm* weather, char flag) {
		TESForm* prevCurr = sky->currWeather;
		g_origForce(sky, weather, flag);
		if (sky->currWeather != prevCurr) {
			Dispatch(kPhase_Start, sky->currWeather, prevCurr);
			Dispatch(kPhase_Complete, sky->currWeather, prevCurr);
		}
	}
}

namespace WeatherChangeEvent {
	void Init() {
		if (g_updateDetour.WriteRelCall(kCallSite_UpdateWeather, UpdateWeatherHook))
			g_origUpdate = reinterpret_cast<UpdateWeather_t>(g_updateDetour.GetOverwrittenAddr());

		if (g_forceDetour.WriteRelJump(kAddr_ForceWeather, ForceWeatherHook, kForcePrologueSize))
			g_origForce = g_forceDetour.GetTrampoline<ForceWeather_t>();
	}
}
