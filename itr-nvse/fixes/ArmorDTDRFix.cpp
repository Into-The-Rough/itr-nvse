//npc armor DT/DR cache invalidation fix
//vanilla ResetArmorRating doesn't dirty the HighProcess CachedActorValues
//NOT hot-reloadable - requires game restart

#include "ArmorDTDRFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/GameLayout.h"

#include "internal/globals.h"
#include "internal/CallTemplates.h"

namespace ArmorDTDRFix
{
	//HighProcess::DirtyCachedActorValues
	constexpr uint32_t kAddr_DirtyCachedActorValues = 0x900780;

	typedef void(__thiscall* ResetArmorRating_t)(void*);

	static Detours::JumpDetour s_detour;
	static ResetArmorRating_t s_originalResetArmorRating = nullptr;

	void __fastcall Hook_ResetArmorRating(void* character, void* edx) {
		s_originalResetArmorRating(character); //trampoline is valid whenever this hook is live

		//baseProcess is uninitialized during Character::Character construction
		//refID is 0 until the form is fully created, skip cache dirtying for half-constructed actors
		auto* actor = reinterpret_cast<Actor*>(character);
		if (!actor->refID) return;

		BaseProcess* process = actor->baseProcess;
		if (process && process->processLevel == 0) {
			ThisCall(kAddr_DirtyCachedActorValues, process, 76); //kAV_DamageThreshold
			ThisCall(kAddr_DirtyCachedActorValues, process, 18); //kAV_DamageResistance
		}
	}

	//prologue: 7 bytes
	void Init() {
		if (s_detour.WriteRelJump(0x8D4E80, Hook_ResetArmorRating, 7)) { //Character::ResetArmorRating
			s_originalResetArmorRating = s_detour.GetTrampoline<ResetArmorRating_t>();
			if (!s_originalResetArmorRating) {
				Log("ArmorDTDRFix failed: trampoline not created");
				s_detour.Remove();
				return;
			}
		}
		else {
			Log("ArmorDTDRFix failed to install");
		}
	}
}
